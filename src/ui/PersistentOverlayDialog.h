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

#include <QDialog>

#include "../core/UwfModel.h"
#include "../uwf/api/PersistentOverlayCommands.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace uwf::ui {

// The native CLI report is deliberately kept verbatim: it is localized by
// Windows and is not a stable protocol for deciding persistence/reset state.
class PersistentOverlayDialog final : public QDialog {
  Q_OBJECT
 public:
  explicit PersistentOverlayDialog(api::PersistentOverlayCommands& commands, const core::UwfSnapshot& snapshot, QWidget* parent = nullptr);

 signals:
  void configurationChanged();
  // The host owns the destructive confirmation, fresh-snapshot checks and
  // power request. This dialog must never stage a reset before that approval.
  void restoreAndRestartRequested();

 private:
  void refreshConfiguration();
  void updateActions();
  void runAction(api::PersistentOverlayAction action);
  [[nodiscard]] QString mutationBlocker() const;
  [[nodiscard]] QString enableBlocker() const;
  [[nodiscard]] QString resetBlocker() const;
  void setFeedback(const QString& message, bool failed);

  api::PersistentOverlayCommands& m_commands;
  const core::UwfSnapshot m_snapshot;
  QLabel* m_prerequisiteLabel = nullptr;
  QLabel* m_reportStatusLabel = nullptr;
  QLabel* m_feedbackLabel = nullptr;
  QPlainTextEdit* m_report = nullptr;
  QPushButton* m_enableButton = nullptr;
  QPushButton* m_disableButton = nullptr;
  QPushButton* m_resetButton = nullptr;
  QPushButton* m_cancelResetButton = nullptr;
  QPushButton* m_restoreButton = nullptr;
};

}  // namespace uwf::ui
