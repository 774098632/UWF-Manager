/*
 * Copyright (c) 2026 HsingYun (iakext@gmail.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "EnhancedModeAgent.h"

#include <windows.h>

#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "../util/Log.h"
#include "EnhancedModePipeIo.h"
#include "EnhancedModeService.h"

namespace uwf::service {

namespace {

constexpr auto kReconnectInitialDelay = std::chrono::milliseconds{250};
constexpr auto kReconnectMaximumDelay = std::chrono::seconds{5};
constexpr auto kConnectionFailureGrace = std::chrono::seconds{1};
constexpr auto kHandshakeTimeout = std::chrono::seconds{2};
constexpr auto kHandshakePollInterval = std::chrono::milliseconds{25};
constexpr auto kControlAcknowledgementTimeout = std::chrono::seconds{2};

class UniqueHandle final {
 public:
  explicit UniqueHandle(HANDLE handle = nullptr) : m_handle(handle) {}
  ~UniqueHandle() {
    if (valid()) CloseHandle(m_handle);
  }
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;
  [[nodiscard]] HANDLE get() const { return m_handle; }
  [[nodiscard]] bool valid() const { return m_handle && m_handle != INVALID_HANDLE_VALUE; }

 private:
  HANDLE m_handle = nullptr;
};

std::wstring processImagePath(const HANDLE process) {
  std::wstring path(32768, L'\0');
  DWORD length = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) return {};
  path.resize(length);
  return path;
}

bool sameFile(const std::wstring& leftPath, const std::wstring& rightPath) {
  const DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  const UniqueHandle left(CreateFileW(leftPath.c_str(), FILE_READ_ATTRIBUTES, sharing, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  const UniqueHandle right(CreateFileW(rightPath.c_str(), FILE_READ_ATTRIBUTES, sharing, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!left.valid() || !right.valid()) return false;
  FILE_ID_INFO leftInfo{};
  FILE_ID_INFO rightInfo{};
  return GetFileInformationByHandleEx(left.get(), FileIdInfo, &leftInfo, sizeof(leftInfo)) &&
         GetFileInformationByHandleEx(right.get(), FileIdInfo, &rightInfo, sizeof(rightInfo)) && leftInfo.VolumeSerialNumber == rightInfo.VolumeSerialNumber &&
         std::equal(std::begin(leftInfo.FileId.Identifier), std::end(leftInfo.FileId.Identifier), std::begin(rightInfo.FileId.Identifier));
}

bool isLocalSystemServiceProcess(const DWORD processId) {
  if (processId == 0) return false;
  const UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
  if (!process.valid() || !sameFile(processImagePath(process.get()), processImagePath(GetCurrentProcess()))) return false;

  DWORD sessionId = std::numeric_limits<DWORD>::max();
  if (!ProcessIdToSessionId(processId, &sessionId) || sessionId != 0) return false;

  HANDLE rawToken = nullptr;
  if (!OpenProcessToken(process.get(), TOKEN_QUERY, &rawToken)) return false;
  const UniqueHandle token(rawToken);
  DWORD bytes = 0;
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
  if (bytes == 0) return false;
  std::vector<BYTE> user(bytes);
  if (!GetTokenInformation(token.get(), TokenUser, user.data(), bytes, &bytes)) return false;
  BYTE localSystem[SECURITY_MAX_SID_SIZE]{};
  DWORD sidSize = sizeof(localSystem);
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, localSystem, &sidSize)) return false;
  return EqualSid(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, localSystem) != FALSE;
}

bool readAgentHandshake(const HANDLE pipe, const HANDLE stopEvent, const std::stop_token stopToken) {
  std::array<char, kEnhancedAgentHandshake.size()> received{};
  std::size_t offset = 0;
  const auto deadline = std::chrono::steady_clock::now() + kHandshakeTimeout;
  while (offset < received.size() && !stopToken.stop_requested()) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
    if (available == 0) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(kHandshakePollInterval);
      continue;
    }

    const auto requested = static_cast<std::size_t>(std::min(available, static_cast<DWORD>(received.size() - offset)));
    auto remaining = std::as_writable_bytes(std::span{received}).subspan(offset, requested);
    if (readEnhancedPipe(pipe, remaining, stopEvent) != EnhancedPipeIoResult::Completed) return false;
    offset += requested;
  }
  return !stopToken.stop_requested() && received == kEnhancedAgentHandshake;
}

}  // namespace

struct EnhancedModeAgent::ControlChannel {
  ControlChannel() : stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!stopEvent.valid()) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "create enhanced mode agent stop event");
  }

  std::mutex stateMutex;
  std::mutex requestMutex;
  std::condition_variable changed;
  UniqueHandle stopEvent;
  HANDLE pipe = nullptr;
  std::uint64_t generation = 0;
  std::uint64_t nextRequestId = 1;
  std::optional<EnhancedAgentControlMessage> lastAcknowledgement;
};

EnhancedModeAgent::EnhancedModeAgent(QObject* parent) : EnhancedModeAgentConnection(parent), m_control(std::make_unique<ControlChannel>()) {}

EnhancedModeAgent::~EnhancedModeAgent() { stop(); }

void EnhancedModeAgent::start() {
  if (m_thread.joinable()) {
    if (m_workerActive.load(std::memory_order_acquire)) return;
    m_thread.join();
  }
  ResetEvent(m_control->stopEvent.get());
  m_workerActive.store(true, std::memory_order_release);
  try {
    m_thread = std::jthread([this](const std::stop_token stopToken) {
      const auto markStopped = qScopeGuard([this] { m_workerActive.store(false, std::memory_order_release); });
      try {
        run(stopToken);
      } catch (const std::exception& error) {
        UWF_LOG_E("service") << "enhanced mode identity agent stopped unexpectedly: error=" << error.what();
        if (!stopToken.stop_requested()) emit connectionStateChanged(false);
      } catch (...) {
        UWF_LOG_E("service") << "enhanced mode identity agent stopped unexpectedly: error=non-standard-exception";
        if (!stopToken.stop_requested()) emit connectionStateChanged(false);
      }
    });
  } catch (...) {
    m_workerActive.store(false, std::memory_order_release);
    throw;
  }
}

void EnhancedModeAgent::stop() {
  if (!m_thread.joinable()) return;
  m_thread.request_stop();
  SetEvent(m_control->stopEvent.get());
  m_waitChanged.notify_all();
  m_control->changed.notify_all();
  m_thread.join();
  m_workerActive.store(false, std::memory_order_release);
}

bool EnhancedModeAgent::running() const { return m_workerActive.load(std::memory_order_acquire); }

bool EnhancedModeAgent::markPreshutdownCommitHandled() { return sendControl(EnhancedAgentControl::PreshutdownCommitHandled); }

bool EnhancedModeAgent::requirePreshutdownCommit() { return sendControl(EnhancedAgentControl::PreshutdownCommitRequired); }

bool EnhancedModeAgent::sendControl(const EnhancedAgentControl control) {
  std::scoped_lock requestLock(m_control->requestMutex);
  HANDLE rawDuplicate = nullptr;
  std::uint64_t generation = 0;
  EnhancedAgentControlMessage request;
  {
    std::scoped_lock stateLock(m_control->stateMutex);
    if (!m_control->pipe) return false;
    if (!DuplicateHandle(GetCurrentProcess(), m_control->pipe, GetCurrentProcess(), &rawDuplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
    generation = m_control->generation;
    request.control = control;
    request.requestId = m_control->nextRequestId;
    m_control->nextRequestId = request.requestId == std::numeric_limits<std::uint64_t>::max() ? 1 : request.requestId + 1;
  }
  const UniqueHandle pipe(rawDuplicate);
  const auto frame = encodeEnhancedAgentControl(request);
  if (writeEnhancedPipe(pipe.get(), std::as_bytes(std::span{frame}), m_control->stopEvent.get()) != EnhancedPipeIoResult::Completed) return false;

  std::unique_lock stateLock(m_control->stateMutex);
  const bool acknowledged = m_control->changed.wait_for(stateLock, kControlAcknowledgementTimeout,
                                                        [&] { return m_control->generation != generation || m_control->lastAcknowledgement == request; });
  return acknowledged && m_control->generation == generation && m_control->lastAcknowledgement == request;
}

bool EnhancedModeAgent::waitBeforeReconnect(const std::stop_token stopToken, const std::chrono::milliseconds delay) {
  std::unique_lock lock(m_waitMutex);
  m_waitChanged.wait_for(lock, delay, [&] { return stopToken.stop_requested(); });
  return !stopToken.stop_requested();
}

void EnhancedModeAgent::run(const std::stop_token stopToken) {
  auto retryDelay = kReconnectInitialDelay;
  const auto failureDeadline = std::chrono::steady_clock::now() + kConnectionFailureGrace;
  bool disconnectedPublished = false;
  const auto publishConnectionFailure = [&] {
    if (!disconnectedPublished && std::chrono::steady_clock::now() >= failureDeadline) {
      disconnectedPublished = true;
      emit connectionStateChanged(false);
    }
  };
  const auto waitForRetry = [&]() {
    const bool keepRunning = waitBeforeReconnect(stopToken, retryDelay);
    publishConnectionFailure();
    retryDelay = std::min(retryDelay * 2, std::chrono::duration_cast<std::chrono::milliseconds>(kReconnectMaximumDelay));
    return keepRunning;
  };

  while (!stopToken.stop_requested()) {
    constexpr DWORD kSecurityFlags = SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION | SECURITY_EFFECTIVE_ONLY;
    UniqueHandle pipe(CreateFileW(kEnhancedPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, kSecurityFlags | FILE_FLAG_OVERLAPPED, nullptr));
    if (!pipe.valid()) {
      if (!waitForRetry()) return;
      continue;
    }

    ULONG serverProcessId = 0;
    if (!GetNamedPipeServerProcessId(pipe.get(), &serverProcessId) || !isLocalSystemServiceProcess(static_cast<DWORD>(serverProcessId))) {
      UWF_LOG_W("service") << "enhanced mode identity pipe rejected: reason=untrusted-server";
      if (!waitForRetry()) return;
      continue;
    }
    if (!readAgentHandshake(pipe.get(), m_control->stopEvent.get(), stopToken)) {
      if (!waitForRetry()) return;
      continue;
    }

    retryDelay = kReconnectInitialDelay;
    disconnectedPublished = false;
    {
      {
        std::scoped_lock lock(m_control->stateMutex);
        m_control->pipe = pipe.get();
        m_control->lastAcknowledgement.reset();
        ++m_control->generation;
      }
      m_control->changed.notify_all();
      const auto releasePublishedPipe = qScopeGuard([this, handle = pipe.get()] {
        {
          std::scoped_lock lock(m_control->stateMutex);
          if (m_control->pipe != handle) return;
          m_control->pipe = nullptr;
          m_control->lastAcknowledgement.reset();
          ++m_control->generation;
        }
        m_control->changed.notify_all();
      });
      emit connectionStateChanged(true);

      // 连接平时只维持身份；安全电源流程会在 UI 完成预提交后发送带请求 ID
      // 的控制帧，服务原样确认一次性 PRESHUTDOWN 令牌的状态。
      for (;;) {
        EnhancedAgentControlFrame frame{};
        if (readEnhancedPipe(pipe.get(), std::as_writable_bytes(std::span{frame}), m_control->stopEvent.get()) != EnhancedPipeIoResult::Completed) break;
        const auto acknowledgement = decodeEnhancedAgentControl(frame);
        if (!acknowledgement) {
          UWF_LOG_W("service") << "enhanced mode identity channel rejected invalid control acknowledgement";
          break;
        }
        {
          std::scoped_lock lock(m_control->stateMutex);
          m_control->lastAcknowledgement = *acknowledgement;
        }
        m_control->changed.notify_all();
      }
    }
    if (stopToken.stop_requested()) return;
    disconnectedPublished = true;
    emit connectionStateChanged(false);
    if (!waitForRetry()) return;
  }
}

}  // namespace uwf::service
