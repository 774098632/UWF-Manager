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
#include "SystemInformationDialog.h"

#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>
#include <utility>

#include "I18n.h"
#include "ThemeManager.h"

namespace uwf::ui {

SystemInformationDialog::SystemInformationDialog(QString report, QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("systemInformationDialog"));
  setWindowTitle(I18n::tr("System information"));
  resize(900, 680);
  setMinimumSize(680, 480);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 16, 16, 12);
  layout->setSpacing(10);

  auto* notice = new QLabel(
      I18n::tr("This report may contain account names, device identifiers, file paths, and recent application logs. Review it before sharing."), this);
  notice->setWordWrap(true);
  notice->setStyleSheet(QStringLiteral("color: %1;").arg(ThemeManager::instance().color(Sem::FgMuted).name()));
  layout->addWidget(notice);

  auto* editor = new QPlainTextEdit(this);
  editor->setObjectName(QStringLiteral("systemInformationReport"));
  editor->setReadOnly(true);
  editor->setLineWrapMode(QPlainTextEdit::NoWrap);
  editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  editor->setPlainText(std::move(report));
  editor->moveCursor(QTextCursor::Start);
  layout->addWidget(editor, 1);

  auto* buttons = new QDialogButtonBox(this);
  auto* copy = buttons->addButton(I18n::tr("Copy all"), QDialogButtonBox::ActionRole);
  copy->setObjectName(QStringLiteral("systemInformationCopyButton"));
  copy->setAutoDefault(false);
  connect(copy, &QPushButton::clicked, this, [editor] { QGuiApplication::clipboard()->setText(editor->toPlainText()); });
  auto* close = buttons->addButton(I18n::tr("Close"), QDialogButtonBox::AcceptRole);
  close->setDefault(true);
  connect(close, &QPushButton::clicked, this, &QDialog::accept);
  layout->addWidget(buttons);
}

}  // namespace uwf::ui
