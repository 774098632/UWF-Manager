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
#include <stdexcept>

#include "ApplicationCommand.h"

namespace uwf::app {

enum class StartupMode {
  Interactive,
  Quiet,
  CommitStage,
  Service,
  InstallService,
  UninstallService,
};

struct StartupOptions {
  StartupMode mode = StartupMode::Interactive;

  [[nodiscard]] ApplicationCommandKind initialCommand() const;
};

class StartupOptionsError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

// arguments 与 QCoreApplication::arguments() 的契约一致：首项是可执行文件，
// 后续项才是用户参数。所有运行模式互斥，未知、重复或组合参数都明确拒绝，
// 避免服务控制命令被静默解释成交互式启动。
[[nodiscard]] StartupOptions parseStartupOptions(const QStringList& arguments);

}  // namespace uwf::app
