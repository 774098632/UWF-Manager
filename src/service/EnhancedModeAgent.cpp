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

#include <QByteArray>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../util/Log.h"
#include "EnhancedModeService.h"

namespace uwf::service {

namespace {

constexpr auto kReconnectInitialDelay = std::chrono::milliseconds{250};
constexpr auto kReconnectMaximumDelay = std::chrono::seconds{5};
constexpr auto kConnectionFailureGrace = std::chrono::seconds{1};
constexpr auto kHandshakeTimeout = std::chrono::seconds{2};
constexpr auto kHandshakePollInterval = std::chrono::milliseconds{25};

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

bool writeAll(const HANDLE pipe, const QByteArray& bytes) {
  qsizetype offset = 0;
  while (offset < bytes.size()) {
    DWORD written = 0;
    const DWORD requested = static_cast<DWORD>(bytes.size() - offset);
    if (!WriteFile(pipe, bytes.constData() + offset, requested, &written, nullptr) || written == 0) return false;
    offset += static_cast<qsizetype>(written);
  }
  return true;
}

bool readAgentHandshake(const HANDLE pipe, const std::stop_token stopToken) {
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

    DWORD count = 0;
    const DWORD requested = std::min(available, static_cast<DWORD>(received.size() - offset));
    if (!ReadFile(pipe, received.data() + offset, requested, &count, nullptr) || count == 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return !stopToken.stop_requested() && received == kEnhancedAgentHandshake;
}

std::optional<QByteArray> readFrame(const HANDLE pipe) {
  QByteArray bytes;
  std::array<char, 4096> chunk{};
  std::size_t expected = 0;
  for (;;) {
    DWORD count = 0;
    if (!ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &count, nullptr) || count == 0) return std::nullopt;
    bytes.append(chunk.data(), static_cast<qsizetype>(count));
    if (expected == 0) expected = app::applicationCommandFrameSize(bytes);
    if (expected != 0 && static_cast<std::size_t>(bytes.size()) >= expected) {
      if (static_cast<std::size_t>(bytes.size()) != expected) return std::nullopt;
      return bytes;
    }
  }
}

}  // namespace

EnhancedModeAgent::EnhancedModeAgent(QObject* parent) : EnhancedModeAgentConnection(parent) {}

EnhancedModeAgent::~EnhancedModeAgent() { stop(); }

void EnhancedModeAgent::start() {
  if (m_thread.joinable()) {
    if (m_workerActive.load(std::memory_order_acquire)) return;
    m_thread.join();
  }
  m_workerActive.store(true, std::memory_order_release);
  try {
    m_thread = std::jthread([this](const std::stop_token stopToken) {
      const auto markStopped = qScopeGuard([this] { m_workerActive.store(false, std::memory_order_release); });
      try {
        run(stopToken);
      } catch (const std::exception& error) {
        UWF_LOG_E("service") << "enhanced mode agent stopped unexpectedly: error=" << error.what();
        if (!stopToken.stop_requested()) emit connectionStateChanged(false);
      } catch (...) {
        UWF_LOG_E("service") << "enhanced mode agent stopped unexpectedly: error=non-standard-exception";
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
  m_responseReady.notify_all();
  CancelSynchronousIo(reinterpret_cast<HANDLE>(m_thread.native_handle()));
  m_thread.join();
  m_workerActive.store(false, std::memory_order_release);
  std::scoped_lock lock(m_mutex);
  m_activeRequestId.reset();
  m_response.reset();
  m_progress.reset();
}

bool EnhancedModeAgent::running() const { return m_workerActive.load(std::memory_order_acquire); }

bool EnhancedModeAgent::waitBeforeReconnect(const std::stop_token stopToken, const std::chrono::milliseconds delay) {
  std::unique_lock lock(m_mutex);
  m_responseReady.wait_for(lock, delay, [&] { return stopToken.stop_requested(); });
  return !stopToken.stop_requested();
}

void EnhancedModeAgent::complete(const std::uint64_t requestId, const app::ApplicationCommandResult& result) {
  {
    std::scoped_lock lock(m_mutex);
    if (m_activeRequestId != requestId) return;
    m_response = Response{requestId, result};
  }
  m_responseReady.notify_all();
}

void EnhancedModeAgent::reportProgress(const std::uint64_t requestId, const std::size_t processed, const std::size_t total) {
  {
    std::scoped_lock lock(m_mutex);
    if (m_activeRequestId != requestId) return;
    // 只保留尚未发送的最新进度；最终结果仍单独排队，绝不会被进度覆盖。
    m_progress = std::pair{requestId, app::ApplicationCommandProgress{processed, total}};
  }
  m_responseReady.notify_all();
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
    if (!WaitNamedPipeW(kEnhancedPipeName, 1000)) {
      if (!waitForRetry()) return;
      continue;
    }
    constexpr DWORD kSecurityFlags = SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION | SECURITY_EFFECTIVE_ONLY;
    UniqueHandle pipe(CreateFileW(kEnhancedPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, kSecurityFlags, nullptr));
    if (!pipe.valid()) {
      if (!waitForRetry()) return;
      continue;
    }

    ULONG serverProcessId = 0;
    if (!GetNamedPipeServerProcessId(pipe.get(), &serverProcessId) || !isLocalSystemServiceProcess(static_cast<DWORD>(serverProcessId))) {
      UWF_LOG_W("service") << "enhanced mode pipe rejected: reason=untrusted-server";
      if (!waitForRetry()) return;
      continue;
    }
    if (!readAgentHandshake(pipe.get(), stopToken)) {
      if (!waitForRetry()) return;
      continue;
    }
    retryDelay = kReconnectInitialDelay;
    disconnectedPublished = false;
    emit connectionStateChanged(true);

    while (!stopToken.stop_requested()) {
      try {
        const auto bytes = readFrame(pipe.get());
        if (!bytes) break;
        const auto request = app::decodeCommandRequest(*bytes);
        if (!request || request->kind != app::ApplicationCommandKind::CommitStage) break;

        {
          std::scoped_lock lock(m_mutex);
          m_activeRequestId = request->requestId;
          m_response.reset();
          m_progress.reset();
        }
        const auto releaseRequest = qScopeGuard([this, requestId = request->requestId] {
          std::scoped_lock lock(m_mutex);
          if (m_activeRequestId != requestId) return;
          m_activeRequestId.reset();
          m_response.reset();
          m_progress.reset();
        });
        emit commitStageRequested(request->requestId);

        bool requestCompleted = false;
        while (!stopToken.stop_requested() && !requestCompleted) {
          std::unique_lock lock(m_mutex);
          m_responseReady.wait(lock, [&] {
            return stopToken.stop_requested() || (m_response && m_response->requestId == request->requestId) ||
                   (m_progress && m_progress->first == request->requestId);
          });
          if (stopToken.stop_requested()) break;
          if (m_response && m_response->requestId == request->requestId) {
            const auto response = std::move(*m_response);
            m_response.reset();
            m_progress.reset();
            lock.unlock();
            requestCompleted = writeAll(pipe.get(), app::encodeCommandResult(response.requestId, response.result));
            if (!requestCompleted) break;
            continue;
          }
          const auto progress = std::move(*m_progress);
          m_progress.reset();
          lock.unlock();
          if (!writeAll(pipe.get(), app::encodeCommandProgress(progress.first, progress.second))) break;
        }
        if (!requestCompleted) break;
      } catch (const std::exception& error) {
        UWF_LOG_W("service") << "enhanced mode agent protocol failed: error=" << error.what();
        break;
      }
    }
    disconnectedPublished = true;
    emit connectionStateChanged(false);
    if (!waitForRetry()) return;
  }
}

}  // namespace uwf::service
