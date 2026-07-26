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
#include <QPointer>
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "../app/ApplicationCommand.h"
#include "../app/FileStagingStore.h"
#include "../uwf/UwfSnapshot.h"
#include "../uwf/wmi/WmiClient.h"

class QWidget;

namespace uwf::ui {

enum class FileStagingRequestOrigin {
  UserCommand,
  ServicePreshutdown,
};

struct FileStagingBatchResult {
  app::ApplicationCommandResult command;
  // 已成功读取的原始暂存列表。即使列表为空也保留 engaged 状态；nullopt
  // 明确表示批次在形成可核验快照前失败，不能把空列表误当成它的输入。
  std::optional<QList<app::FileStagingEntry>> sourceEntries;
};

// 把任意来源的暂存提交请求合并成一个进行中的批次。每个请求都收到同一份
// 最终结果；不会为了第二个客户端重建进度窗或重复提交文件。
class FileStagingCoordinator final : public QObject {
  Q_OBJECT
 public:
  using Completion = std::function<void(const FileStagingBatchResult&)>;

  // 安全电源流程需要先计算、等待用户确认，再在自己的对话框中提交。租约把
  // 这段时间纳入同一个单飞域：服务和第二实例到达的请求只加入等待队列，
  // 不会另起一个 FileStagingTask。若租约未产生最终结果（例如用户在提交前
  // 取消），析构时会自动为等待队列启动普通命令批次。
  class ExternalBatch final {
   public:
    ExternalBatch() = default;
    ~ExternalBatch();
    ExternalBatch(const ExternalBatch&) = delete;
    ExternalBatch& operator=(const ExternalBatch&) = delete;
    ExternalBatch(ExternalBatch&& other) noexcept;
    ExternalBatch& operator=(ExternalBatch&& other) noexcept;

    void complete(const FileStagingBatchResult& result);
    [[nodiscard]] explicit operator bool() const { return !m_owner.isNull(); }

   private:
    friend class FileStagingCoordinator;
    explicit ExternalBatch(FileStagingCoordinator* owner) : m_owner(owner) {}
    void release();

    QPointer<FileStagingCoordinator> m_owner;
  };

  using ExternalCompletion = std::function<void(const FileStagingBatchResult&, ExternalBatch)>;

  FileStagingCoordinator(WmiOperations& session, app::FileStagingStore& store, UwfCapability capability, QWidget* parentWindow, QObject* parent = nullptr);
  ~FileStagingCoordinator() override;

  void requestCommit(FileStagingRequestOrigin origin, Completion completion);
  // 空 optional 表示已有批次拥有执行权；completedActiveBatch 已加入该批次，
  // 最终结果到达后协调器会把外部租约一并移交给调用方，使确认窗口期间仍
  // 保持单飞所有权，不会出现服务重试抢先启动第二个提交批次。
  [[nodiscard]] std::optional<ExternalBatch> reserveExternalBatch(ExternalCompletion completedActiveBatch);
  [[nodiscard]] bool active() const;

 signals:
  void progressChanged(std::size_t processed, std::size_t total);

 private:
  class Operation;
  enum class Ownership {
    Idle,
    Command,
    External,
    Count,
  };
  enum class OwnershipEvent {
    CommandStarted,
    CommandCompleted,
    CommandHandedOff,
    ExternalReserved,
    ExternalCompleted,
    ExternalReleased,
    Count,
  };
  struct OwnershipTransition {
    Ownership next = Ownership::Idle;
    bool valid = false;
  };

  [[nodiscard]] static const std::array<OwnershipTransition, static_cast<std::size_t>(Ownership::Count) *
                                                                static_cast<std::size_t>(OwnershipEvent::Count)>&
  ownershipTransitions();
  void postOwnershipEvent(OwnershipEvent event);
  void startCommandBatch();
  void completeExternalBatch(const FileStagingBatchResult& result);
  void releaseExternalBatch();
  void finish(const FileStagingBatchResult& result);

  WmiOperations& m_session;
  app::FileStagingStore& m_store;
  UwfCapability m_capability;
  QWidget* m_parentWindow;
  Ownership m_ownership = Ownership::Idle;
  std::unique_ptr<Operation> m_operation;
  std::vector<Completion> m_completions;
  std::optional<ExternalCompletion> m_externalCompletion;
};

}  // namespace uwf::ui
