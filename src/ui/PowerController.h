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

#include <QObject>
#include <exception>
#include <functional>

#include "../app/FileStagingStore.h"
#include "../uwf/UwfSnapshot.h"
#include "../uwf/wmi/WmiClient.h"
#include "FileStagingCoordinator.h"

class QWidget;

namespace uwf::ui {

enum class PowerAction;
enum class PowerActionDialogMode;

// UWF 安全关机 / 重启用例：先显示单一对话框，再异步展开文件暂存目录；统一
// 提交计划、确认与进度、Filter 复核、WMI 电源调用和失败决策。工具栏与应用
// 完成对话框直接连接这里，避免维护多份入口逻辑。
struct PowerControllerServices {
  enum class PreshutdownControlResult {
    NotApplicable,
    Acknowledged,
    Unacknowledged,
  };

  struct PreshutdownCommitControl {
    std::function<PreshutdownControlResult()> markHandled;
    std::function<PreshutdownControlResult()> markRequired;
  };

  WmiOperations& session;
  app::FileStagingStore& fileStaging;
  UwfCapability uwfCapability;
  FileStagingCoordinator& stagingCoordinator;
  // 安全电源流程完成 UI 预提交后，用已认证服务连接武装/撤销一次性
  // PRESHUTDOWN 跳过令牌。返回值只表示确认是否到达；写入成功但确认丢失
  // 时服务仍可能已经应用状态，因此电源调用失败后必须无条件尝试撤销。
  PreshutdownCommitControl preshutdown{};
};

class PowerController : public QObject {
  Q_OBJECT
 public:
  PowerController(PowerControllerServices services, QWidget* dialogParent, QObject* parent = nullptr);

 public slots:
  void safeShutdown();
  void safeRestart();
  void directRestart();

 private:
  void execute(PowerAction action);
  void executeDirectRestart();
  void executeReserved(PowerAction action, FileStagingCoordinator::ExternalBatch batch);
  void executeWithCompletedStaging(PowerAction action, FileStagingBatchResult stagingResult, FileStagingCoordinator::ExternalBatch batch);
  void invokePowerAction(PowerAction action);
  void reportPowerFailure(PowerAction action, const std::exception& error, PowerActionDialogMode mode);
  void reportUnknownPowerFailure(PowerAction action, PowerActionDialogMode mode);

  QWidget* m_dialogParent;
  WmiOperations& m_session;
  app::FileStagingStore& m_fileStaging;
  UwfCapability m_uwfCapability;
  FileStagingCoordinator& m_stagingCoordinator;
  PowerControllerServices::PreshutdownCommitControl m_preshutdown;
  bool m_actionActive = false;
};

}  // namespace uwf::ui
