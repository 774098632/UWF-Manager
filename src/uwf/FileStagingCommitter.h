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

#include <QList>
#include <QString>
#include <QStringList>
#include <atomic>
#include <cstddef>
#include <stop_token>
#include <vector>

#include "../app/FileStagingStore.h"
#include "api/UwfVolume.h"

namespace uwf {

enum class FileStagingCommitFailureKind {
  InvalidPath,
  ReparsePoint,
  PathInspectionFailed,
  FileTypeChanged,
  DirectoryTypeChanged,
  DirectoryEnumerationFailed,
  MissingDriveLetter,
  VolumeRoot,
  ProviderFailure,
  UnknownFailure,
};

struct FileStagingCommitFailure {
  QString path;
  FileStagingCommitFailureKind kind;
  QString detail;
};

struct FileStagingCommitResult {
  std::size_t discoveredFiles = 0;
  std::size_t committedFiles = 0;
  std::size_t skippedFiles = 0;
  std::size_t skippedEntries = 0;
  QList<FileStagingCommitFailure> failures;

  [[nodiscard]] bool succeeded() const { return failures.isEmpty(); }
};

class FileStagingCommitOperation;
class FileStagingCommitter;

// UI/WMI 线程先冻结当前受保护卷和真正需要扫描的暂存条目。未受保护卷在这里
// 整项跳过，避免关机阶段先递归遍历一个最终不会调用 CommitFile 的大目录。
class FileStagingScanPlan {
 private:
  friend class FileStagingCommitter;

  QList<app::FileStagingEntry> m_entries;
  QStringList m_fileExclusions;
  std::vector<api::VolumeRow> m_volumes;
  std::size_t m_skippedEntries = 0;
  QList<FileStagingCommitFailure> m_failures;
};

// 文件系统扫描结果已经携带准备阶段冻结的卷快照。scan() 只访问文件系统，
// 可安全地在工作线程执行；prepare() 只是把完成的扫描结果收束成提交计划。
class FileStagingScan {
 private:
  friend class FileStagingCommitter;

  QList<QString> m_files;
  QStringList m_fileExclusions;
  std::vector<api::VolumeRow> m_volumes;
  std::size_t m_skippedFiles = 0;
  std::size_t m_skippedEntries = 0;
  QList<FileStagingCommitFailure> m_failures;
};

// prepare() 生成的提交快照。文件数只包含当前位于受保护卷、确实需要调用
// CommitFile 的普通文件；未受保护卷、缺失路径和扫描失败分别保留为跳过或失败
// 结果。计划一旦生成便不再吸收目录中后续新增的文件，确保确认框展示的数量与
// 本轮实际处理范围一致。
class FileStagingCommitPlan {
 public:
  [[nodiscard]] std::size_t fileCount() const { return static_cast<std::size_t>(m_files.size()); }

 private:
  friend class FileStagingCommitter;
  friend class FileStagingCommitOperation;

  QList<QString> m_files;
  std::vector<api::VolumeRow> m_volumes;
  std::size_t m_discoveredFiles = 0;
  std::size_t m_skippedFiles = 0;
  std::size_t m_skippedEntries = 0;
  QList<FileStagingCommitFailure> m_failures;
};

// 在 UI 线程上每次只推进一个 CommitFile 调用。调用方可在相邻 advance()
// 之间把控制权交还 Qt 事件循环，从而更新同一个电源对话框中的真实进度。
class FileStagingCommitOperation {
 public:
  [[nodiscard]] bool finished() const { return m_nextFile >= m_files.size(); }
  [[nodiscard]] std::size_t totalFiles() const { return static_cast<std::size_t>(m_files.size()); }
  [[nodiscard]] std::size_t processedFiles() const { return static_cast<std::size_t>(m_nextFile); }
  [[nodiscard]] const FileStagingCommitResult& result() const { return m_result; }

  // 返回本次处理的规范化路径；finished() 为 true 时调用属于编程错误。
  [[nodiscard]] QString advance();

 private:
  friend class FileStagingCommitter;

  FileStagingCommitOperation(const api::UwfVolume& volume, FileStagingCommitPlan plan);

  // UwfVolume 是只持有 WmiOperations 引用的轻量值对象。操作按值保存它，
  // 避免 beginCommit() 返回值隐式依赖创建它的 FileStagingCommitter 寿命。
  api::UwfVolume m_volume;
  QList<QString> m_files;
  std::vector<api::VolumeRow> m_volumes;
  qsizetype m_nextFile = 0;
  FileStagingCommitResult m_result;
};

// 把持久化暂存条目展开成实际文件并提交到当前会话卷。目录递归时跳过所有
// reparse point；重叠目录和显式文件按大小写不敏感路径去重。失败逐项收集，
// 由电源用例决定是否继续关机/重启。
class FileStagingCommitter {
 public:
  explicit FileStagingCommitter(WmiOperations& session);

  [[nodiscard]] FileStagingScanPlan prepareScan(const QList<app::FileStagingEntry>& entries) const;
  [[nodiscard]] static FileStagingScan scan(FileStagingScanPlan plan, std::stop_token stopToken = {}, std::atomic_size_t* discoveredProgress = nullptr);
  [[nodiscard]] FileStagingCommitPlan prepare(FileStagingScan scan) const;
  [[nodiscard]] FileStagingCommitOperation beginCommit(FileStagingCommitPlan plan) const;

 private:
  api::UwfVolume m_volume;
};

}  // namespace uwf
