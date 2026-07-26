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

#include <windows.h>

#include <cstddef>
#include <span>

namespace uwf::service {

enum class EnhancedPipeIoResult {
  Completed,
  Disconnected,
  Stopped,
};

// 增强模式身份管道只使用 overlapped I/O。每项操作同时等待 I/O 事件和调用方
// 的停止事件；停止事件胜出时取消并回收当前 OVERLAPPED，再返回 Stopped。
// 这样线程退出不依赖 CancelSynchronousIo，也不会在不可中断的 ReadFile /
// ConnectNamedPipe 后执行无界 join。
[[nodiscard]] EnhancedPipeIoResult connectEnhancedPipe(HANDLE pipe, HANDLE stopEvent);
[[nodiscard]] EnhancedPipeIoResult readEnhancedPipe(HANDLE pipe, std::span<std::byte> bytes, HANDLE stopEvent);
[[nodiscard]] EnhancedPipeIoResult writeEnhancedPipe(HANDLE pipe, std::span<const std::byte> bytes, HANDLE stopEvent);

}  // namespace uwf::service
