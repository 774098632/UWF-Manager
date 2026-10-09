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

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace uwf::api {

enum class PersistentOverlayAction { GetConfig, Enable, Disable, Reset, CancelReset };

struct PersistentOverlayCommandResult {
  int exitCode = -1;
  QString output;
  bool executionFailed = false;

  [[nodiscard]] bool succeeded() const { return exitCode == 0 && !executionFailed; }
};

// Microsoft documents persistent-overlay controls through uwfmgr.exe. Its
// localized output is evidence for display only, never a configuration boolean.
class PersistentOverlayCommands {
 public:
  virtual ~PersistentOverlayCommands() = default;
  [[nodiscard]] virtual PersistentOverlayCommandResult execute(PersistentOverlayAction action) = 0;
};

class NativePersistentOverlayCommands final : public PersistentOverlayCommands {
 public:
  [[nodiscard]] PersistentOverlayCommandResult execute(PersistentOverlayAction action) override;
  [[nodiscard]] static QStringList arguments(PersistentOverlayAction action);
  // Explicit Unicode BOMs determine encoding; unmarked native-console output
  // uses the Windows OEM code page. This never interprets configuration text.
  [[nodiscard]] static QString decodeOutput(const QByteArray& bytes);
};

}  // namespace uwf::api
