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

#include <QList>
#include <QStringList>
#include <QWidget>

#include "../app/FileStagingStore.h"

namespace uwf::app {
class FileStagingConflictPolicy;
}

class QAction;
class QEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace uwf::ui {

class RoundedCornerOverlay;
namespace dialogs {
class FileDialogProvider;
}

// 安全关机/重启前自动提交的持久化路径列表。每个磁盘页只呈现本卷条目，但
// FileStagingStore 仍保存全部卷的统一快照；修改本卷时必须保留其它卷条目。
// 修改会立即写入存储，不进入 UWF 配置的“待应用”计划。
class FileStagingWidget : public QWidget {
  Q_OBJECT
 public:
  FileStagingWidget(QString driveLetter, app::FileStagingStore& store, QWidget* parent = nullptr);
  FileStagingWidget(QString driveLetter, app::FileStagingStore& store, dialogs::FileDialogProvider& fileDialogs, QWidget* parent = nullptr);
  FileStagingWidget(QString driveLetter, app::FileStagingStore& store, dialogs::FileDialogProvider& fileDialogs, app::FileStagingConflictPolicy& conflicts,
                    QWidget* parent = nullptr);

  [[nodiscard]] QString driveLetter() const { return m_driveLetter; }
  // 只读状态只禁止修改持久化列表；筛选、选择、复制和打开所在目录仍可用。
  // 存储读取失败是另一条状态，不得用 setEnabled(false) 把两者混为一谈。
  void setReadOnly(bool readOnly);

 signals:
  void copiedToClipboard(const QString& hint);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private slots:
  void addFiles();
  void addDirectory();
  void removeSelected();
  void filterChanged(const QString& text);
  void copySelectedPath(QListWidgetItem* item);

 private:
  FileStagingWidget(QString driveLetter, app::FileStagingStore& store, dialogs::FileDialogProvider& fileDialogs, app::FileStagingConflictPolicy* conflicts,
                    QWidget* parent);
  void load();
  void addEntries(const QStringList& paths, app::FileStagingKind kind);
  void persist(const QList<app::FileStagingEntry>& entries);
  void rebuild();
  void refreshThemedIcons();
  void updateModificationControls();
  void setStorageUnavailable(const QString& error);
  void reportUpdateFailure(const QString& error);

  QString m_driveLetter;
  app::FileStagingStore& m_store;
  dialogs::FileDialogProvider& m_fileDialogs;
  app::FileStagingConflictPolicy* m_conflicts = nullptr;
  QList<app::FileStagingEntry> m_entries;
  QLineEdit* m_filter = nullptr;
  QListWidget* m_list = nullptr;
  QLabel* m_summary = nullptr;
  QPushButton* m_addButton = nullptr;
  QPushButton* m_removeButton = nullptr;
  QAction* m_addFileAction = nullptr;
  QAction* m_addDirectoryAction = nullptr;
  RoundedCornerOverlay* m_cornerOverlay = nullptr;
  bool m_storageAvailable = true;
  bool m_readOnly = false;
};

}  // namespace uwf::ui
