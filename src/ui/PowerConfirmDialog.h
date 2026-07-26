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
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <variant>

class QWidget;

namespace uwf::ui {

enum class PowerAction { Shutdown, Restart };

enum class PowerStagingState { InProgress, Completed, Failed };
enum class PowerActionDialogOutcome { Canceled, Confirmed, CanceledAfterStagingFailure, ContinuedAfterStagingFailure };

struct PowerStagingProgress {
  PowerStagingState state = PowerStagingState::Completed;
  std::size_t processedFiles = 0;
  std::size_t totalFiles = 0;
  QString currentPath;
  QString failureDetails;
};

struct PowerStagingNotRequired {};

struct PowerStagingCommit {
  std::size_t filesToCommit = 0;
  std::function<PowerStagingProgress()> advanceStaging;
};

struct PowerStagingFailure {
  QString details;
};

using PowerStagingPreparation = std::variant<PowerStagingNotRequired, PowerStagingCommit, PowerStagingFailure>;

struct PowerActionDialogRequest {
  PowerActionDialogRequest(PowerAction requestedAction, std::function<std::optional<PowerStagingPreparation>()> preparation = {},
                           std::optional<PowerStagingFailure> completedFailure = std::nullopt)
      : action(requestedAction), pollStagingPreparation(std::move(preparation)), completedStagingFailure(std::move(completedFailure)) {}

  PowerAction action;
  // 返回 nullopt 表示仍在计算。回调首次在对话框进入事件循环后执行，确保
  // 文件系统扫描不会阻塞对话框的首次显示；返回结果后不再调用。
  std::function<std::optional<PowerStagingPreparation>()> pollStagingPreparation;
  // 另一个入口已经完成同一批暂存提交时，电源流程直接复用其最终失败结果，
  // 不再伪装成一次新的“计算”或额外等待最短进度展示时间。
  std::optional<PowerStagingFailure> completedStagingFailure;
};

// UWF 安全电源操作专用单一对话框。存在文件暂存条目时先立即显示计算状态，
// 该状态至少稳定展示 2 秒且计算完成前禁止确认；随后原位切换为确认或提交
// 进度。失败仍在同一窗口展示逐项详情，并以取消为默认选项询问是否继续。
PowerActionDialogOutcome runPowerActionDialog(QWidget* parent, PowerActionDialogRequest request);

}  // namespace uwf::ui
