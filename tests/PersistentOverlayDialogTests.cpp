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
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>
#include <stdexcept>
#include <vector>

#include "ui/I18n.h"
#include "ui/PersistentOverlayDialog.h"

namespace {

using Action = uwf::api::PersistentOverlayAction;
using Dialog = uwf::ui::PersistentOverlayDialog;

class MemoryCommands final : public uwf::api::PersistentOverlayCommands {
 public:
  std::vector<Action> actions;
  QString nativeReport = QStringLiteral("Persistent mode: ON\nReset requested: OFF\n");
  uwf::api::PersistentOverlayCommandResult mutationResult{0, QStringLiteral("Native command completed")};
  bool throwOnMutation = false;

  uwf::api::PersistentOverlayCommandResult execute(const Action action) override {
    actions.push_back(action);
    if (action == Action::GetConfig) return {0, nativeReport};
    if (throwOnMutation) throw std::runtime_error("mock transport error");
    return mutationResult;
  }
};

uwf::core::UwfSnapshot protectedDiskSnapshot() {
  uwf::core::UwfSnapshot snapshot;
  snapshot.uwfAvailable = true;
  snapshot.elevated = true;
  snapshot.current.overlay.type = uwf::core::OverlayType::Disk;
  snapshot.next.overlay.type = uwf::core::OverlayType::Disk;
  snapshot.current.filter.enabled = true;
  snapshot.next.filter.enabled = true;
  snapshot.current.volumes.push_back({"Volume{current}", "C:", true});
  snapshot.next.volumes.push_back({"Volume{current}", "C:", true});
  return snapshot;
}

QPushButton* actionButton(Dialog& dialog, const char* action) {
  return dialog.findChild<QPushButton*>(QStringLiteral("persistentOverlay%1Button").arg(QString::fromLatin1(action)));
}

QString dialogText(const QDialog& dialog) {
  QString text;
  for (const auto* label : dialog.findChildren<QLabel*>()) text += label->text() + QLatin1Char('\n');
  return text;
}

class PersistentOverlayDialogTests final : public QObject {
  Q_OBJECT
 private slots:
  void initTestCase();
  void nativeReportIsReadOnlyAndUnmodified();
  void enablingRequiresElevationAndAppliedDiskType();
  void pendingDisableDoesNotAllowChangingPersistence();
  void resetRequiresProtectionInBothSessions();
  void cancelledDestructiveActionsDoNotExecute();
  void successfulMutationRefreshesAndSignals();
  void failedMutationPreservesNativeReport();
  void transportExceptionIsDisplayedWithoutChangingConfiguration();
  void cancelResetRemainsAvailableWithoutResetPrerequisites();
  void restoreOnlyEmitsIntent();
  void resetConfirmationExplainsStagingAndCommittedData();
};

void PersistentOverlayDialogTests::initTestCase() { uwf::ui::I18n::instance().setLang(uwf::ui::I18n::Lang::En); }

void PersistentOverlayDialogTests::nativeReportIsReadOnlyAndUnmodified() {
  MemoryCommands commands;
  commands.nativeReport = QString::fromUtf8("原生报告：持久化关闭\nReset: ON\n<enabled>true</enabled>\n");
  Dialog dialog(commands, protectedDiskSnapshot());
  const auto* report = dialog.findChild<QPlainTextEdit*>(QStringLiteral("persistentOverlayNativeReport"));
  QVERIFY(report);
  QVERIFY(report->isReadOnly());
  QCOMPARE(report->toPlainText(), commands.nativeReport);
  // Report language or contradictory words must not infer command state.
  QVERIFY(!actionButton(dialog, "Enable")->isEnabled());
  QVERIFY(!actionButton(dialog, "Disable")->isEnabled());
  QVERIFY(actionButton(dialog, "CancelReset")->isEnabled());
  QCOMPARE(commands.actions, std::vector<Action>{Action::GetConfig});
}

void PersistentOverlayDialogTests::enablingRequiresElevationAndAppliedDiskType() {
  MemoryCommands commands;
  auto snapshot = protectedDiskSnapshot();
  snapshot.elevated = false;
  Dialog unelevated(commands, snapshot);
  QVERIFY(!actionButton(unelevated, "Enable")->isEnabled());
  actionButton(unelevated, "Enable")->click();
  QCOMPARE(commands.actions.size(), std::size_t{1});

  snapshot.elevated = true;
  snapshot.next.overlay.type = uwf::core::OverlayType::RAM;
  Dialog pendingRam(commands, snapshot);
  QVERIFY(!actionButton(pendingRam, "Enable")->isEnabled());
  QVERIFY(dialogText(pendingRam).contains(QStringLiteral("disable UWF")));
  actionButton(pendingRam, "Enable")->click();
  QCOMPARE(commands.actions.size(), std::size_t{2});

  // A scheduled Disk session may be enabled while the current session is RAM.
  snapshot.current.overlay.type = uwf::core::OverlayType::RAM;
  snapshot.current.filter.enabled = false;
  snapshot.next.overlay.type = uwf::core::OverlayType::Disk;
  Dialog pendingDisk(commands, snapshot);
  QVERIFY(actionButton(pendingDisk, "Enable")->isEnabled());
  QVERIFY(!actionButton(pendingDisk, "Reset")->isEnabled());
}

void PersistentOverlayDialogTests::pendingDisableDoesNotAllowChangingPersistence() {
  MemoryCommands commands;
  auto snapshot = protectedDiskSnapshot();
  // Applying a pending disable has not turned off this session's driver yet.
  snapshot.next.filter.enabled = false;
  Dialog dialog(commands, snapshot);
  QSignalSpy changed(&dialog, &Dialog::configurationChanged);
  for (const char* action : {"Enable", "Disable"}) {
    auto* button = actionButton(dialog, action);
    QVERIFY(button);
    QVERIFY(!button->isEnabled());
    QVERIFY(button->toolTip().contains(QStringLiteral("disable UWF, apply and restart")));
    button->click();
    QCOMPARE(commands.actions, std::vector<Action>{Action::GetConfig});

    // The action handler also refuses a stale or externally enabled button.
    bool confirmationShown = false;
    QTimer confirmationGuard;
    confirmationGuard.setSingleShot(true);
    connect(&confirmationGuard, &QTimer::timeout, &dialog, [&confirmationShown] {
      if (auto* confirmation = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
        confirmationShown = true;
        confirmation->reject();
      }
    });
    confirmationGuard.start(0);
    button->setEnabled(true);
    button->click();
    confirmationGuard.stop();
    QVERIFY(!confirmationShown);
    QCOMPARE(commands.actions, std::vector<Action>{Action::GetConfig});
  }
  QCOMPARE(changed.count(), 0);
  QVERIFY(dialogText(dialog).contains(QStringLiteral("Uncommitted overlay changes may be lost")));
}

void PersistentOverlayDialogTests::resetRequiresProtectionInBothSessions() {
  MemoryCommands commands;
  const auto ensureBlocked = [&commands](const uwf::core::UwfSnapshot& snapshot) {
    Dialog dialog(commands, snapshot);
    QVERIFY(!actionButton(dialog, "Reset")->isEnabled());
    QVERIFY(!actionButton(dialog, "Restore")->isEnabled());
  };
  auto snapshot = protectedDiskSnapshot();
  snapshot.next.filter.enabled = false;
  ensureBlocked(snapshot);
  snapshot = protectedDiskSnapshot();
  snapshot.current.volumes.front().isProtected = false;
  ensureBlocked(snapshot);
  snapshot = protectedDiskSnapshot();
  snapshot.next.volumes.clear();
  ensureBlocked(snapshot);
  snapshot = protectedDiskSnapshot();
  snapshot.uwfAvailable = false;
  ensureBlocked(snapshot);
  for (const auto action : commands.actions) QCOMPARE(action, Action::GetConfig);
}

void PersistentOverlayDialogTests::cancelledDestructiveActionsDoNotExecute() {
  MemoryCommands commands;
  for (const char* action : {"Reset", "Disable"}) {
    auto snapshot = protectedDiskSnapshot();
    if (QString::fromLatin1(action) == QStringLiteral("Disable")) snapshot.current.filter.enabled = false;
    Dialog dialog(commands, snapshot);
    QSignalSpy changed(&dialog, &Dialog::configurationChanged);
    bool inspected = false;
    QTimer::singleShot(0, &dialog, [&inspected] {
      if (auto* confirmation = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
        inspected = true;
        confirmation->reject();
      }
    });
    actionButton(dialog, action)->click();
    QVERIFY(inspected);
    QCOMPARE(changed.count(), 0);
  }
  QCOMPARE(commands.actions, (std::vector<Action>{Action::GetConfig, Action::GetConfig}));
}

void PersistentOverlayDialogTests::successfulMutationRefreshesAndSignals() {
  MemoryCommands commands;
  auto snapshot = protectedDiskSnapshot();
  snapshot.current.filter.enabled = false;
  Dialog dialog(commands, snapshot);
  QSignalSpy changed(&dialog, &Dialog::configurationChanged);
  commands.nativeReport = QStringLiteral("Next persistent overlay: ON\n");
  actionButton(dialog, "Enable")->click();
  QCOMPARE(commands.actions, (std::vector<Action>{Action::GetConfig, Action::Enable, Action::GetConfig}));
  QCOMPARE(changed.count(), 1);
  QCOMPARE(dialog.findChild<QPlainTextEdit*>(QStringLiteral("persistentOverlayNativeReport"))->toPlainText(), commands.nativeReport);
  QVERIFY(dialogText(dialog).contains(QStringLiteral("Native command completed")));
}

void PersistentOverlayDialogTests::failedMutationPreservesNativeReport() {
  MemoryCommands commands;
  auto snapshot = protectedDiskSnapshot();
  snapshot.current.filter.enabled = false;
  Dialog dialog(commands, snapshot);
  QSignalSpy changed(&dialog, &Dialog::configurationChanged);
  commands.mutationResult = {5, QStringLiteral("Access denied")};
  actionButton(dialog, "Enable")->click();
  QCOMPARE(changed.count(), 0);
  QCOMPARE(commands.actions, (std::vector<Action>{Action::GetConfig, Action::Enable}));
  QCOMPARE(dialog.findChild<QPlainTextEdit*>(QStringLiteral("persistentOverlayNativeReport"))->toPlainText(), commands.nativeReport);
  QVERIFY(dialogText(dialog).contains(QStringLiteral("exit code 5")));
  QVERIFY(dialogText(dialog).contains(QStringLiteral("Access denied")));
}

void PersistentOverlayDialogTests::transportExceptionIsDisplayedWithoutChangingConfiguration() {
  MemoryCommands commands;
  auto snapshot = protectedDiskSnapshot();
  snapshot.current.filter.enabled = false;
  Dialog dialog(commands, snapshot);
  QSignalSpy changed(&dialog, &Dialog::configurationChanged);
  commands.throwOnMutation = true;
  actionButton(dialog, "Enable")->click();
  QCOMPARE(changed.count(), 0);
  QVERIFY(dialogText(dialog).contains(QStringLiteral("mock transport error")));
}

void PersistentOverlayDialogTests::cancelResetRemainsAvailableWithoutResetPrerequisites() {
  MemoryCommands commands;
  auto snapshot = protectedDiskSnapshot();
  snapshot.current.overlay.type = uwf::core::OverlayType::RAM;
  snapshot.next.filter.enabled = false;
  snapshot.next.volumes.clear();
  Dialog dialog(commands, snapshot);
  QVERIFY(!actionButton(dialog, "Reset")->isEnabled());
  QVERIFY(actionButton(dialog, "CancelReset")->isEnabled());
  actionButton(dialog, "CancelReset")->click();
  QCOMPARE(commands.actions, (std::vector<Action>{Action::GetConfig, Action::CancelReset, Action::GetConfig}));
}

void PersistentOverlayDialogTests::restoreOnlyEmitsIntent() {
  MemoryCommands commands;
  Dialog dialog(commands, protectedDiskSnapshot());
  QSignalSpy restored(&dialog, &Dialog::restoreAndRestartRequested);
  QSignalSpy changed(&dialog, &Dialog::configurationChanged);
  actionButton(dialog, "Restore")->click();
  QCOMPARE(restored.count(), 1);
  QCOMPARE(changed.count(), 0);
  // Power and reset remain entirely under the host's guarded confirmation.
  // The second read reports a reset that the host may already have scheduled.
  QCOMPARE(commands.actions, (std::vector<Action>{Action::GetConfig, Action::GetConfig}));
}

void PersistentOverlayDialogTests::resetConfirmationExplainsStagingAndCommittedData() {
  MemoryCommands commands;
  Dialog dialog(commands, protectedDiskSnapshot());
  QSignalSpy changed(&dialog, &Dialog::configurationChanged);
  QString confirmationText;
  QTimer::singleShot(0, &dialog, [&confirmationText] {
    if (auto* confirmation = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
      confirmationText = dialogText(*confirmation);
      confirmation->accept();
    }
  });
  actionButton(dialog, "Reset")->click();
  QVERIFY(confirmationText.contains(QStringLiteral("Safe restart")));
  QVERIFY(confirmationText.contains(QStringLiteral("already committed")));
  QVERIFY(confirmationText.contains(QStringLiteral("exclusions")));
  QVERIFY(confirmationText.contains(QStringLiteral("Restore and restart")));
  QCOMPARE(changed.count(), 1);
  QCOMPARE(commands.actions, (std::vector<Action>{Action::GetConfig, Action::Reset, Action::GetConfig}));
}

}  // namespace

QTEST_MAIN(PersistentOverlayDialogTests)
#include "PersistentOverlayDialogTests.moc"
