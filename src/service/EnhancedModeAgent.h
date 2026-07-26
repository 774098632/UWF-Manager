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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace uwf::service {

enum class EnhancedAgentControl : std::uint8_t;

// UI 进程与 LocalSystem 服务之间的身份连接边界。管道不承载 UWF 提交任务；
// 除维持双方身份外，只传递安全电源流程的一次性 PRESHUTDOWN 令牌状态。
class EnhancedModeAgentConnection : public QObject {
  Q_OBJECT
 public:
  explicit EnhancedModeAgentConnection(QObject* parent = nullptr) : QObject(parent) {}
  ~EnhancedModeAgentConnection() override = default;

  virtual void start() = 0;
  virtual void stop() = 0;
  [[nodiscard]] virtual bool running() const = 0;
  [[nodiscard]] virtual bool markPreshutdownCommitHandled() = 0;
  [[nodiscard]] virtual bool requirePreshutdownCommit() = 0;

 signals:
  void connectionStateChanged(bool connected);
};

class EnhancedModeAgent final : public EnhancedModeAgentConnection {
  Q_OBJECT
 public:
  explicit EnhancedModeAgent(QObject* parent = nullptr);
  ~EnhancedModeAgent() override;

  void start() override;
  void stop() override;
  [[nodiscard]] bool running() const override;
  [[nodiscard]] bool markPreshutdownCommitHandled() override;
  [[nodiscard]] bool requirePreshutdownCommit() override;

 private:
  struct ControlChannel;
  void run(std::stop_token stopToken);
  [[nodiscard]] bool sendControl(EnhancedAgentControl control);
  [[nodiscard]] bool waitBeforeReconnect(std::stop_token stopToken, std::chrono::milliseconds delay);

  std::mutex m_waitMutex;
  std::condition_variable m_waitChanged;
  std::atomic_bool m_workerActive{false};
  std::jthread m_thread;
  std::unique_ptr<ControlChannel> m_control;
};

}  // namespace uwf::service
