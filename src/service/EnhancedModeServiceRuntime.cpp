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
// MinGW 的 sddl.h 不会自行定义 WINADVAPI / WINBOOL，windows.h 必须保持在前。
// clang-format off
#include <windows.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>
// clang-format on

#include <QByteArray>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "../app/ApplicationCommand.h"
#include "../util/Log.h"
#include "EnhancedModeService.h"

namespace uwf::service {

namespace {

constexpr DWORD kServiceWaitHintMs = 60'000;
constexpr auto kServiceHeartbeatInterval = std::chrono::seconds{15};
constexpr auto kAgentStartupGrace = std::chrono::seconds{30};
constexpr auto kAgentRetryInitial = std::chrono::seconds{5};
constexpr auto kAgentRetryMaximum = std::chrono::seconds{60};
constexpr auto kOrchestrationRetryDelay = std::chrono::seconds{1};
// PRESHUTDOWN 不能成为永久关机屏障。五分钟约束“始终无法形成一次可用
// 代理请求”的基础设施故障；代理持续上报进度时，每条有效帧重新获得完整的
// 空闲窗口，但整个编排仍受独立的绝对上限约束。三十分钟远高于普通关机钩子
// 的默认窗口，同时保证损坏的代理或异常大的目录不能永久阻止系统关机。
constexpr auto kPreshutdownInfrastructureDeadline = std::chrono::minutes{5};
constexpr auto kAgentResponseIdleTimeout = std::chrono::minutes{5};
constexpr auto kPreshutdownMaximumDuration = std::chrono::minutes{30};
constexpr auto kPipePollInterval = std::chrono::milliseconds{100};

class UniqueHandle final {
 public:
  explicit UniqueHandle(HANDLE handle = nullptr) : m_handle(handle) {}
  ~UniqueHandle() { reset(); }
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;
  UniqueHandle(UniqueHandle&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this == &other) return *this;
    reset();
    m_handle = std::exchange(other.m_handle, nullptr);
    return *this;
  }

  void reset(HANDLE handle = nullptr) {
    if (valid()) CloseHandle(m_handle);
    m_handle = handle;
  }
  [[nodiscard]] HANDLE get() const { return m_handle; }
  [[nodiscard]] bool valid() const { return m_handle && m_handle != INVALID_HANDLE_VALUE; }

 private:
  HANDLE m_handle = nullptr;
};

class LocalMemory final {
 public:
  ~LocalMemory() {
    if (m_memory) LocalFree(m_memory);
  }
  [[nodiscard]] void** put() { return &m_memory; }
  [[nodiscard]] void* get() const { return m_memory; }

 private:
  void* m_memory = nullptr;
};

class EnvironmentBlock final {
 public:
  explicit EnvironmentBlock(const HANDLE token) {
    if (!CreateEnvironmentBlock(&m_environment, token, FALSE)) {
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "create user environment");
    }
  }
  ~EnvironmentBlock() { DestroyEnvironmentBlock(m_environment); }
  [[nodiscard]] void* get() const { return m_environment; }

 private:
  void* m_environment = nullptr;
};

[[noreturn]] void throwSystemError(const char* operation, const DWORD error = GetLastError()) {
  throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

std::wstring executablePath() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || static_cast<std::size_t>(length) >= path.size()) throwSystemError("read service executable path");
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

std::wstring processImagePath(const HANDLE process) {
  std::wstring path(32768, L'\0');
  DWORD length = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) return {};
  path.resize(length);
  return path;
}

struct AuthenticatedAgent {
  DWORD processId = 0;
  DWORD sessionId = 0;
  UniqueHandle process;
};

bool tokenIsElevatedAdministrator(const HANDLE token) {
  TOKEN_ELEVATION elevation{};
  DWORD bytes = sizeof(elevation);
  if (!GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes) || elevation.TokenIsElevated == 0) return false;

  BYTE administrators[SECURITY_MAX_SID_SIZE]{};
  DWORD sidSize = sizeof(administrators);
  if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &sidSize)) return false;

  // OpenProcessToken、WTSQueryUserToken 和 DuplicateTokenEx(TokenPrimary)
  // 在这里提供的都是主令牌；CheckTokenMembership 显式接收的令牌却必须是
  // 模拟令牌。直接读取 TokenGroups 可按令牌本来的类型完成同一项判断，也
  // 避免为了查询组成员关系再申请 TOKEN_DUPLICATE 权限。
  bytes = 0;
  GetTokenInformation(token, TokenGroups, nullptr, 0, &bytes);
  if (bytes == 0) return false;
  std::vector<BYTE> groupBuffer(bytes);
  if (!GetTokenInformation(token, TokenGroups, groupBuffer.data(), bytes, &bytes)) return false;

  const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(groupBuffer.data());
  for (DWORD index = 0; index < groups->GroupCount; ++index) {
    const auto& group = groups->Groups[index];
    const bool enabled = (group.Attributes & SE_GROUP_ENABLED) != 0;
    const bool denyOnly = (group.Attributes & SE_GROUP_USE_FOR_DENY_ONLY) != 0;
    if (enabled && !denyOnly && EqualSid(group.Sid, administrators)) return true;
  }
  return false;
}

bool sessionChangeRequiresAgent(const DWORD eventType) {
  // 登录和重新连接会话表示出现了新的交互桌面；锁定/解锁等状态变化不能
  // 重新拉起用户已经主动退出的 UI。
  return eventType == WTS_SESSION_LOGON || eventType == WTS_CONSOLE_CONNECT || eventType == WTS_REMOTE_CONNECT;
}

DWORD activeInteractiveSessionId() {
  const DWORD console = WTSGetActiveConsoleSessionId();
  PWTS_SESSION_INFOW sessions = nullptr;
  DWORD count = 0;
  if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
    return console != 0 ? console : std::numeric_limits<DWORD>::max();
  }
  const auto release = std::unique_ptr<WTS_SESSION_INFOW, decltype(&WTSFreeMemory)>(sessions, &WTSFreeMemory);
  DWORD firstActive = std::numeric_limits<DWORD>::max();
  for (DWORD index = 0; index < count; ++index) {
    const auto& session = sessions[index];
    if (session.SessionId == 0 || session.State != WTSActive) continue;
    if (session.SessionId == console) return session.SessionId;
    if (firstActive == std::numeric_limits<DWORD>::max()) firstActive = session.SessionId;
  }
  return firstActive;
}

std::optional<AuthenticatedAgent> authenticateAgent(const HANDLE pipe) {
  ULONG processId = 0;
  if (!GetNamedPipeClientProcessId(pipe, &processId) || processId == 0) return std::nullopt;
  UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(processId)));
  if (!process.valid() || !sameFile(processImagePath(process.get()), executablePath())) return std::nullopt;

  DWORD sessionId = 0;
  if (!ProcessIdToSessionId(static_cast<DWORD>(processId), &sessionId) || sessionId == 0 || sessionId != activeInteractiveSessionId()) {
    return std::nullopt;
  }
  HANDLE rawToken = nullptr;
  if (!OpenProcessToken(process.get(), TOKEN_QUERY, &rawToken)) return std::nullopt;
  const UniqueHandle token(rawToken);
  return tokenIsElevatedAdministrator(token.get()) ? std::optional{AuthenticatedAgent{static_cast<DWORD>(processId), sessionId, std::move(process)}}
                                                   : std::nullopt;
}

void enablePrivilege(const wchar_t* name) {
  HANDLE rawToken = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawToken)) throwSystemError("open service process token");
  const UniqueHandle token(rawToken);
  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount = 1;
  if (!LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid)) throwSystemError("resolve service privilege");
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  SetLastError(ERROR_SUCCESS);
  if (!AdjustTokenPrivileges(token.get(), FALSE, &privileges, 0, nullptr, nullptr) || GetLastError() != ERROR_SUCCESS) {
    throwSystemError("enable service privilege");
  }
}

UniqueHandle elevatedInteractiveToken(const DWORD sessionId) {
  HANDLE rawUserToken = nullptr;
  if (!WTSQueryUserToken(sessionId, &rawUserToken)) throwSystemError("query interactive user token");
  UniqueHandle userToken(rawUserToken);

  TOKEN_ELEVATION_TYPE elevationType = TokenElevationTypeDefault;
  DWORD bytes = sizeof(elevationType);
  if (!GetTokenInformation(userToken.get(), TokenElevationType, &elevationType, sizeof(elevationType), &bytes)) {
    throwSystemError("read interactive token elevation type");
  }

  HANDLE sourceToken = userToken.get();
  UniqueHandle linkedToken;
  if (elevationType == TokenElevationTypeLimited) {
    TOKEN_LINKED_TOKEN linked{};
    bytes = sizeof(linked);
    if (!GetTokenInformation(userToken.get(), TokenLinkedToken, &linked, sizeof(linked), &bytes)) {
      throwSystemError("read linked administrator token");
    }
    linkedToken.reset(linked.LinkedToken);
    sourceToken = linkedToken.get();
  }

  HANDLE duplicated = nullptr;
  if (!DuplicateTokenEx(sourceToken, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &duplicated)) {
    throwSystemError("duplicate interactive token");
  }
  UniqueHandle elevatedToken(duplicated);
  if (!tokenIsElevatedAdministrator(elevatedToken.get())) {
    throw std::runtime_error("the active session has no elevated administrator token");
  }
  DWORD targetSessionId = sessionId;
  if (!SetTokenInformation(elevatedToken.get(), TokenSessionId, &targetSessionId, sizeof(targetSessionId))) {
    throwSystemError("assign interactive token session");
  }
  return elevatedToken;
}

struct LaunchedAgent {
  UniqueHandle process;
  DWORD processId = 0;
  DWORD sessionId = 0;
};

LaunchedAgent launchInteractiveAgent() {
  const DWORD sessionId = activeInteractiveSessionId();
  if (sessionId == 0 || sessionId == std::numeric_limits<DWORD>::max()) {
    throw std::runtime_error("no active interactive session is available");
  }

  enablePrivilege(SE_TCB_NAME);
  enablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
  enablePrivilege(SE_INCREASE_QUOTA_NAME);
  auto token = elevatedInteractiveToken(sessionId);
  EnvironmentBlock environment(token.get());
  const std::wstring image = executablePath();
  std::wstring commandLine = L"\"" + image + L"\" --quiet";

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  wchar_t desktop[] = L"winsta0\\default";
  startup.lpDesktop = desktop;
  PROCESS_INFORMATION process{};
  if (!CreateProcessAsUserW(token.get(), image.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP,
                            environment.get(), nullptr, &startup, &process)) {
    throwSystemError("launch elevated UWF Manager agent");
  }
  UniqueHandle processHandle(process.hProcess);
  const UniqueHandle threadHandle(process.hThread);
  return {std::move(processHandle), process.dwProcessId, sessionId};
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

class PipeFrameReader final {
 public:
  [[nodiscard]] std::optional<QByteArray> read(const HANDLE pipe, const HANDLE process, const HANDLE released, const HANDLE stopEvent,
                                               const std::chrono::steady_clock::time_point deadline) {
    for (;;) {
      const std::size_t expected = app::applicationCommandFrameSize(m_buffer);
      if (expected != 0 && static_cast<std::size_t>(m_buffer.size()) >= expected) {
        QByteArray frame = m_buffer.left(static_cast<qsizetype>(expected));
        m_buffer.remove(0, static_cast<qsizetype>(expected));
        return frame;
      }

      if (std::chrono::steady_clock::now() >= deadline) {
        throw std::runtime_error("enhanced mode UI agent response timed out");
      }

      DWORD available = 0;
      if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return std::nullopt;
      if (available == 0) {
        const HANDLE events[] = {stopEvent, process, released};
        const DWORD wait = WaitForMultipleObjects(3, events, FALSE, static_cast<DWORD>(kPipePollInterval.count()));
        if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1 || wait == WAIT_OBJECT_0 + 2) return std::nullopt;
        if (wait == WAIT_TIMEOUT) continue;
        throwSystemError("wait for enhanced mode agent response");
      }

      std::array<char, 4096> chunk{};
      DWORD count = 0;
      const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(chunk.size()));
      if (!ReadFile(pipe, chunk.data(), requested, &count, nullptr) || count == 0) return std::nullopt;
      m_buffer.append(chunk.data(), static_cast<qsizetype>(count));
    }
  }

 private:
  QByteArray m_buffer;
};

struct AgentConnection {
  AgentConnection(UniqueHandle pipeHandle, AuthenticatedAgent identity)
      : pipe(std::move(pipeHandle)),
        processId(identity.processId),
        sessionId(identity.sessionId),
        process(std::move(identity.process)),
        released(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!released.valid()) throwSystemError("create enhanced mode agent release event");
  }
  UniqueHandle pipe;
  DWORD processId = 0;
  DWORD sessionId = 0;
  UniqueHandle process;
  UniqueHandle released;
};

enum class AgentLaunchObservation {
  Connected,
  ProcessExited,
  TimedOut,
  SessionChanged,
  ServiceStopping,
};

enum class AgentSessionScope {
  ActiveInteractive,
  AnyAuthenticated,
};

struct AgentLaunchResult {
  AgentLaunchObservation observation = AgentLaunchObservation::TimedOut;
  DWORD processId = 0;
  DWORD exitCode = STILL_ACTIVE;
};

class ServiceRuntime final {
 public:
  static ServiceRuntime& instance() {
    static ServiceRuntime runtime;
    return runtime;
  }

  void run() {
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kEnhancedServiceName), &ServiceRuntime::serviceMainThunk}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) throwSystemError("start service control dispatcher");
  }

 private:
  static void WINAPI serviceMainThunk(DWORD, LPWSTR*) {
    try {
      instance().serviceMain();
    } catch (...) {
      // ERROR_SERVICE_SPECIFIC_ERROR 要求同时提供非零的
      // dwServiceSpecificExitCode；这里没有可对外承诺的私有错误码，使用通用
      // Win32 失败状态，避免向 SCM 发布内部不一致的停止记录。
      instance().reportStopped(ERROR_GEN_FAILURE);
    }
  }

  static DWORD WINAPI handlerThunk(const DWORD control, const DWORD eventType, void*, void* context) {
    try {
      return static_cast<ServiceRuntime*>(context)->handleControl(control, eventType);
    } catch (...) {
      return ERROR_EXCEPTION_IN_SERVICE;
    }
  }

  void serviceMain() {
    m_statusHandle = RegisterServiceCtrlHandlerExW(kEnhancedServiceName, &ServiceRuntime::handlerThunk, this);
    if (!m_statusHandle) throwSystemError("register enhanced mode service handler");
    report(SERVICE_START_PENDING, 0, 1, kServiceWaitHintMs);
    try {
      m_stopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      m_preshutdownEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      m_agentNeededEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
      m_agentConnectedEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      if (!m_stopEvent.valid() || !m_preshutdownEvent.valid() || !m_agentNeededEvent.valid() || !m_agentConnectedEvent.valid()) {
        throwSystemError("create enhanced mode service events");
      }

      m_acceptThread =
          std::jthread([this](const std::stop_token stopToken) { runInfrastructureThread("enhanced mode agent listener", [&] { acceptAgents(stopToken); }); });
      m_supervisorThread = std::jthread(
          [this](const std::stop_token stopToken) { runInfrastructureThread("enhanced mode agent supervisor", [&] { superviseAgent(stopToken); }); });
      SetEvent(m_agentNeededEvent.get());
      report(SERVICE_RUNNING, SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE, 0, 0);

      const HANDLE events[] = {m_stopEvent.get(), m_preshutdownEvent.get()};
      const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
      if (wait == WAIT_OBJECT_0 + 1) runPreshutdownCommit();
      if (wait != WAIT_OBJECT_0 && wait != WAIT_OBJECT_0 + 1) throwSystemError("wait for enhanced mode service shutdown");
      stopInfrastructure();
      reportStopped(m_stopExitCode.load(std::memory_order_acquire));
    } catch (...) {
      if (m_stopEvent.valid()) SetEvent(m_stopEvent.get());
      stopInfrastructure();
      throw;
    }
  }

  DWORD handleControl(const DWORD control, const DWORD eventType) {
    switch (control) {
      case SERVICE_CONTROL_STOP:
        if (!m_preshutdownStarted.load(std::memory_order_acquire)) {
          report(SERVICE_STOP_PENDING, 0, 1, kServiceWaitHintMs);
          SetEvent(m_stopEvent.get());
        }
        return NO_ERROR;
      case SERVICE_CONTROL_PRESHUTDOWN: {
        bool expected = false;
        if (!m_preshutdownStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return NO_ERROR;
        try {
          report(SERVICE_STOP_PENDING, 0, nextCheckpoint(), kServiceWaitHintMs);
          if (!SetEvent(m_preshutdownEvent.get())) throwSystemError("request enhanced mode preshutdown");
        } catch (...) {
          m_preshutdownStarted.store(false, std::memory_order_release);
          throw;
        }
        return NO_ERROR;
      }
      case SERVICE_CONTROL_SESSIONCHANGE:
        if (sessionChangeRequiresAgent(eventType)) SetEvent(m_agentNeededEvent.get());
        return NO_ERROR;
      case SERVICE_CONTROL_INTERROGATE:
        publishCurrentStatus();
        return NO_ERROR;
      default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
  }

  template <typename Work>
  void runInfrastructureThread(const char* operation, Work&& work) noexcept {
    try {
      std::forward<Work>(work)();
    } catch (const std::system_error& error) {
      const auto code = static_cast<DWORD>(error.code().value());
      stopForInfrastructureFailure(operation, code, error.what());
    } catch (const std::exception& error) {
      stopForInfrastructureFailure(operation, ERROR_GEN_FAILURE, error.what());
    } catch (...) {
      stopForInfrastructureFailure(operation, ERROR_GEN_FAILURE, "non-standard-exception");
    }
  }

  void report(const DWORD state, const DWORD controls, const DWORD checkpoint, const DWORD waitHint) {
    std::scoped_lock lock(m_statusMutex);
    m_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    m_status.dwCurrentState = state;
    m_status.dwControlsAccepted = controls;
    m_status.dwWin32ExitCode = NO_ERROR;
    m_status.dwServiceSpecificExitCode = 0;
    m_status.dwCheckPoint = checkpoint;
    m_status.dwWaitHint = waitHint;
    if (m_statusHandle && !SetServiceStatus(m_statusHandle, &m_status)) throwSystemError("publish enhanced mode service status");
  }

  void publishCurrentStatus() {
    std::scoped_lock lock(m_statusMutex);
    if (m_statusHandle && !SetServiceStatus(m_statusHandle, &m_status)) throwSystemError("republish enhanced mode service status");
  }

  void reportStopped(const DWORD exitCode) noexcept {
    std::scoped_lock lock(m_statusMutex);
    m_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    m_status.dwCurrentState = SERVICE_STOPPED;
    m_status.dwControlsAccepted = 0;
    m_status.dwWin32ExitCode = exitCode;
    m_status.dwServiceSpecificExitCode = 0;
    m_status.dwCheckPoint = 0;
    m_status.dwWaitHint = 0;
    if (m_statusHandle) SetServiceStatus(m_statusHandle, &m_status);
  }

  DWORD nextCheckpoint() { return m_checkpoint.fetch_add(1, std::memory_order_relaxed) + 1; }

  void reportRealProgress() { report(SERVICE_STOP_PENDING, 0, nextCheckpoint(), kServiceWaitHintMs); }

  void acceptAgents(const std::stop_token stopToken) {
    LocalMemory descriptor;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, descriptor.put(), nullptr)) {
      stopForInfrastructureFailure("create enhanced mode pipe security descriptor", GetLastError());
      return;
    }
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor.get(), FALSE};

    while (!stopToken.stop_requested()) {
      UniqueHandle pipe(CreateNamedPipeW(kEnhancedPipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                         64 * 1024, 64 * 1024, 0, &security));
      if (!pipe.valid()) {
        stopForInfrastructureFailure("create enhanced mode agent pipe", GetLastError());
        return;
      }

      const BOOL connected = ConnectNamedPipe(pipe.get(), nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
      if (!connected) {
        const DWORD error = GetLastError();
        if (stopToken.stop_requested()) return;
        if (error == ERROR_NO_DATA) continue;
        stopForInfrastructureFailure("accept enhanced mode agent connection", error);
        return;
      }
      if (stopToken.stop_requested()) return;
      auto identity = authenticateAgent(pipe.get());
      if (!identity) {
        DisconnectNamedPipe(pipe.get());
        continue;
      }
      const QByteArray handshake(kEnhancedAgentHandshake.data(), static_cast<qsizetype>(kEnhancedAgentHandshake.size()));
      if (!writeAll(pipe.get(), handshake)) {
        DisconnectNamedPipe(pipe.get());
        continue;
      }

      auto connection = std::make_shared<AgentConnection>(std::move(pipe), std::move(*identity));
      {
        std::scoped_lock lock(m_connectionMutex);
        m_connection = connection;
        SetEvent(m_agentConnectedEvent.get());
      }
      m_connectionChanged.notify_all();

      // 不轮询 UI 进程。进程句柄在退出时由内核置为有信号；released 则覆盖
      // 协议失败主动丢弃连接的场景。任一事件都立即释放旧实例并重新创建监听，
      // 让用户关闭 UI 后再次启动的新进程可以重新完成认证。
      const HANDLE events[] = {m_stopEvent.get(), connection->process.get(), connection->released.get()};
      const DWORD wait = WaitForMultipleObjects(3, events, FALSE, INFINITE);
      if (wait == WAIT_OBJECT_0) return;
      if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_OBJECT_0 + 2) {
        discardConnection(connection);
        continue;
      }
      stopForInfrastructureFailure("wait for enhanced mode agent lifetime", GetLastError());
      return;
    }
  }

  void superviseAgent(const std::stop_token stopToken) {
    auto retryDelay = kAgentRetryInitial;
    bool delayBeforeLaunch = false;

    while (!stopToken.stop_requested()) {
      const auto sessionScope =
          m_preshutdownStarted.load(std::memory_order_acquire) ? AgentSessionScope::AnyAuthenticated : AgentSessionScope::ActiveInteractive;
      if (retainHealthyAgent(sessionScope)) {
        retryDelay = kAgentRetryInitial;
        delayBeforeLaunch = false;
        const HANDLE events[] = {m_stopEvent.get(), m_agentNeededEvent.get()};
        // 正常运行期间不轮询管道。用户主动关闭 UI 后，服务保留这个选择；
        // 仅新的交互会话事件或 PRESHUTDOWN 会唤醒监督线程重新评估。
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) return;
        if (wait != WAIT_OBJECT_0 + 1) {
          stopForInfrastructureFailure("wait for enhanced mode agent request", GetLastError());
          return;
        }
        continue;
      }

      if (delayBeforeLaunch) {
        const HANDLE events[] = {m_stopEvent.get(), m_agentNeededEvent.get(), m_agentConnectedEvent.get()};
        const DWORD wait = WaitForMultipleObjects(3, events, FALSE, static_cast<DWORD>(std::chrono::milliseconds{retryDelay}.count()));
        if (wait == WAIT_OBJECT_0) return;
        if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_OBJECT_0 + 2) {
          delayBeforeLaunch = false;
          continue;
        }
        if (wait != WAIT_TIMEOUT) {
          stopForInfrastructureFailure("wait for enhanced mode agent retry", GetLastError());
          return;
        }
        retryDelay = std::min(retryDelay * 2, kAgentRetryMaximum);
      }

      try {
        auto launched = launchInteractiveAgent();
        UWF_LOG_I("service") << "enhanced mode UI launch started: pid=" << launched.processId << " session=" << launched.sessionId;
        const auto result = observeLaunchedAgent(launched);
        switch (result.observation) {
          case AgentLaunchObservation::Connected:
            UWF_LOG_I("service") << "enhanced mode UI launch confirmed: launchedPid=" << launched.processId << " connectedPid=" << result.processId;
            retryDelay = kAgentRetryInitial;
            delayBeforeLaunch = false;
            continue;
          case AgentLaunchObservation::ServiceStopping:
            return;
          case AgentLaunchObservation::SessionChanged:
            UWF_LOG_I("service") << "enhanced mode UI launch superseded by active-session change: pid=" << launched.processId;
            retryDelay = kAgentRetryInitial;
            delayBeforeLaunch = false;
            continue;
          case AgentLaunchObservation::ProcessExited:
            UWF_LOG_W("service") << "enhanced mode UI exited before agent authentication: pid=" << launched.processId << " exitCode=" << result.exitCode;
            break;
          case AgentLaunchObservation::TimedOut:
            UWF_LOG_W("service") << "enhanced mode UI did not authenticate before timeout: pid=" << launched.processId;
            break;
        }
      } catch (const std::exception& error) {
        UWF_LOG_W("service") << "enhanced mode UI launch failed: error=" << error.what();
      } catch (...) {
        UWF_LOG_W("service") << "enhanced mode UI launch failed: error=non-standard-exception";
      }
      delayBeforeLaunch = true;
    }
  }

  bool retainHealthyAgent(const AgentSessionScope sessionScope) {
    std::shared_ptr<AgentConnection> connection;
    {
      std::scoped_lock lock(m_connectionMutex);
      connection = m_connection;
    }
    if (!connection) return false;
    DWORD available = 0;
    const bool sessionMatches = sessionScope == AgentSessionScope::AnyAuthenticated || connection->sessionId == activeInteractiveSessionId();
    const bool processAlive = WaitForSingleObject(connection->process.get(), 0) == WAIT_TIMEOUT;
    if (processAlive && sessionMatches && PeekNamedPipe(connection->pipe.get(), nullptr, 0, nullptr, &available, nullptr)) return true;
    discardConnection(connection);
    return false;
  }

  AgentLaunchResult observeLaunchedAgent(const LaunchedAgent& launched) {
    const auto deadline = std::chrono::steady_clock::now() + kAgentStartupGrace;
    for (;;) {
      const auto sessionScope =
          m_preshutdownStarted.load(std::memory_order_acquire) ? AgentSessionScope::AnyAuthenticated : AgentSessionScope::ActiveInteractive;
      if (auto connection = currentConnection(); connection && retainHealthyAgent(sessionScope)) {
        return {AgentLaunchObservation::Connected, connection->processId, STILL_ACTIVE};
      }

      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return {AgentLaunchObservation::TimedOut, launched.processId, STILL_ACTIVE};
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      const auto timeout = static_cast<DWORD>(std::min<std::int64_t>(remaining.count(), std::numeric_limits<DWORD>::max()));
      const HANDLE events[] = {m_stopEvent.get(), m_agentNeededEvent.get(), m_agentConnectedEvent.get(), launched.process.get()};
      const DWORD wait = WaitForMultipleObjects(4, events, FALSE, timeout);
      if (wait == WAIT_OBJECT_0) return {AgentLaunchObservation::ServiceStopping, launched.processId, STILL_ACTIVE};
      if (wait == WAIT_OBJECT_0 + 1) {
        if (activeInteractiveSessionId() != launched.sessionId) {
          return {AgentLaunchObservation::SessionChanged, launched.processId, STILL_ACTIVE};
        }
        continue;
      }
      if (wait == WAIT_OBJECT_0 + 2) continue;
      if (wait == WAIT_OBJECT_0 + 3) {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(launched.process.get(), &exitCode)) exitCode = GetLastError();
        return {AgentLaunchObservation::ProcessExited, launched.processId, exitCode};
      }
      if (wait == WAIT_TIMEOUT) return {AgentLaunchObservation::TimedOut, launched.processId, STILL_ACTIVE};
      throwSystemError("wait for enhanced mode UI authentication");
    }
  }

  std::shared_ptr<AgentConnection> currentConnection() {
    std::scoped_lock lock(m_connectionMutex);
    return m_connection;
  }

  void stopForInfrastructureFailure(const char* operation, const DWORD error, const std::string_view detail = {}) noexcept {
    const DWORD exitCode = error == NO_ERROR ? ERROR_GEN_FAILURE : error;
    DWORD expected = NO_ERROR;
    if (m_stopExitCode.compare_exchange_strong(expected, exitCode, std::memory_order_acq_rel)) {
      try {
        if (detail.empty())
          UWF_LOG_E("service") << operation << " failed: error=" << exitCode;
        else
          UWF_LOG_E("service") << operation << " failed: error=" << detail;
      } catch (...) {
      }
    }
    if (m_stopEvent.valid()) SetEvent(m_stopEvent.get());
  }

  std::shared_ptr<AgentConnection> waitForAgent(const std::chrono::steady_clock::duration grace) {
    // PRESHUTDOWN 期间活动会话可能先于服务收尾发生切换。已经通过认证且管道
    // 仍健康的代理必须保留到批次结束，不能因为会话状态抖动中途拆掉它。
    if (retainHealthyAgent(AgentSessionScope::AnyAuthenticated)) return currentConnection();
    SetEvent(m_agentNeededEvent.get());
    std::unique_lock lock(m_connectionMutex);
    m_connectionChanged.wait_for(lock, grace, [&] { return static_cast<bool>(m_connection); });
    return m_connection;
  }

  void discardConnection(const std::shared_ptr<AgentConnection>& connection) {
    std::scoped_lock lock(m_connectionMutex);
    if (m_connection == connection) {
      DisconnectNamedPipe(connection->pipe.get());
      m_connection.reset();
      ResetEvent(m_agentConnectedEvent.get());
      SetEvent(connection->released.get());
      m_connectionChanged.notify_all();
    }
  }

  [[nodiscard]] bool stopRequested() const {
    const DWORD wait = WaitForSingleObject(m_stopEvent.get(), 0);
    if (wait == WAIT_OBJECT_0) return true;
    if (wait == WAIT_TIMEOUT) return false;
    throwSystemError("inspect enhanced mode service stop event");
  }

  [[nodiscard]] bool waitForRetryOrStop(const std::chrono::steady_clock::duration duration) const {
    const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(duration);
    const auto timeout = static_cast<DWORD>(std::min<std::int64_t>(milliseconds.count(), std::numeric_limits<DWORD>::max()));
    const DWORD wait = WaitForSingleObject(m_stopEvent.get(), timeout);
    if (wait == WAIT_OBJECT_0) return true;
    if (wait == WAIT_TIMEOUT) return false;
    throwSystemError("wait for enhanced mode orchestration retry");
  }

  app::ApplicationCommandResult requestStagedCommit() {
    const auto startedAt = std::chrono::steady_clock::now();
    const auto infrastructureDeadline = startedAt + kPreshutdownInfrastructureDeadline;
    const auto absoluteDeadline = startedAt + kPreshutdownMaximumDuration;
    std::uint64_t attempt = 0;
    QString lastFailure = QStringLiteral("no authenticated UI agent became available");
    while (std::chrono::steady_clock::now() < std::min(infrastructureDeadline, absoluteDeadline)) {
      if (stopRequested()) {
        return app::ApplicationCommandResult::releasedPreshutdown(
            QStringLiteral("automatic file staging was released because the enhanced mode service infrastructure stopped"));
      }
      ++attempt;
      reportRealProgress();
      const auto remaining = std::min(infrastructureDeadline, absoluteDeadline) - std::chrono::steady_clock::now();
      auto connection = waitForAgent(std::min<std::chrono::steady_clock::duration>(kAgentStartupGrace, remaining));
      if (!connection) {
        UWF_LOG_W("service") << "enhanced mode staged commit has no authenticated UI agent: attempt=" << attempt;
        lastFailure = QStringLiteral("no authenticated UI agent became available");
        continue;
      }
      try {
        const std::uint64_t requestId = m_nextRequestId.fetch_add(1, std::memory_order_relaxed);
        if (!writeAll(connection->pipe.get(), app::encodeCommandRequest({app::ApplicationCommandKind::CommitStage, requestId}))) {
          discardConnection(connection);
          continue;
        }
        reportRealProgress();

        PipeFrameReader reader;
        auto responseDeadline = std::min(std::chrono::steady_clock::now() + kAgentResponseIdleTimeout, absoluteDeadline);
        std::size_t lastProcessed = 0;
        bool commitPhase = false;
        bool retryRequired = false;
        for (;;) {
          const auto bytes = reader.read(connection->pipe.get(), connection->process.get(), connection->released.get(), m_stopEvent.get(), responseDeadline);
          if (!bytes) break;
          responseDeadline = std::min(std::chrono::steady_clock::now() + kAgentResponseIdleTimeout, absoluteDeadline);
          switch (app::applicationCommandMessageKind(*bytes)) {
            case app::ApplicationCommandMessageKind::Progress: {
              const auto progress = app::decodeCommandProgress(*bytes);
              if (!progress || progress->first != requestId) throw std::runtime_error("enhanced mode progress request ID does not match");
              // 扫描阶段以 total=0 上报已发现文件数，提交阶段才带最终总数。
              // 两阶段的 processed 都从各自的零点计数，不能把扫描计数直接拿来
              // 压制提交阶段的 checkpoint，否则大目录扫描完成后整个提交阶段
              // 可能不再向 SCM 发布真实进度。
              if (progress->second.total != 0 && !commitPhase) {
                commitPhase = true;
                lastProcessed = 0;
              }
              if (progress->second.processed > lastProcessed) {
                lastProcessed = progress->second.processed;
                reportRealProgress();
              }
              break;
            }
            case app::ApplicationCommandMessageKind::Result: {
              const auto result = app::decodeCommandResult(*bytes);
              if (!result || result->first != requestId) throw std::runtime_error("enhanced mode result request ID does not match");
              reportRealProgress();
              if (result->second.authorizesPreshutdownRelease()) return result->second;
              UWF_LOG_W("service") << "enhanced mode staged commit returned a non-final result: attempt=" << attempt
                                   << " outcome=" << static_cast<int>(result->second.outcome);
              lastFailure = result->second.detail.isEmpty() ? QStringLiteral("the UI agent returned a non-final result") : result->second.detail;
              retryRequired = true;
              break;
            }
            case app::ApplicationCommandMessageKind::Request:
              throw std::runtime_error("enhanced mode agent sent an unexpected request");
          }
          if (retryRequired) break;
        }
      } catch (const std::exception& error) {
        UWF_LOG_W("service") << "enhanced mode staged commit protocol failed: attempt=" << attempt << " error=" << error.what();
        lastFailure = QString::fromUtf8(error.what());
      } catch (...) {
        UWF_LOG_W("service") << "enhanced mode staged commit protocol failed: attempt=" << attempt << " error=non-standard-exception";
        lastFailure = QStringLiteral("the UI agent protocol failed with a non-standard exception");
      }
      discardConnection(connection);
      const auto remainingAfterFailure = std::min(infrastructureDeadline, absoluteDeadline) - std::chrono::steady_clock::now();
      if (remainingAfterFailure > std::chrono::steady_clock::duration::zero() &&
          waitForRetryOrStop(std::min<std::chrono::steady_clock::duration>(kOrchestrationRetryDelay, remainingAfterFailure))) {
        return app::ApplicationCommandResult::releasedPreshutdown(
            QStringLiteral("automatic file staging was released because the enhanced mode service infrastructure stopped"));
      }
    }
    return app::ApplicationCommandResult::releasedPreshutdown(
        QStringLiteral("automatic file staging was released after its preshutdown safety deadline: %1").arg(lastFailure));
  }

  void runPreshutdownCommit() {
    std::mutex heartbeatMutex;
    std::condition_variable heartbeatChanged;
    bool completed = false;
    std::jthread heartbeat([this, &heartbeatMutex, &heartbeatChanged, &completed] {
      std::unique_lock lock(heartbeatMutex);
      while (!heartbeatChanged.wait_for(lock, kServiceHeartbeatInterval, [&] { return completed; })) {
        lock.unlock();
        try {
          reportRealProgress();
        } catch (const std::exception& error) {
          UWF_LOG_W("service") << "enhanced mode SCM heartbeat failed: error=" << error.what();
        } catch (...) {
          UWF_LOG_W("service") << "enhanced mode SCM heartbeat failed: error=non-standard-exception";
        }
        lock.lock();
      }
    });

    app::ApplicationCommandResult result;
    try {
      result = requestStagedCommit();
    } catch (const std::exception& error) {
      result = app::ApplicationCommandResult::releasedPreshutdown(QString::fromUtf8(error.what()));
    } catch (...) {
      result = app::ApplicationCommandResult::releasedPreshutdown(QStringLiteral("non-standard preshutdown orchestration failure"));
    }
    if (result.outcome == app::ApplicationCommandOutcome::PreshutdownReleased) {
      UWF_LOG_E("service") << "enhanced mode released the preshutdown barrier after infrastructure failure: error=" << result.detail.toStdString();
    }

    {
      std::scoped_lock lock(heartbeatMutex);
      completed = true;
    }
    heartbeatChanged.notify_all();
    heartbeat.join();
  }

  void stopInfrastructure() {
    if (m_supervisorThread.joinable()) {
      m_supervisorThread.request_stop();
      SetEvent(m_stopEvent.get());
      m_connectionChanged.notify_all();
      m_supervisorThread.join();
    }
    if (m_acceptThread.joinable()) {
      m_acceptThread.request_stop();
      CancelSynchronousIo(reinterpret_cast<HANDLE>(m_acceptThread.native_handle()));
      {
        std::scoped_lock lock(m_connectionMutex);
        if (m_connection) {
          DisconnectNamedPipe(m_connection->pipe.get());
          m_connection.reset();
          ResetEvent(m_agentConnectedEvent.get());
        }
      }
      m_connectionChanged.notify_all();
      m_acceptThread.join();
    }
  }

  SERVICE_STATUS_HANDLE m_statusHandle = nullptr;
  SERVICE_STATUS m_status{};
  std::mutex m_statusMutex;
  std::atomic_uint32_t m_checkpoint{1};
  std::atomic_uint64_t m_nextRequestId{1};
  std::atomic_bool m_preshutdownStarted{false};
  std::atomic<DWORD> m_stopExitCode{NO_ERROR};
  UniqueHandle m_stopEvent;
  UniqueHandle m_preshutdownEvent;
  UniqueHandle m_agentNeededEvent;
  UniqueHandle m_agentConnectedEvent;
  std::jthread m_acceptThread;
  std::jthread m_supervisorThread;
  std::mutex m_connectionMutex;
  std::condition_variable m_connectionChanged;
  std::shared_ptr<AgentConnection> m_connection;
};

}  // namespace

int runEnhancedModeService() {
  ServiceRuntime::instance().run();
  return 0;
}

}  // namespace uwf::service
