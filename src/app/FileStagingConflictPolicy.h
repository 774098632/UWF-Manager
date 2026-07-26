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
#include <optional>

#include "FileStagingCodec.h"

namespace uwf::app {

// 文件排除和文件暂存对同一棵卷目录树声明了相反的持久化语义。该模型保存
// 两侧已经提交/待应用的快照，并以路径段为边界判断相交关系；UI 的两个入口
// 共用它，避免只在某一侧拦截而留下顺序相关行为。
class FileStagingConflictPolicy final {
 public:
  void setFileExclusions(QStringList exclusions);
  void setStagedEntries(const QList<FileStagingEntry>& entries);

  [[nodiscard]] std::optional<QString> conflictingExclusion(const QString& stagedPath) const;
  [[nodiscard]] std::optional<QString> conflictingStagedPath(const QString& exclusionPath) const;

 private:
  QStringList m_fileExclusions;
  QStringList m_stagedPaths;
};

}  // namespace uwf::app
