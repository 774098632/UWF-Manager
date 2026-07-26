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

#include <filesystem>

namespace uwf::winfs {

// Windows 文件属性的一次权威观察。ReparsePoint 独立于文件/目录分类，调用方
// 可以先建立递归边界，再决定是否处理普通文件或目录。
enum class EntryType {
  Missing,
  File,
  Directory,
  ReparsePoint,
};

// 路径明确不存在返回 Missing；权限、I/O 或无效路径等无法确认事实的情况抛
// std::system_error，不得伪装成“不存在”。
[[nodiscard]] EntryType inspect(const std::filesystem::path& path);

}  // namespace uwf::winfs
