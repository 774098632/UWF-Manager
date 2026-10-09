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
#include "PersistentOverlayLibrary.h"

#include <exception>

namespace uwf::api {

namespace {

constexpr std::uint32_t kPersistentFlag = 0x2;
constexpr std::uint32_t kHormFlag = 0x4;
constexpr std::int32_t kAccessDenied = -2147024891;     // 0x80070005
constexpr std::int32_t kInvalidData = -2147024883;      // 0x8007000d
constexpr std::int32_t kInvalidArgument = -2147024809;  // 0x80070057

bool knownResetMode(const std::uint32_t mode) { return mode == 0 || mode == 1 || mode == 255; }

QString hexValue(const std::uint32_t value) {
  return QStringLiteral("0x%1").arg(static_cast<qulonglong>(value), 8, 16, QLatin1Char('0'));
}

QString statusText(const std::int32_t status) {
  return QStringLiteral("%1 (%2)").arg(status).arg(hexValue(static_cast<std::uint32_t>(status)));
}

PersistentOverlayCommandResult failure(const std::int32_t status, const QString& message, const bool writeAttempted = false) {
  QString output = message;
  if (writeAttempted) {
    output += QStringLiteral("\nA write was attempted. Its final outcome is not confirmed; inspect the configuration before retrying.");
  }
  return {static_cast<int>(status), output, true};
}

PersistentOverlayCommandResult operationFailure(const std::int32_t status, const QString& stage, const bool writeAttempted = false) {
  return failure(status, QStringLiteral("%1 failed: HRESULT %2").arg(stage, statusText(status)), writeAttempted);
}

QString flagsReport(const QString& session, const std::uint32_t flags) {
  return QStringLiteral("%1 overlay flags: %2; persistent: %3; read-only media/HORM flag: %4")
      .arg(session, hexValue(flags), (flags & kPersistentFlag) ? QStringLiteral("ON") : QStringLiteral("OFF"),
           (flags & kHormFlag) ? QStringLiteral("ON") : QStringLiteral("OFF"));
}

QString resetReport(const std::uint32_t mode) {
  const QString meaning = mode == 0   ? QStringLiteral("retain persistent state")
                          : mode == 1 ? QStringLiteral("discard persistent state at next boot")
                                      : QStringLiteral("saved mode");
  return QStringLiteral("Persistent overlay reset mode: %1 (%2)").arg(mode).arg(meaning);
}

}  // namespace

PersistentOverlayCommandResult PersistentOverlayLibrary::execute(const PersistentOverlayAction action,
                                                                const PersistentOverlayLibraryContext& context) {
  bool writeAttempted = false;
  QString stage = QStringLiteral("Persistent overlay operation");
  try {
    switch (action) {
      case PersistentOverlayAction::GetConfig: {
        std::uint32_t currentFlags = 0;
        std::uint32_t nextFlags = 0;
        std::uint32_t resetMode = 0;
        stage = QStringLiteral("Get current overlay flags");
        if (const auto status = m_operations.getFlags(true, currentFlags); status != 0) return operationFailure(status, stage);
        stage = QStringLiteral("Get next overlay flags");
        if (const auto status = m_operations.getFlags(false, nextFlags); status != 0) return operationFailure(status, stage);
        stage = QStringLiteral("Get persistent overlay reset mode");
        if (const auto status = m_operations.getReset(resetMode); status != 0) return operationFailure(status, stage);
        const QString report = flagsReport(QStringLiteral("Current"), currentFlags) + QLatin1Char('\n') +
                               flagsReport(QStringLiteral("Next"), nextFlags) + QLatin1Char('\n');
        if (!knownResetMode(resetMode))
          return failure(kInvalidData, report + QStringLiteral("Unknown persistent overlay reset mode: %1").arg(resetMode));
        return {0, report + resetReport(resetMode), false};
      }
      case PersistentOverlayAction::Enable:
      case PersistentOverlayAction::Disable: {
        if (context.currentEnabled || !context.nextDisk)
          return failure(kAccessDenied, QStringLiteral("Changing persistence requires UWF disabled in the current session and Disk overlay for the next session."));
        std::uint32_t nextFlags = 0;
        stage = QStringLiteral("Get next overlay flags before changing persistence");
        if (const auto status = m_operations.getFlags(false, nextFlags); status != 0) return operationFailure(status, stage);
        if (nextFlags & kHormFlag)
          return failure(kAccessDenied, QStringLiteral("Changing persistence with the read-only media/HORM flag is unsupported."));
        const std::uint32_t plannedFlags = action == PersistentOverlayAction::Enable ? nextFlags | kPersistentFlag : nextFlags & ~kPersistentFlag;
        stage = QStringLiteral("Set next overlay flags");
        writeAttempted = true;
        if (const auto status = m_operations.setFlags(plannedFlags); status != 0) return operationFailure(status, stage, writeAttempted);
        std::uint32_t confirmedFlags = 0;
        stage = QStringLiteral("Confirm next overlay flags after changing persistence");
        if (const auto status = m_operations.getFlags(false, confirmedFlags); status != 0) return operationFailure(status, stage, writeAttempted);
        if (confirmedFlags != plannedFlags)
          return failure(kInvalidData, QStringLiteral("Next overlay flags did not match the requested value: expected %1; observed %2.")
                                          .arg(hexValue(plannedFlags), hexValue(confirmedFlags)),
                         writeAttempted);
        return {0, flagsReport(QStringLiteral("Confirmed next"), confirmedFlags), false};
      }
      case PersistentOverlayAction::Reset:
      case PersistentOverlayAction::CancelReset: {
        if (action == PersistentOverlayAction::Reset) {
          if (!context.currentEnabled || !context.nextEnabled || !context.currentDisk || !context.nextDisk || !context.currentProtected ||
              !context.nextProtected)
            return failure(kAccessDenied, QStringLiteral("Restore requires UWF enabled, Disk overlay, and a protected volume in both current and next sessions."));
          std::uint32_t currentFlags = 0;
          std::uint32_t nextFlags = 0;
          stage = QStringLiteral("Get current overlay flags before restoring");
          if (const auto status = m_operations.getFlags(true, currentFlags); status != 0) return operationFailure(status, stage);
          stage = QStringLiteral("Get next overlay flags before restoring");
          if (const auto status = m_operations.getFlags(false, nextFlags); status != 0) return operationFailure(status, stage);
          if (!(currentFlags & kPersistentFlag) || !(nextFlags & kPersistentFlag))
            return failure(kAccessDenied, QStringLiteral("Restore requires persistent overlay already enabled in both current and next sessions."));
          if ((currentFlags | nextFlags) & kHormFlag)
            return failure(kAccessDenied, QStringLiteral("Restoring persistent overlay with the read-only media/HORM flag is unsupported."));
        }
        // Cancellation remains available after pending session settings change.
        // The native adapter must still verify an existing accessible device.
        std::uint32_t previousMode = 0;
        stage = QStringLiteral("Get persistent overlay reset mode before changing it");
        if (const auto status = m_operations.getReset(previousMode); status != 0) return operationFailure(status, stage);
        if (!knownResetMode(previousMode))
          return failure(kInvalidData, QStringLiteral("Unknown persistent overlay reset mode: %1. No write was attempted.").arg(previousMode));
        // Never pass 255 to the setter: it invokes the saved-mode operation.
        const std::uint32_t plannedMode = action == PersistentOverlayAction::Reset ? 1 : 0;
        stage = QStringLiteral("Set persistent overlay reset mode");
        writeAttempted = true;
        if (const auto status = m_operations.setReset(plannedMode); status != 0) return operationFailure(status, stage, writeAttempted);
        std::uint32_t confirmedMode = 0;
        stage = QStringLiteral("Confirm persistent overlay reset mode after changing it");
        if (const auto status = m_operations.getReset(confirmedMode); status != 0) return operationFailure(status, stage, writeAttempted);
        if (confirmedMode != plannedMode)
          return failure(kInvalidData, QStringLiteral("Persistent overlay reset mode did not match the requested value: expected %1; observed %2.")
                                          .arg(plannedMode)
                                          .arg(confirmedMode),
                         writeAttempted);
        return {0, resetReport(confirmedMode), false};
      }
    }
    return failure(kInvalidArgument, QStringLiteral("Persistent overlay action is invalid. No write was attempted."));
  } catch (const std::exception& error) {
    return failure(-1, QStringLiteral("%1 could not complete: %2").arg(stage, QString::fromUtf8(error.what())), writeAttempted);
  } catch (...) {
    return failure(-1, QStringLiteral("%1 could not complete: unexpected exception.").arg(stage), writeAttempted);
  }
}

}  // namespace uwf::api
