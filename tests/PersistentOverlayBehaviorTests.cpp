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
#include <QtTest>
#include <limits>
#include <stdexcept>

#include "uwf/api/PersistentOverlayCommands.h"

namespace {

using uwf::api::NativePersistentOverlayCommands;
using uwf::api::PersistentOverlayAction;
using uwf::api::PersistentOverlayCommandResult;
using uwf::api::PersistentOverlayCommands;

class ScriptedPersistentOverlayCommands final : public PersistentOverlayCommands {
 public:
  QList<PersistentOverlayAction> invocations;
  QList<PersistentOverlayCommandResult> outcomes;

  PersistentOverlayCommandResult execute(const PersistentOverlayAction action) override {
    invocations.append(action);
    if (outcomes.isEmpty()) throw std::logic_error("no scripted command outcome");
    return outcomes.takeFirst();
  }
};

class PersistentOverlayBehaviorTests final : public QObject {
  Q_OBJECT

 private slots:
  void actionsUseOnlyDocumentedOverlayCommands();
  void invalidActionCannotMapToAWrite();
  void successRequiresExactlyZeroExitCode();
  void commandBoundaryPreservesLocalizedEvidenceAndFailures();
};

void PersistentOverlayBehaviorTests::actionsUseOnlyDocumentedOverlayCommands() {
  // The reset flag applies at next boot, while set-persistent selects whether
  // ordinary boots keep the overlay. Substituting filter reset-settings here
  // would erase unrelated UWF configuration instead of resetting the overlay.
  QCOMPARE(NativePersistentOverlayCommands::arguments(PersistentOverlayAction::GetConfig),
           (QStringList{QStringLiteral("overlay"), QStringLiteral("get-config")}));
  QCOMPARE(NativePersistentOverlayCommands::arguments(PersistentOverlayAction::Enable),
           (QStringList{QStringLiteral("overlay"), QStringLiteral("set-persistent"), QStringLiteral("on")}));
  QCOMPARE(NativePersistentOverlayCommands::arguments(PersistentOverlayAction::Disable),
           (QStringList{QStringLiteral("overlay"), QStringLiteral("set-persistent"), QStringLiteral("off")}));
  QCOMPARE(NativePersistentOverlayCommands::arguments(PersistentOverlayAction::Reset),
           (QStringList{QStringLiteral("overlay"), QStringLiteral("reset-persistentstate"), QStringLiteral("on")}));
  QCOMPARE(NativePersistentOverlayCommands::arguments(PersistentOverlayAction::CancelReset),
           (QStringList{QStringLiteral("overlay"), QStringLiteral("reset-persistentstate"), QStringLiteral("off")}));
}

void PersistentOverlayBehaviorTests::invalidActionCannotMapToAWrite() {
  for (const int invalid : {-1, 5, 100}) {
    QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                            static_cast<void>(NativePersistentOverlayCommands::arguments(static_cast<PersistentOverlayAction>(invalid))));
  }
}

void PersistentOverlayBehaviorTests::successRequiresExactlyZeroExitCode() {
  QVERIFY(!PersistentOverlayCommandResult{}.succeeded());
  for (const int failure : {-1, 1, 5, 3010, std::numeric_limits<int>::max()}) {
    QVERIFY((!PersistentOverlayCommandResult{failure, QStringLiteral("Success")}.succeeded()));
  }
  QVERIFY((PersistentOverlayCommandResult{0, QStringLiteral("OFF")}.succeeded()));
  QVERIFY((PersistentOverlayCommandResult{0, QString{}}.succeeded()));
}

void PersistentOverlayBehaviorTests::commandBoundaryPreservesLocalizedEvidenceAndFailures() {
  ScriptedPersistentOverlayCommands scripted;
  const QString configuration = QString::fromUtf8("本次会话：持久覆盖层已启用\r\n下次会话：保留写入\r\n");
  const QString rejected = QString::fromUtf8("访问被拒绝。\r\n");
  const QString uncertain = QStringLiteral("uwfmgr.exe timed out; the command outcome is unknown");
  scripted.outcomes = {{0, configuration}, {5, rejected}, {-1, uncertain}, {0, QStringLiteral("Reset canceled")}};
  PersistentOverlayCommands& commands = scripted;

  const auto read = commands.execute(PersistentOverlayAction::GetConfig);
  QVERIFY(read.succeeded());
  QCOMPARE(read.output, configuration);
  const auto enable = commands.execute(PersistentOverlayAction::Enable);
  QVERIFY(!enable.succeeded());
  QCOMPARE(enable.exitCode, 5);
  QCOMPARE(enable.output, rejected);
  const auto reset = commands.execute(PersistentOverlayAction::Reset);
  QVERIFY(!reset.succeeded());
  QCOMPARE(reset.exitCode, -1);
  QCOMPARE(reset.output, uncertain);
  QVERIFY(commands.execute(PersistentOverlayAction::CancelReset).succeeded());
  QCOMPARE(scripted.invocations,
           (QList<PersistentOverlayAction>{PersistentOverlayAction::GetConfig, PersistentOverlayAction::Enable, PersistentOverlayAction::Reset,
                                          PersistentOverlayAction::CancelReset}));
  QVERIFY(scripted.outcomes.isEmpty());
}

}  // namespace

QTEST_APPLESS_MAIN(PersistentOverlayBehaviorTests)

#include "PersistentOverlayBehaviorTests.moc"
