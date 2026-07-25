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

#include <QStringList>

namespace uwf::app {

enum class StartupMode {
  Interactive,
  Quiet,
};

struct StartupOptions {
  StartupMode mode = StartupMode::Interactive;
};

// arguments 与 QCoreApplication::arguments() 的契约一致：首项是可执行文件，
// 后续项才是用户参数。只接受完整、区分大小写的 --quiet，避免把拼写相近的
// 未知参数静默解释成另一种启动行为。
[[nodiscard]] StartupOptions parseStartupOptions(const QStringList& arguments);

}  // namespace uwf::app
