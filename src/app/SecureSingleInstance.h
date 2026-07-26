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
#include <memory>

#include "ApplicationCommand.h"

class QLocalSocket;

namespace uwf::app {

// 进程级安全单实例协调器。对外只暴露主实例资格与激活请求；Windows 命名
// 管道、对端身份认证和启动竞态均封装在实现文件中。
class SecureSingleInstance final : public QObject {
  Q_OBJECT
 public:
  struct Scope {
    // 空值表示生产默认作用域。非空值用于同一可执行文件中彼此独立的逻辑实例，
    // 会先哈希再进入管道名，不能注入路径或改变对端身份认证规则。
    QString discriminator;
  };

  enum class AcquireResult {
    Primary,
    ForwardedExisting,
    Unprotected,
  };

  explicit SecureSingleInstance(QObject* parent = nullptr);
  explicit SecureSingleInstance(const Scope& scope, QObject* parent = nullptr);
  ~SecureSingleInstance() override;

  SecureSingleInstance(const SecureSingleInstance&) = delete;
  SecureSingleInstance& operator=(const SecureSingleInstance&) = delete;

  // 把启动命令转交已有受信实例；不存在时注册当前进程为主实例。CommitStage
  // 会同步等待主实例返回最终批次结果，其他命令只等待主实例接收确认。
  // Unprotected 表示服务名被无法认证的对象占用或监听失败，调用方可以继续
  // 运行，但不再保证单实例。
  [[nodiscard]] AcquireResult acquire(ApplicationCommandKind command = ApplicationCommandKind::Activate, ApplicationCommandResult* forwardedResult = nullptr);
  [[nodiscard]] QString errorString() const;

  // MainWindow 建立信号连接后调用。此前到达并通过认证的激活请求会合并，
  // 提交请求则逐个保留并投递，避免窗口构造期间丢失命令。
  void enableCommandNotifications();

  // 完成一个 commitStageRequested 请求。requestToken 是进程内令牌，不是
  // 客户端可控的协议 requestId；重复、过期令牌被安全忽略。
  void completeCommand(std::uint64_t requestToken, const ApplicationCommandResult& result);

 signals:
  void activationRequested();
  void commitStageRequested(std::uint64_t requestToken);

 private:
  class Private;
  std::unique_ptr<Private> d;

  void processPendingConnections();
  void processSocket(QLocalSocket* socket);
  void deliverPendingCommands();
};

}  // namespace uwf::app
