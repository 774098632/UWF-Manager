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
#pragma once

#include <QObject>
#include <QString>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include "../uwf/RegistryTreeCommitter.h"
#include "../uwf/UwfSnapshot.h"
#include "../uwf/wmi/WmiClient.h"

namespace uwf::service {

inline constexpr wchar_t kEnhancedServiceName[] = L"UWFManagerEnhanced";
inline constexpr wchar_t kEnhancedServiceDisplayName[] = L"UWF Manager Enhanced Mode";
inline constexpr wchar_t kEnhancedPipeName[] = L"\\\\.\\pipe\\UWFManager.EnhancedService.v3";
inline constexpr char kEnhancedServiceRegistryKey[] = R"(HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\UWFManagerEnhanced)";
inline constexpr std::array<char, 8> kEnhancedAgentHandshake{'U', 'W', 'F', 'A', 'G', 'T', '0', '3'};

enum class EnhancedAgentControl : std::uint8_t {
  PreshutdownCommitHandled = 1,
  PreshutdownCommitRequired = 2,
};

struct EnhancedAgentControlMessage {
  EnhancedAgentControl control = EnhancedAgentControl::PreshutdownCommitRequired;
  std::uint64_t requestId = 0;

  [[nodiscard]] bool operator==(const EnhancedAgentControlMessage&) const = default;
};

inline constexpr std::size_t kEnhancedAgentControlFrameSize = 1 + sizeof(std::uint64_t);
using EnhancedAgentControlFrame = std::array<std::uint8_t, kEnhancedAgentControlFrameSize>;

[[nodiscard]] constexpr EnhancedAgentControlFrame encodeEnhancedAgentControl(const EnhancedAgentControlMessage message) {
  if (message.requestId == 0) throw std::invalid_argument("enhanced agent control request ID cannot be zero");
  if (message.control != EnhancedAgentControl::PreshutdownCommitHandled && message.control != EnhancedAgentControl::PreshutdownCommitRequired) {
    throw std::invalid_argument("enhanced agent control is invalid");
  }
  EnhancedAgentControlFrame frame{};
  frame[0] = static_cast<std::uint8_t>(message.control);
  for (std::size_t index = 0; index < sizeof(message.requestId); ++index) {
    frame[index + 1] = static_cast<std::uint8_t>((message.requestId >> (index * 8U)) & 0xffU);
  }
  return frame;
}

[[nodiscard]] constexpr std::optional<EnhancedAgentControlMessage> decodeEnhancedAgentControl(const EnhancedAgentControlFrame& frame) {
  const auto rawControl = frame[0];
  if (rawControl != static_cast<std::uint8_t>(EnhancedAgentControl::PreshutdownCommitHandled) &&
      rawControl != static_cast<std::uint8_t>(EnhancedAgentControl::PreshutdownCommitRequired)) {
    return std::nullopt;
  }

  std::uint64_t requestId = 0;
  for (std::size_t index = 0; index < sizeof(requestId); ++index) {
    requestId |= static_cast<std::uint64_t>(frame[index + 1]) << (index * 8U);
  }
  if (requestId == 0) return std::nullopt;
  return EnhancedAgentControlMessage{static_cast<EnhancedAgentControl>(rawControl), requestId};
}

enum class EnhancedModeState {
  Disabled,
  Enabled,
  Stopped,
  RepairRequired,
};

enum class EnhancedModeAction {
  Enable,
  Disable,
  Start,
  Repair,
};

enum class EnhancedModeAgentState {
  Unobserved,
  Connecting,
  Disconnected,
  Connected,
};

struct EnhancedModeStatus {
  EnhancedModeState state = EnhancedModeState::Disabled;
  bool serviceExists = false;
  bool ownProcess = false;
  bool automaticStart = false;
  bool localSystemAccount = false;
  bool running = false;
  bool executableMatches = false;
  bool preshutdownTimeoutConfigured = false;
  bool requiredPrivilegesConfigured = false;
  bool preshutdownAccepted = false;
  bool serviceRegistryPresent = false;
  // 只有交互式 UI 进程能观测自己的代理管道。命令行服务控制入口保持
  // Unobserved；Connected 表示服务与 UI 已完成双向身份认证。
  EnhancedModeAgentState agentState = EnhancedModeAgentState::Unobserved;
  QString detail;

  [[nodiscard]] bool hasRemnants() const { return serviceExists || serviceRegistryPresent; }
  [[nodiscard]] bool serviceConfigurationSatisfied() const {
    return serviceExists && ownProcess && automaticStart && localSystemAccount && executableMatches && preshutdownTimeoutConfigured &&
           requiredPrivilegesConfigured;
  }
  [[nodiscard]] bool serviceContractSatisfied() const { return serviceConfigurationSatisfied() && running && preshutdownAccepted; }
  [[nodiscard]] EnhancedModeAction availableAction() const {
    switch (state) {
      case EnhancedModeState::Disabled:
        return EnhancedModeAction::Enable;
      case EnhancedModeState::Enabled:
        return EnhancedModeAction::Disable;
      case EnhancedModeState::Stopped:
        return EnhancedModeAction::Start;
      case EnhancedModeState::RepairRequired:
        return EnhancedModeAction::Repair;
    }
    throw std::logic_error("unknown enhanced mode state");
  }
};

struct EnhancedModeChangeResult {
  EnhancedModeStatus status;
  QString persistenceWarning;
};

// Windows 服务及其注册表工件共同构成明确的系统边界。UI 与业务编排只依赖
// 这个接口，不能在注入内存实现后仍绕过它访问真实 SCM 或 HKLM。
class EnhancedModeServiceControl {
 public:
  virtual ~EnhancedModeServiceControl() = default;
  [[nodiscard]] virtual EnhancedModeStatus query() const = 0;
  virtual void installAndStart(const QString& description) = 0;
  virtual void start() = 0;
  [[nodiscard]] virtual std::vector<RegistryCommitTarget> planRegistryCommit() const = 0;
  [[nodiscard]] virtual std::vector<RegistryCommitTarget> planRegistryDeletion() const = 0;
  virtual void deleteRegistryRemnants() = 0;
  // SCM 只有在服务停止且最后一个服务句柄关闭后才真正删除服务项。调用方在
  // 返回的屏障存活期间提交服务注册表键的递归删除，随后销毁屏障完成删除。
  class DeletionBarrier {
   public:
    virtual ~DeletionBarrier() = default;
  };
  enum class RemovalState {
    Absent,
    AlreadyMarkedForDeletion,
    MarkedForDeletion,
  };
  struct Removal {
    RemovalState state = RemovalState::Absent;
    std::unique_ptr<DeletionBarrier> barrier;
  };
  [[nodiscard]] virtual Removal stopAndMarkForDeletion() = 0;
};

class WindowsEnhancedModeServiceControl final : public EnhancedModeServiceControl {
 public:
  [[nodiscard]] EnhancedModeStatus query() const override;
  void installAndStart(const QString& description) override;
  void start() override;
  [[nodiscard]] std::vector<RegistryCommitTarget> planRegistryCommit() const override;
  [[nodiscard]] std::vector<RegistryCommitTarget> planRegistryDeletion() const override;
  void deleteRegistryRemnants() override;
  [[nodiscard]] Removal stopAndMarkForDeletion() override;
};

class EnhancedModeManager final : public QObject {
  Q_OBJECT
 public:
  EnhancedModeManager(EnhancedModeServiceControl& serviceControl, WmiOperations& session, UwfCapability capability, QObject* parent = nullptr);

  [[nodiscard]] EnhancedModeStatus status() const;
  [[nodiscard]] EnhancedModeChangeResult enable(const QString& serviceDescription);
  [[nodiscard]] EnhancedModeChangeResult start();
  [[nodiscard]] EnhancedModeChangeResult disable();
  void setAgentState(EnhancedModeAgentState state);

 signals:
  void statusChanged(uwf::service::EnhancedModeStatus status);
  void agentConnectionStateChanged(uwf::service::EnhancedModeAgentState state);

 private:
  [[nodiscard]] EnhancedModeChangeResult publishChangeResult(QString persistenceWarning);
  [[nodiscard]] QString persistInstallation() const;
  // nullopt 表示当前删除会直接落盘；engaged 表示 UWF 正在保护当前会话，
  // 且破坏性 SCM 操作开始前已经完整冻结待删除注册表树。能力读取或规划失败
  // 必须在 DeleteService 前抛出，不能留下“当前消失、重启复活”的半事务。
  [[nodiscard]] std::optional<std::vector<RegistryCommitTarget>> prepareRemovalPersistence() const;
  [[nodiscard]] QString persistRemoval(const std::vector<RegistryCommitTarget>& deletionPlan) const;

  EnhancedModeServiceControl& m_serviceControl;
  WmiOperations& m_session;
  UwfCapability m_capability;
  EnhancedModeAgentState m_agentState = EnhancedModeAgentState::Unobserved;
};

// 必须在 QApplication、CrashHandler 和 WMI 初始化之前调用。
int runEnhancedModeService();

}  // namespace uwf::service

Q_DECLARE_METATYPE(uwf::service::EnhancedModeStatus)
Q_DECLARE_METATYPE(uwf::service::EnhancedModeAgentState)
