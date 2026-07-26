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
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "../app/ApplicationCommand.h"

namespace uwf::service {

// UI 进程与 LocalSystem 服务之间的认证代理边界。MainWindow 只依赖这个
// 生命周期/消息契约；生产实现负责命名管道，其它宿主可以提供自己的传输，
// 不必让 UI 编排知道 Windows I/O 细节。
class EnhancedModeAgentConnection : public QObject {
  Q_OBJECT
 public:
  explicit EnhancedModeAgentConnection(QObject* parent = nullptr) : QObject(parent) {}
  ~EnhancedModeAgentConnection() override = default;

  virtual void start() = 0;
  virtual void stop() = 0;
  [[nodiscard]] virtual bool running() const = 0;
  virtual void reportProgress(std::uint64_t requestId, std::size_t processed, std::size_t total) = 0;
  virtual void complete(std::uint64_t requestId, const app::ApplicationCommandResult& result) = 0;

 signals:
  void commitStageRequested(std::uint64_t requestId);
  void connectionStateChanged(bool connected);
};

// 交互式管理员进程中的服务代理。工作线程只负责经过身份认证的命名管道；
// 真正的 UWF 任务通过信号回到 UI 线程。服务一次只发一个请求，因此响应槽
// 同样保持单飞，不需要把并发协议复杂度泄露给业务层。
class EnhancedModeAgent final : public EnhancedModeAgentConnection {
  Q_OBJECT
 public:
  explicit EnhancedModeAgent(QObject* parent = nullptr);
  ~EnhancedModeAgent() override;

  void start() override;
  void stop() override;
  [[nodiscard]] bool running() const override;
  void reportProgress(std::uint64_t requestId, std::size_t processed, std::size_t total) override;
  void complete(std::uint64_t requestId, const app::ApplicationCommandResult& result) override;

 private:
  struct Response {
    std::uint64_t requestId = 0;
    app::ApplicationCommandResult result;
  };

  void run(std::stop_token stopToken);
  [[nodiscard]] bool waitBeforeReconnect(std::stop_token stopToken, std::chrono::milliseconds delay);

  mutable std::mutex m_mutex;
  std::condition_variable m_responseReady;
  std::optional<std::uint64_t> m_activeRequestId;
  std::optional<Response> m_response;
  std::optional<std::pair<std::uint64_t, app::ApplicationCommandProgress>> m_progress;
  std::atomic_bool m_workerActive{false};
  std::jthread m_thread;
};

}  // namespace uwf::service
