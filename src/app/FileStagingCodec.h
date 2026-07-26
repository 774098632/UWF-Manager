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
#include <string>
#include <vector>

namespace uwf::app {

enum class FileStagingKind {
  File,
  Directory,
};

struct FileStagingEntry {
  FileStagingKind kind;
  QString path;

  bool operator==(const FileStagingEntry&) const = default;
};

// 文件暂存最终会调用按卷寻址的 UWF CommitFile，只接受带盘符的本地绝对
// 路径。UNC、卷内相对路径和设备路径都不属于可提交的卷地址。
[[nodiscard]] bool isAbsoluteLocalFileStagingPath(const QString& path);

// 注册表持久化格式是一个 REG_MULTI_SZ：每条记录以 F| 或 D| 标记文件/目录，
// 后面保留完整路径。编码与存储传输解耦，纯协议测试不必链接注册表和 WMI。
[[nodiscard]] std::vector<std::string> encodeFileStagingEntries(const QList<FileStagingEntry>& entries);
[[nodiscard]] QList<FileStagingEntry> decodeFileStagingEntries(const std::vector<std::string>& records);

}  // namespace uwf::app
