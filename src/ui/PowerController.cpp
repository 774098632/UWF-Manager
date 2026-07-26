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
#include "PowerController.h"

#include <QScopeGuard>
#include <QTimer>
#include <QWidget>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include "../util/Log.h"
#include "../uwf/FileStagingTask.h"
#include "../uwf/api/UwfFilter.h"
#include "Dialogs.h"
#include "FileStagingPresentation.h"
#include "I18n.h"
#include "PowerConfirmDialog.h"

namespace uwf::ui {

using dialogs::warning;

PowerController::PowerController(PowerControllerServices services, QWidget* dialogParent, QObject* parent)
    : QObject(parent),
      m_dialogParent(dialogParent),
      m_session(services.session),
      m_fileStaging(services.fileStaging),
      m_uwfCapability(services.uwfCapability),
      m_stagingCoordinator(services.stagingCoordinator),
      m_preshutdown(std::move(services.preshutdown)) {}

void PowerController::safeShutdown() { execute(PowerAction::Shutdown); }

void PowerController::safeRestart() { execute(PowerAction::Restart); }

void PowerController::execute(const PowerAction action) {
  if (m_actionActive) return;
  m_actionActive = true;
  bool deferred = false;
  const auto releaseAction = qScopeGuard([&] {
    if (!deferred) m_actionActive = false;
  });
  try {
    auto batch = m_stagingCoordinator.reserveExternalBatch([this, action](const FileStagingBatchResult& result, FileStagingCoordinator::ExternalBatch batch) {
      // 协调器会在发布同一批次的所有完成通知时调用这里。把交互式电源对话框
      // 排到下一轮事件循环，不能在协调器发布其它已等待命令的回调栈中进入
      // 模态事件循环。
      try {
        QTimer::singleShot(0, this, [this, action, result, batch = std::move(batch)]() mutable {
          const auto releaseAction = qScopeGuard([this] { m_actionActive = false; });
          executeWithCompletedStaging(action, result, std::move(batch));
        });
      } catch (...) {
        // 投递失败时 batch 的 RAII 析构会把执行权交还协调器；同时恢复动作门，
        // 否则本次基础设施异常会永久禁用后续安全关机/重启。
        m_actionActive = false;
        throw;
      }
    });
    if (!batch) {
      deferred = true;
      return;
    }
    executeReserved(action, std::move(*batch));
  } catch (const std::exception& error) {
    reportPowerFailure(action, error);
  } catch (...) {
    reportUnknownPowerFailure(action);
  }
}

void PowerController::executeReserved(const PowerAction action, FileStagingCoordinator::ExternalBatch batch) {
  std::optional<QList<app::FileStagingEntry>> stagedEntries;
  std::optional<app::ApplicationCommandResult> stagingResult;
  // 只有形成最终批次结果后才完成租约。用户在提交前取消时，租约析构会把
  // 等待中的第二实例请求交给普通命令批次。
  const auto completeBatch = qScopeGuard([&] {
    if (stagingResult) batch.complete({*stagingResult, stagedEntries});
  });

  std::optional<FileStagingTask> stagingTask;
  try {
    const auto advanceStaging = [&]() -> PowerStagingProgress {
      try {
        if (!stagingTask || !stagingTask->preparationFinished()) throw std::logic_error("file staging task is unavailable");
        FileStagingTaskProgress progress;
        if (!stagingTask->finished()) progress = stagingTask->advance();
        if (!stagingTask->finished()) {
          return {PowerStagingState::InProgress, progress.processedFiles, progress.totalFiles, progress.currentPath, {}};
        }

        const auto& result = stagingTask->result();
        stagingResult = toApplicationCommandResult(result);
        UWF_LOG_I("power") << "automatic file staging completed: discovered=" << result.discoveredFiles << " committed=" << result.committedFiles
                           << " skippedFiles=" << result.skippedFiles << " skippedEntries=" << result.skippedEntries << " failures=" << result.failures.size();
        if (result.succeeded()) {
          return {PowerStagingState::Completed, progress.processedFiles, progress.totalFiles, progress.currentPath, {}};
        }

        for (const auto& failure : result.failures) {
          UWF_LOG_E("power") << "automatic file staging failed: path=" << failure.path.toStdString()
                             << " error=" << fileStagingFailureReason(failure).toStdString();
        }
        return {PowerStagingState::Failed, progress.processedFiles, progress.totalFiles, progress.currentPath, renderFileStagingResult(result)};
      } catch (const std::exception& error) {
        const QString detail = I18n::tr("Automatic file staging stopped unexpectedly:\n%1").arg(QString::fromUtf8(error.what()));
        stagingResult = app::ApplicationCommandResult::infrastructureFailure(detail);
        throw;
      } catch (...) {
        const QString detail = I18n::tr("Automatic file staging stopped because of an unknown error.");
        stagingResult = app::ApplicationCommandResult::infrastructureFailure(detail);
        throw;
      }
    };

    PowerActionDialogRequest dialogRequest{
        action, [&]() -> std::optional<PowerStagingPreparation> {
          try {
            if (!stagedEntries) {
              try {
                stagedEntries = m_fileStaging.load();
              } catch (const std::exception& error) {
                const QString detail = I18n::tr("The file staging list could not be read:\n%1").arg(QString::fromUtf8(error.what()));
                stagingResult = app::ApplicationCommandResult::infrastructureFailure(detail);
                return PowerStagingFailure{detail};
              } catch (...) {
                const QString detail = I18n::tr("The file staging list could not be read because of an unknown error.");
                stagingResult = app::ApplicationCommandResult::infrastructureFailure(detail);
                return PowerStagingFailure{detail};
              }
              if (stagedEntries->isEmpty()) {
                stagingResult = app::ApplicationCommandResult{};
                return PowerStagingNotRequired{};
              }
              stagingTask.emplace(m_session, *stagedEntries, m_uwfCapability);
            }

            if (!stagingTask) throw std::logic_error("file staging task was not created for a non-empty staging list");
            if (!stagingTask->pollPreparation()) return std::nullopt;
            const std::size_t filesToCommit = stagingTask->totalFiles();
            UWF_LOG_I("power") << "automatic file staging prepared: files=" << filesToCommit;
            if (stagingTask->finished()) {
              const auto status = advanceStaging();
              if (status.state == PowerStagingState::Failed) return PowerStagingFailure{std::move(status.failureDetails)};
              return PowerStagingNotRequired{};
            }
            return PowerStagingCommit{filesToCommit, advanceStaging};
          } catch (const std::exception& error) {
            UWF_LOG_E("power") << "automatic file staging preparation failed: error=" << error.what();
            const QString detail = I18n::tr("Automatic file staging stopped unexpectedly:\n%1").arg(QString::fromUtf8(error.what()));
            stagingResult = app::ApplicationCommandResult::infrastructureFailure(detail);
            return PowerStagingFailure{detail};
          } catch (...) {
            UWF_LOG_E("power") << "automatic file staging preparation failed: error=unknown";
            const QString detail = I18n::tr("Automatic file staging stopped because of an unknown error.");
            stagingResult = app::ApplicationCommandResult::infrastructureFailure(detail);
            return PowerStagingFailure{detail};
          }
        }};

    const auto dialogOutcome = runPowerActionDialog(m_dialogParent, std::move(dialogRequest));
    bool continuationApproved = false;
    switch (dialogOutcome) {
      case PowerActionDialogOutcome::Canceled:
        return;
      case PowerActionDialogOutcome::CanceledAfterStagingFailure:
        UWF_LOG_I("power") << "safe power action canceled after file staging failure";
        return;
      case PowerActionDialogOutcome::ContinuedAfterStagingFailure:
        UWF_LOG_W("power") << "safe power action continuing after user accepted file staging failures";
        if (!stagingResult) throw std::logic_error("safe power action has no file staging failure to approve");
        if (stagingResult->failedFiles == 0) throw std::logic_error("cannot approve a file staging failure without a recorded failed target");
        continuationApproved = true;
        break;
      case PowerActionDialogOutcome::Confirmed:
        break;
    }

    if (!stagingResult) throw std::logic_error("safe power action has no final file staging result");
    if (stagingResult->outcome != app::ApplicationCommandOutcome::Succeeded && !continuationApproved) {
      throw std::logic_error("safe power action cannot continue without a completed staging result or explicit user approval");
    }
    invokePowerAction(action);
  } catch (const std::exception& error) {
    reportPowerFailure(action, error);
  } catch (...) {
    reportUnknownPowerFailure(action);
  }
}

void PowerController::executeWithCompletedStaging(const PowerAction action, FileStagingBatchResult stagingResult, FileStagingCoordinator::ExternalBatch batch) {
  bool completeBatch = false;
  const auto releaseBatch = qScopeGuard([&] {
    if (completeBatch) batch.complete(stagingResult);
  });
  try {
    PowerActionDialogRequest request{action, {}, std::nullopt};
    if (stagingResult.command.outcome != app::ApplicationCommandOutcome::Succeeded) {
      request.completedStagingFailure =
          PowerStagingFailure{stagingResult.command.detail.isEmpty()
                                  ? (stagingResult.command.completed() ? I18n::tr("All staged targets were processed; one or more operations failed.")
                                                                       : I18n::tr("Automatic file staging stopped because of an unknown error."))
                                  : stagingResult.command.detail};
    }

    const auto outcome = runPowerActionDialog(m_dialogParent, std::move(request));
    bool continuationApproved = false;
    if (outcome == PowerActionDialogOutcome::Canceled || outcome == PowerActionDialogOutcome::CanceledAfterStagingFailure) {
      // 已完整处理的批次仍可满足确认期间到达的第二实例命令；基础设施失败
      // 则释放租约，让等待方重新执行，不能把用户取消当成关机放行授权。
      completeBatch = stagingResult.command.completed();
      return;
    }
    if (outcome == PowerActionDialogOutcome::ContinuedAfterStagingFailure) {
      UWF_LOG_W("power") << "safe power action continuing after user accepted file staging failures";
      if (stagingResult.command.failedFiles == 0) throw std::logic_error("cannot approve a file staging failure without a recorded failed target");
      continuationApproved = true;
    }
    if (stagingResult.command.outcome != app::ApplicationCommandOutcome::Succeeded && !continuationApproved) {
      throw std::logic_error("safe power action cannot continue without a completed staging result or explicit user approval");
    }
    completeBatch = true;
    invokePowerAction(action);
  } catch (const std::exception& error) {
    reportPowerFailure(action, error);
  } catch (...) {
    reportUnknownPowerFailure(action);
  }
}

void PowerController::invokePowerAction(const PowerAction action) {
  using ControlResult = PowerControllerServices::PreshutdownControlResult;
  const ControlResult skipResult = m_preshutdown.markHandled ? m_preshutdown.markHandled() : ControlResult::NotApplicable;
  if (skipResult == ControlResult::Unacknowledged) {
    UWF_LOG_W("power") << "enhanced mode preshutdown skip token was not acknowledged; service fallback remains authoritative";
  }
  auto revokeSkipOnFailure = qScopeGuard([&] {
    if (skipResult != ControlResult::NotApplicable && m_preshutdown.markRequired && m_preshutdown.markRequired() == ControlResult::Unacknowledged) {
      UWF_LOG_W("power") << "enhanced mode preshutdown skip token revocation was not acknowledged";
    }
  });
  // 提交文件可能持续一段时间。真正调用电源方法前重新读取 Filter，避免沿用
  // 对话框打开前取得的对象身份和状态快照。
  api::UwfFilter filter(m_session);
  const auto row = filter.read();
  if (action == PowerAction::Shutdown)
    filter.shutdownSystem(row);
  else
    filter.restartSystem(row);
  revokeSkipOnFailure.dismiss();
}

void PowerController::reportPowerFailure(const PowerAction action, const std::exception& error) {
  const bool shutdown = action == PowerAction::Shutdown;
  UWF_LOG_E("power") << (shutdown ? "safe shutdown failed: error=" : "safe restart failed: error=") << error.what();
  const QString title = shutdown ? I18n::tr("Safe shutdown failed") : I18n::tr("Safe restart failed");
  const QString message =
      shutdown ? I18n::tr("Shutdown failed: %1").arg(QString::fromUtf8(error.what())) : I18n::tr("Restart failed: %1").arg(QString::fromUtf8(error.what()));
  warning(m_dialogParent, title, message);
}

void PowerController::reportUnknownPowerFailure(const PowerAction action) {
  const bool shutdown = action == PowerAction::Shutdown;
  UWF_LOG_E("power") << (shutdown ? "safe shutdown failed: error=non-standard-exception" : "safe restart failed: error=non-standard-exception");
  warning(m_dialogParent, shutdown ? I18n::tr("Safe shutdown failed") : I18n::tr("Safe restart failed"),
          I18n::tr("The operation failed with an unknown error."));
}

}  // namespace uwf::ui
