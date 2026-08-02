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
#include <vector>

namespace uwf::ui {

struct DiagnosticField {
  QString key;
  QString value;
};

// 收集当前进程 Token 的稳定文本字段。Win32 错误在 API 返回后立即捕获，
// 调用者不依赖线程 last-error 的隐式状态。
[[nodiscard]] std::vector<DiagnosticField> collectProcessTokenDiagnostics();

}  // namespace uwf::ui
