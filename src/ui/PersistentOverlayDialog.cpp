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
#include "PersistentOverlayDialog.h"

#include <QColor>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <exception>

#include "Dialogs.h"
#include "I18n.h"
#include "ThemeManager.h"

namespace uwf::ui {
namespace {

QLabel* wrappingLabel(QWidget* parent, const QString& text) {
  auto* label = new QLabel(text, parent);
  label->setWordWrap(true);
  label->setTextFormat(Qt::PlainText);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  return label;
}

bool hasProtectedVolume(const core::SessionSnapshot& session) {
  return std::ranges::any_of(session.volumes, [](const core::VolumeRecord& volume) { return volume.isProtected; });
}

QString successMessage(const api::PersistentOverlayAction action) {
  switch (action) {
    case api::PersistentOverlayAction::Enable:
      return I18n::tr("The persistence enable command completed. Check the native report and restart to apply it.");
    case api::PersistentOverlayAction::Disable:
      return I18n::tr("The persistence disable command completed. Overlay changes will be discarded at the next restart.");
    case api::PersistentOverlayAction::Reset:
      return I18n::tr("The reset command completed. Overlay changes will be discarded on the next boot. Safe restart or enhanced-mode staging may commit "
                      "staged files before reset. Use Restore and restart to skip staging, or cancel this request before restarting.");
    case api::PersistentOverlayAction::CancelReset:
      return I18n::tr("The cancel-reset command completed. Check the native report before restarting.");
    case api::PersistentOverlayAction::GetConfig:
      break;
  }
  return {};
}

}  // namespace

PersistentOverlayDialog::PersistentOverlayDialog(api::PersistentOverlayCommands& commands, const core::UwfSnapshot& snapshot, QWidget* parent)
    : QDialog(parent), m_commands(commands), m_snapshot(snapshot) {
  setObjectName(QStringLiteral("persistentOverlayDialog"));
  setWindowTitle(I18n::tr("Persistent overlay / Restore"));
  setMinimumSize(720, 640);
  resize(800, 720);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(24, 22, 24, 18);
  layout->setSpacing(12);

  auto* summary = wrappingLabel(
      this, I18n::tr("Persistent disk overlay preserves changes to protected volumes across normal restarts. Restore discards overlay changes on the next "
                    "boot. Files and registry values already committed, or written through exclusions, remain on disk."));
  QFont summaryFont = summary->font();
  summaryFont.setBold(true);
  summary->setFont(summaryFont);
  layout->addWidget(summary);

  auto* notice = wrappingLabel(
      this, I18n::tr("Windows marks persistent overlay as experimental. It requires a Disk overlay and uses the configured overlay capacity. Changes "
                    "accumulate between restarts; monitor free overlay space and restore before it fills up."));
  notice->setObjectName(QStringLiteral("persistentOverlayNotice"));
  notice->setStyleSheet(QStringLiteral("color: %1;").arg(ThemeManager::instance().color(Sem::Warn).name()));
  layout->addWidget(notice);

  m_prerequisiteLabel = wrappingLabel(this, {});
  m_prerequisiteLabel->setObjectName(QStringLiteral("persistentOverlayPrerequisites"));
  layout->addWidget(m_prerequisiteLabel);

  auto* reportHeader = new QHBoxLayout();
  auto* reportTitle = new QLabel(I18n::tr("Native Windows overlay configuration"), this);
  reportHeader->addWidget(reportTitle, 1);
  auto* refresh = new QPushButton(I18n::tr("Refresh status"), this);
  refresh->setObjectName(QStringLiteral("persistentOverlayRefreshButton"));
  refresh->setAutoDefault(false);
  reportHeader->addWidget(refresh);
  connect(refresh, &QPushButton::clicked, this, &PersistentOverlayDialog::refreshConfiguration);
  layout->addLayout(reportHeader);

  m_reportStatusLabel = wrappingLabel(this, I18n::tr("Check persistence and any pending reset in the native report below."));
  m_reportStatusLabel->setObjectName(QStringLiteral("persistentOverlayReportStatus"));
  layout->addWidget(m_reportStatusLabel);
  m_report = new QPlainTextEdit(this);
  m_report->setObjectName(QStringLiteral("persistentOverlayNativeReport"));
  m_report->setReadOnly(true);
  m_report->setMinimumHeight(170);
  layout->addWidget(m_report, 1);

  m_feedbackLabel = wrappingLabel(this, {});
  m_feedbackLabel->setObjectName(QStringLiteral("persistentOverlayFeedback"));
  m_feedbackLabel->hide();
  layout->addWidget(m_feedbackLabel);

  auto* actions = new QGridLayout();
  actions->setHorizontalSpacing(12);
  actions->setVerticalSpacing(10);
  const auto addAction = [this, actions](const QString& title, const QString& name, const int row, const int column) {
    auto* button = new QPushButton(title, this);
    button->setObjectName(name);
    button->setAutoDefault(false);
    actions->addWidget(button, row, column);
    return button;
  };
  m_enableButton = addAction(I18n::tr("Enable persistent overlay"), QStringLiteral("persistentOverlayEnableButton"), 0, 0);
  m_disableButton = addAction(I18n::tr("Disable persistent overlay"), QStringLiteral("persistentOverlayDisableButton"), 0, 1);
  m_resetButton = addAction(I18n::tr("Reset on next boot"), QStringLiteral("persistentOverlayResetButton"), 1, 0);
  m_cancelResetButton = addAction(I18n::tr("Cancel scheduled reset"), QStringLiteral("persistentOverlayCancelResetButton"), 1, 1);
  m_restoreButton = addAction(I18n::tr("Restore and restart"), QStringLiteral("persistentOverlayRestoreButton"), 2, 0);
  auto* close = addAction(I18n::tr("Close"), QStringLiteral("persistentOverlayCloseButton"), 2, 1);
  close->setDefault(true);
  connect(close, &QPushButton::clicked, this, &QDialog::reject);
  connect(m_enableButton, &QPushButton::clicked, this, [this] { runAction(api::PersistentOverlayAction::Enable); });
  connect(m_disableButton, &QPushButton::clicked, this, [this] { runAction(api::PersistentOverlayAction::Disable); });
  connect(m_resetButton, &QPushButton::clicked, this, [this] { runAction(api::PersistentOverlayAction::Reset); });
  connect(m_cancelResetButton, &QPushButton::clicked, this, [this] { runAction(api::PersistentOverlayAction::CancelReset); });
  connect(m_restoreButton, &QPushButton::clicked, this, [this] {
    if (!resetBlocker().isEmpty()) return;
    emit restoreAndRestartRequested();
    // The host may have scheduled reset before a restart failure. Re-read its
    // native status so the cancellation action is based on visible evidence.
    refreshConfiguration();
  });
  layout->addLayout(actions);

  updateActions();
  refreshConfiguration();
}

QString PersistentOverlayDialog::mutationBlocker() const {
  if (!m_snapshot.uwfAvailable) return I18n::tr("UWF is unavailable. Persistent overlay commands cannot be applied.");
  if (!m_snapshot.elevated) return I18n::tr("Run UWF Manager as administrator to change persistent overlay settings.");
  return {};
}

QString PersistentOverlayDialog::enableBlocker() const {
  if (const QString blocker = mutationBlocker(); !blocker.isEmpty()) return blocker;
  if (m_snapshot.next.overlay.type != core::OverlayType::Disk)
    return I18n::tr("Apply Disk as the next overlay type before enabling persistence. To change overlay type, disable UWF, apply and restart; then select "
                    "Disk, apply, and enable protection for the next boot.");
  return {};
}

QString PersistentOverlayDialog::resetBlocker() const {
  if (const QString blocker = mutationBlocker(); !blocker.isEmpty()) return blocker;
  if (m_snapshot.current.overlay.type != core::OverlayType::Disk || m_snapshot.next.overlay.type != core::OverlayType::Disk ||
      !m_snapshot.current.filter.enabled || !m_snapshot.next.filter.enabled || !hasProtectedVolume(m_snapshot.current) ||
      !hasProtectedVolume(m_snapshot.next))
    return I18n::tr("Restore requires Disk overlay, UWF enabled, and at least one protected volume in both the current and next sessions. Apply the "
                    "configuration and restart first.");
  return {};
}

void PersistentOverlayDialog::updateActions() {
  const QString baseBlocker = mutationBlocker();
  const QString enablingBlocker = enableBlocker();
  const QString restoringBlocker = resetBlocker();
  m_enableButton->setEnabled(enablingBlocker.isEmpty());
  m_enableButton->setToolTip(enablingBlocker);
  m_disableButton->setEnabled(baseBlocker.isEmpty());
  m_disableButton->setToolTip(baseBlocker);
  m_cancelResetButton->setEnabled(baseBlocker.isEmpty());
  m_cancelResetButton->setToolTip(baseBlocker);
  m_resetButton->setEnabled(restoringBlocker.isEmpty());
  m_resetButton->setToolTip(restoringBlocker);
  m_restoreButton->setEnabled(restoringBlocker.isEmpty());
  m_restoreButton->setToolTip(restoringBlocker);

  QString prerequisites;
  if (!enablingBlocker.isEmpty()) prerequisites = enablingBlocker;
  if (!restoringBlocker.isEmpty() && restoringBlocker != enablingBlocker) {
    if (!prerequisites.isEmpty()) prerequisites += QLatin1Char('\n');
    prerequisites += restoringBlocker;
  }
  m_prerequisiteLabel->setText(prerequisites);
  m_prerequisiteLabel->setVisible(!prerequisites.isEmpty());
}

void PersistentOverlayDialog::refreshConfiguration() {
  try {
    const auto result = m_commands.execute(api::PersistentOverlayAction::GetConfig);
    m_report->setPlainText(result.output);
    m_reportStatusLabel->setText(result.succeeded()
                                     ? I18n::tr("Check persistence and any pending reset in the native report below.")
                                     : I18n::tr("Native configuration could not be read (exit code %1).").arg(result.exitCode));
    m_reportStatusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(
        ThemeManager::instance().color(result.succeeded() ? Sem::FgMuted : Sem::Danger).name()));
  } catch (const std::exception& error) {
    m_report->clear();
    m_reportStatusLabel->setText(I18n::tr("Native configuration could not be read:\n%1").arg(QString::fromUtf8(error.what())));
    m_reportStatusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ThemeManager::instance().color(Sem::Danger).name()));
  } catch (...) {
    m_report->clear();
    m_reportStatusLabel->setText(I18n::tr("Native configuration could not be read because of an unknown error."));
    m_reportStatusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ThemeManager::instance().color(Sem::Danger).name()));
  }
}

void PersistentOverlayDialog::setFeedback(const QString& message, const bool failed) {
  m_feedbackLabel->setText(message);
  m_feedbackLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ThemeManager::instance().color(failed ? Sem::Danger : Sem::AddOk).name()));
  m_feedbackLabel->setVisible(!message.isEmpty());
}

void PersistentOverlayDialog::runAction(const api::PersistentOverlayAction action) {
  const QString blocker = action == api::PersistentOverlayAction::Enable   ? enableBlocker()
                          : action == api::PersistentOverlayAction::Reset ? resetBlocker()
                                                                          : mutationBlocker();
  if (!blocker.isEmpty()) {
    setFeedback(blocker, true);
    return;
  }
  if (action == api::PersistentOverlayAction::Disable &&
      !dialogs::confirm(this, I18n::tr("Disable persistent overlay"),
                        I18n::tr("Disabling persistence discards overlay changes at the next restart. Files and registry values already committed, or "
                                 "written through exclusions, remain on disk. Continue?")))
    return;
  if (action == api::PersistentOverlayAction::Reset &&
      !dialogs::confirm(this, I18n::tr("Reset on next boot"),
                        I18n::tr("Discard all changes in the protected-volume overlay on the next boot? Files and registry values already committed, or "
                                 "written through exclusions, remain on disk. Safe restart or enhanced-mode automatic staging may commit staged files "
                                 "before reset. Use Restore and restart to skip staging. You can cancel this reset before restarting.")))
    return;

  try {
    const auto result = m_commands.execute(action);
    QString feedback = result.succeeded() ? successMessage(action) : I18n::tr("Persistent overlay command failed (exit code %1).").arg(result.exitCode);
    if (!result.output.isEmpty()) feedback += QLatin1Char('\n') + result.output;
    setFeedback(feedback, !result.succeeded());
    if (result.succeeded()) {
      refreshConfiguration();
      emit configurationChanged();
    }
  } catch (const std::exception& error) {
    setFeedback(I18n::tr("Persistent overlay command failed:\n%1").arg(QString::fromUtf8(error.what())), true);
  } catch (...) {
    setFeedback(I18n::tr("Persistent overlay command failed because of an unknown error."), true);
  }
}

}  // namespace uwf::ui
