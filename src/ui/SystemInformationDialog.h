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
#include <QString>

namespace uwf::ui {

// 只读诊断报告窗口。报告采用稳定的英文 key，便于用户直接复制到 issue，
// 而窗口标题、提示与按钮仍跟随界面语言。
class SystemInformationDialog final : public QDialog {
  Q_OBJECT
 public:
  explicit SystemInformationDialog(QString report, QWidget* parent = nullptr);
};

}  // namespace uwf::ui
