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

#include "../app/FileStagingStore.h"
#include "../uwf/UwfSnapshot.h"
#include "../uwf/wmi/WmiClient.h"
#include "FileStagingCoordinator.h"

class QWidget;

namespace uwf::ui {

enum class PowerAction;

// UWF 安全关机 / 重启用例：先显示单一对话框，再异步展开文件暂存目录；统一
// 提交计划、确认与进度、Filter 复核、WMI 电源调用和失败决策。工具栏与应用
// 完成对话框直接连接这里，避免维护多份入口逻辑。
struct PowerControllerServices {
  WmiOperations& session;
  app::FileStagingStore& fileStaging;
  UwfCapability uwfCapability;
  FileStagingCoordinator& stagingCoordinator;
};

class PowerController : public QObject {
  Q_OBJECT
 public:
  PowerController(PowerControllerServices services, QWidget* dialogParent, QObject* parent = nullptr);

 public slots:
  void safeShutdown();
  void safeRestart();

 private:
  void execute(PowerAction action);
  void executeReserved(PowerAction action, FileStagingCoordinator::ExternalBatch batch);
  void executeWithCompletedStaging(PowerAction action, FileStagingBatchResult stagingResult, FileStagingCoordinator::ExternalBatch batch);
  void invokePowerAction(PowerAction action);
  void reportPowerFailure(PowerAction action, const std::exception& error);
  void reportUnknownPowerFailure(PowerAction action);

  QWidget* m_dialogParent;
  WmiOperations& m_session;
  app::FileStagingStore& m_fileStaging;
  UwfCapability m_uwfCapability;
  FileStagingCoordinator& m_stagingCoordinator;
  bool m_actionActive = false;
};

}  // namespace uwf::ui
