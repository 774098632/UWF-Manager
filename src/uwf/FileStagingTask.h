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

#include <QString>
#include <array>
#include <atomic>
#include <future>
#include <optional>
#include <thread>

#include "../app/FileStagingStore.h"
#include "FileStagingCommitter.h"
#include "UwfSnapshot.h"
#include "api/UwfFilter.h"

namespace uwf {

struct FileStagingTaskProgress {
  std::size_t processedFiles = 0;
  std::size_t totalFiles = 0;
  QString currentPath;
};

// 一次完整的文件暂存批次。准备阶段只在首次 pollPreparation() 时读取注册表、
// Filter 和当前卷快照，先排除未受保护卷，再把文件系统扫描放到 jthread；
// 提交阶段仍由拥有 WMI session 的线程逐项推进。PowerController、
// --commit-stage 和服务内 PRESHUTDOWN worker 共用这一个状态机，因而不会
// 出现三套“哪些文件该提交、失败后是否继续”的口径。
class FileStagingTask final {
 public:
  FileStagingTask(WmiOperations& session, app::FileStagingStore& store, UwfCapability capability);
  FileStagingTask(WmiOperations& session, QList<app::FileStagingEntry> entries, UwfCapability capability);
  ~FileStagingTask();

  FileStagingTask(const FileStagingTask&) = delete;
  FileStagingTask& operator=(const FileStagingTask&) = delete;

  // 返回 false 表示扫描仍在后台进行；返回 true 后 totalFiles()、finished() 和
  // result() 可用。失败不会截断已经生成的批次，提交阶段仍处理每个目标。
  [[nodiscard]] bool pollPreparation();
  [[nodiscard]] bool preparationFinished() const;
  [[nodiscard]] bool finished() const;
  [[nodiscard]] std::size_t discoveredDuringScan() const;
  [[nodiscard]] std::size_t totalFiles() const;
  [[nodiscard]] const std::optional<QList<app::FileStagingEntry>>& sourceEntries() const;
  [[nodiscard]] FileStagingTaskProgress advance();
  [[nodiscard]] const FileStagingCommitResult& result() const;

 private:
  enum class State {
    NotStarted,
    Scanning,
    Ready,
    Count,
  };
  enum class Event {
    ScanStarted,
    ScanCompleted,
    CompletedWithoutScan,
    Count,
  };
  struct Transition {
    State next = State::NotStarted;
    bool valid = false;
  };

  [[nodiscard]] static const std::array<Transition, static_cast<std::size_t>(State::Count) * static_cast<std::size_t>(Event::Count)>& transitions();
  void postEvent(Event event);
  void finishWithoutCommit(FileStagingCommitResult result = {});

  app::FileStagingStore* m_store = nullptr;
  std::optional<QList<app::FileStagingEntry>> m_entries;
  UwfCapability m_capability;
  api::UwfFilter m_filter;
  FileStagingCommitter m_committer;
  std::optional<QList<app::FileStagingEntry>> m_sourceEntries;
  State m_state = State::NotStarted;
  std::future<FileStagingScan> m_scanFuture;
  std::atomic_size_t m_discoveredDuringScan{0};
  std::jthread m_scanThread;
  std::optional<FileStagingCommitOperation> m_operation;
  std::optional<FileStagingCommitResult> m_terminalResult;
};

}  // namespace uwf
