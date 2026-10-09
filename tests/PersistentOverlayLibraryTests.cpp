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
#include <stdexcept>

#include "uwf/api/PersistentOverlayLibrary.h"

namespace {

using uwf::api::PersistentOverlayAction;
using uwf::api::PersistentOverlayLibrary;
using uwf::api::PersistentOverlayLibraryContext;
using uwf::api::PersistentOverlayLibraryOperations;

constexpr std::int32_t kDenied = -2147024891;
constexpr std::int32_t kInvalidData = -2147024883;

enum class Call { CurrentFlags, NextFlags, ReadReset, WriteFlags, WriteReset };

struct Step {
  Call call;
  std::uint32_t value;
  std::int32_t status = 0;
  bool throwOnCall = false;
};

// An unexpected call fails the result, while the tests also check that every
// expected step was consumed. All writes end here; there is no native adapter.
class ScriptedOperations final : public PersistentOverlayLibraryOperations {
 public:
  QList<Step> steps;
  int reads = 0;
  int writes = 0;

  std::int32_t getFlags(const bool current, std::uint32_t& value) override {
    ++reads;
    return read(current ? Call::CurrentFlags : Call::NextFlags, value);
  }
  std::int32_t getReset(std::uint32_t& value) override {
    ++reads;
    return read(Call::ReadReset, value);
  }
  std::int32_t setFlags(const std::uint32_t value) override { return write(Call::WriteFlags, value); }
  std::int32_t setReset(const std::uint32_t value) override { return write(Call::WriteReset, value); }

 private:
  Step take(const Call call) {
    if (steps.isEmpty()) throw std::logic_error("unexpected operation after the scripted boundary");
    const auto step = steps.takeFirst();
    if (step.call != call) throw std::logic_error("operation order did not match the script");
    if (step.throwOnCall) throw std::runtime_error("scripted transport interruption");
    return step;
  }
  std::int32_t read(const Call call, std::uint32_t& value) {
    const auto step = take(call);
    if (step.status == 0) value = step.value;
    return step.status;
  }
  std::int32_t write(const Call call, const std::uint32_t value) {
    ++writes;
    const auto step = take(call);
    if (step.value != value) throw std::logic_error("write would change an unrequested value");
    return step.status;
  }
};

PersistentOverlayLibraryContext persistentContext() { return {true, true, true, true, true, true}; }
PersistentOverlayLibraryContext persistenceChangeContext() { return {false, true, false, true, false, true}; }

QList<Step> prerequisites(const PersistentOverlayAction action) {
  if (action == PersistentOverlayAction::Enable) return {{Call::NextFlags, 0x80}};
  if (action == PersistentOverlayAction::Disable) return {{Call::NextFlags, 0x82}};
  if (action == PersistentOverlayAction::Reset) return {{Call::CurrentFlags, 2}, {Call::NextFlags, 2}, {Call::ReadReset, 0}};
  return {{Call::ReadReset, 1}};
}

std::uint32_t planned(const PersistentOverlayAction action) {
  if (action == PersistentOverlayAction::Enable) return 0x82;
  if (action == PersistentOverlayAction::Disable) return 0x80;
  return action == PersistentOverlayAction::Reset ? 1 : 0;
}

Call writeCall(const PersistentOverlayAction action) {
  return action == PersistentOverlayAction::Enable || action == PersistentOverlayAction::Disable ? Call::WriteFlags : Call::WriteReset;
}

Call confirmationCall(const PersistentOverlayAction action) {
  return writeCall(action) == Call::WriteFlags ? Call::NextFlags : Call::ReadReset;
}

PersistentOverlayLibraryContext contextFor(const PersistentOverlayAction action) {
  return writeCall(action) == Call::WriteFlags ? persistenceChangeContext() : persistentContext();
}

const QList<PersistentOverlayAction> kWrites = {PersistentOverlayAction::Enable, PersistentOverlayAction::Disable,
                                              PersistentOverlayAction::Reset, PersistentOverlayAction::CancelReset};

class PersistentOverlayLibraryTests final : public QObject {
  Q_OBJECT

 private slots:
  void configurationReportsBothTypedSessionsAndSavedMode();
  void failedConfigurationReadStopsAtThatBoundary();
  void invalidActionCannotReachOperations();
  void persistencePrerequisitesRejectBeforeReading();
  void restoreRequiresEverySessionPrerequisite();
  void persistenceWritesPreserveEveryOtherFlag();
  void missingPersistenceAndHormRefuseWrites();
  void failedPrerequisiteReadsPreventEveryWrite();
  void unknownResetModesNeverReachTheSetter();
  void savedModeIsNeverPassedToTheSetter();
  void cancellationSurvivesChangedPendingSessionSettings();
  void writeFailureStopsWithoutConfirmationOrRetry();
  void failedAndMismatchedReadbackNeverReportSuccess();
  void interruptedWriteNeverRetries();
  void sFalseCannotSubstituteUninitializedValuesForConfiguration();
};

void PersistentOverlayLibraryTests::configurationReportsBothTypedSessionsAndSavedMode() {
  ScriptedOperations operations;
  operations.steps = {{Call::CurrentFlags, 2}, {Call::NextFlags, 0x80000004}, {Call::ReadReset, 255}};
  const auto result = PersistentOverlayLibrary(operations).execute(PersistentOverlayAction::GetConfig, {});
  QVERIFY(result.succeeded());
  QVERIFY(result.output.contains(QStringLiteral("Current overlay flags: 0x00000002; persistent: ON; read-only media/HORM flag: OFF")));
  QVERIFY(result.output.contains(QStringLiteral("Next overlay flags: 0x80000004; persistent: OFF; read-only media/HORM flag: ON")));
  QVERIFY(result.output.contains(QStringLiteral("255 (saved mode)")));
  QVERIFY(operations.steps.isEmpty());
  QCOMPARE(operations.writes, 0);
}

void PersistentOverlayLibraryTests::failedConfigurationReadStopsAtThatBoundary() {
  const QList<Step> reads = {{Call::CurrentFlags, 2}, {Call::NextFlags, 2}, {Call::ReadReset, 0}};
  for (int failedRead = 0; failedRead < reads.size(); ++failedRead) {
    ScriptedOperations operations;
    operations.steps = reads;
    operations.steps[failedRead].status = kDenied;
    const auto result = PersistentOverlayLibrary(operations).execute(PersistentOverlayAction::GetConfig, {});
    QVERIFY(!result.succeeded());
    QCOMPARE(result.exitCode, kDenied);
    QVERIFY(result.output.contains(QStringLiteral("0x80070005")));
    QCOMPARE(operations.reads, failedRead + 1);
    QCOMPARE(operations.writes, 0);
    QCOMPARE(operations.steps.size(), reads.size() - failedRead - 1);
  }
  ScriptedOperations operations;
  operations.steps = {{Call::CurrentFlags, 2}, {Call::NextFlags, 2}, {Call::ReadReset, 256}};
  const auto result = PersistentOverlayLibrary(operations).execute(PersistentOverlayAction::GetConfig, {});
  QVERIFY(!result.succeeded());
  QCOMPARE(result.exitCode, kInvalidData);
  QVERIFY(result.output.contains(QStringLiteral("Unknown persistent overlay reset mode: 256")));
  QCOMPARE(operations.writes, 0);
}

void PersistentOverlayLibraryTests::invalidActionCannotReachOperations() {
  for (const int invalid : {-1, 5, 100}) {
    ScriptedOperations operations;
    QVERIFY(!PersistentOverlayLibrary(operations).execute(static_cast<PersistentOverlayAction>(invalid), persistentContext()).succeeded());
    QCOMPARE(operations.reads, 0);
    QCOMPARE(operations.writes, 0);
  }
}

void PersistentOverlayLibraryTests::persistencePrerequisitesRejectBeforeReading() {
  for (const auto action : {PersistentOverlayAction::Enable, PersistentOverlayAction::Disable}) {
    for (const auto context : {PersistentOverlayLibraryContext{}, persistentContext()}) {
      ScriptedOperations operations;
      QVERIFY(!PersistentOverlayLibrary(operations).execute(action, context).succeeded());
      QCOMPARE(operations.reads, 0);
      QCOMPARE(operations.writes, 0);
    }
  }
}

void PersistentOverlayLibraryTests::restoreRequiresEverySessionPrerequisite() {
  const QList<bool PersistentOverlayLibraryContext::*> requirements = {
      &PersistentOverlayLibraryContext::currentEnabled, &PersistentOverlayLibraryContext::nextEnabled,
      &PersistentOverlayLibraryContext::currentDisk, &PersistentOverlayLibraryContext::nextDisk,
      &PersistentOverlayLibraryContext::currentProtected, &PersistentOverlayLibraryContext::nextProtected};
  for (const auto requirement : requirements) {
    auto context = persistentContext();
    context.*requirement = false;
    ScriptedOperations operations;
    QVERIFY(!PersistentOverlayLibrary(operations).execute(PersistentOverlayAction::Reset, context).succeeded());
    QCOMPARE(operations.reads, 0);
    QCOMPARE(operations.writes, 0);
  }
}

void PersistentOverlayLibraryTests::persistenceWritesPreserveEveryOtherFlag() {
  for (const auto action : {PersistentOverlayAction::Enable, PersistentOverlayAction::Disable}) {
    for (const std::uint32_t unrelated : {0u, 0x81u, 0x80000009u}) {
      const auto original = action == PersistentOverlayAction::Enable ? unrelated : unrelated | 2u;
      const auto expected = action == PersistentOverlayAction::Enable ? unrelated | 2u : unrelated;
      ScriptedOperations operations;
      operations.steps = {{Call::NextFlags, original}, {Call::WriteFlags, expected}, {Call::NextFlags, expected}};
      QVERIFY(PersistentOverlayLibrary(operations).execute(action, persistenceChangeContext()).succeeded());
      QVERIFY(operations.steps.isEmpty());
      QCOMPARE(operations.writes, 1);
    }
  }
}

void PersistentOverlayLibraryTests::missingPersistenceAndHormRefuseWrites() {
  for (const auto flags : {QPair<std::uint32_t, std::uint32_t>{0, 2}, {2, 0}, {6, 2}, {2, 6}}) {
    ScriptedOperations operations;
    operations.steps = {{Call::CurrentFlags, flags.first}, {Call::NextFlags, flags.second}};
    QVERIFY(!PersistentOverlayLibrary(operations).execute(PersistentOverlayAction::Reset, persistentContext()).succeeded());
    QVERIFY(operations.steps.isEmpty());
    QCOMPARE(operations.writes, 0);
  }
  for (const auto action : {PersistentOverlayAction::Enable, PersistentOverlayAction::Disable}) {
    ScriptedOperations operations;
    operations.steps = {{Call::NextFlags, 6}};
    QVERIFY(!PersistentOverlayLibrary(operations).execute(action, persistenceChangeContext()).succeeded());
    QCOMPARE(operations.writes, 0);
  }
}

void PersistentOverlayLibraryTests::failedPrerequisiteReadsPreventEveryWrite() {
  for (const auto action : kWrites) {
    const auto reads = prerequisites(action);
    for (int failedRead = 0; failedRead < reads.size(); ++failedRead) {
      ScriptedOperations operations;
      operations.steps = reads;
      operations.steps[failedRead].status = kDenied;
      const auto result = PersistentOverlayLibrary(operations).execute(action, contextFor(action));
      QVERIFY(!result.succeeded());
      QCOMPARE(result.exitCode, kDenied);
      QCOMPARE(operations.reads, failedRead + 1);
      QCOMPARE(operations.writes, 0);
    }
  }
}

void PersistentOverlayLibraryTests::unknownResetModesNeverReachTheSetter() {
  for (const auto action : {PersistentOverlayAction::Reset, PersistentOverlayAction::CancelReset}) {
    for (const std::uint32_t mode : {2u, 254u, 256u, 0xffffffffu}) {
      ScriptedOperations operations;
      operations.steps = prerequisites(action);
      operations.steps.last().value = mode;
      const auto result = PersistentOverlayLibrary(operations).execute(action, contextFor(action));
      QVERIFY(!result.succeeded());
      QCOMPARE(result.exitCode, kInvalidData);
      QVERIFY(operations.steps.isEmpty());
      QCOMPARE(operations.writes, 0);
    }
  }
}

void PersistentOverlayLibraryTests::savedModeIsNeverPassedToTheSetter() {
  for (const auto action : {PersistentOverlayAction::Reset, PersistentOverlayAction::CancelReset}) {
    for (const std::uint32_t mode : {0u, 1u, 255u}) {
      ScriptedOperations operations;
      operations.steps = prerequisites(action);
      operations.steps.last().value = mode;
      operations.steps.append({Call::WriteReset, planned(action)});
      operations.steps.append({Call::ReadReset, planned(action)});
      QVERIFY(PersistentOverlayLibrary(operations).execute(action, contextFor(action)).succeeded());
      QVERIFY(operations.steps.isEmpty());
      QCOMPARE(operations.writes, 1);
    }
  }
}

void PersistentOverlayLibraryTests::cancellationSurvivesChangedPendingSessionSettings() {
  ScriptedOperations operations;
  operations.steps = {{Call::ReadReset, 1}, {Call::WriteReset, 0}, {Call::ReadReset, 0}};
  QVERIFY(PersistentOverlayLibrary(operations).execute(PersistentOverlayAction::CancelReset, {}).succeeded());
  QVERIFY(operations.steps.isEmpty());
  QCOMPARE(operations.writes, 1);
}

void PersistentOverlayLibraryTests::writeFailureStopsWithoutConfirmationOrRetry() {
  for (const auto action : kWrites) {
    ScriptedOperations operations;
    operations.steps = prerequisites(action);
    operations.steps.append({writeCall(action), planned(action), kDenied});
    operations.steps.append({confirmationCall(action), planned(action)});
    const auto result = PersistentOverlayLibrary(operations).execute(action, contextFor(action));
    QVERIFY(!result.succeeded());
    QCOMPARE(result.exitCode, kDenied);
    QVERIFY(result.output.contains(QStringLiteral("final outcome is not confirmed")));
    QCOMPARE(operations.writes, 1);
    QCOMPARE(operations.steps.size(), 1);
  }
}

void PersistentOverlayLibraryTests::failedAndMismatchedReadbackNeverReportSuccess() {
  for (const auto action : kWrites) {
    for (const bool failedRead : {false, true}) {
      ScriptedOperations operations;
      operations.steps = prerequisites(action);
      operations.steps.append({writeCall(action), planned(action)});
      operations.steps.append({confirmationCall(action), planned(action) ^ 0x80u, failedRead ? kDenied : 0});
      const auto result = PersistentOverlayLibrary(operations).execute(action, contextFor(action));
      QVERIFY(!result.succeeded());
      QCOMPARE(result.exitCode, failedRead ? kDenied : kInvalidData);
      QVERIFY(result.output.contains(QStringLiteral("final outcome is not confirmed")));
      QVERIFY(operations.steps.isEmpty());
      QCOMPARE(operations.writes, 1);
    }
  }
}

void PersistentOverlayLibraryTests::interruptedWriteNeverRetries() {
  ScriptedOperations operations;
  operations.steps = {{Call::ReadReset, 1}, {Call::WriteReset, 0, 0, true}, {Call::ReadReset, 0}};
  const auto result = PersistentOverlayLibrary(operations).execute(PersistentOverlayAction::CancelReset, {});
  QVERIFY(!result.succeeded());
  QVERIFY(result.output.contains(QStringLiteral("scripted transport interruption")));
  QVERIFY(result.output.contains(QStringLiteral("final outcome is not confirmed")));
  QCOMPARE(operations.writes, 1);
  QCOMPARE(operations.steps.size(), 1);
}

void PersistentOverlayLibraryTests::sFalseCannotSubstituteUninitializedValuesForConfiguration() {
  // The audited adapter only fills out parameters on S_OK. S_FALSE must not
  // turn an untouched zero into real flags or a verified canceled reset.
  const QList<Step> configurationReads = {{Call::CurrentFlags, 2}, {Call::NextFlags, 2}, {Call::ReadReset, 0}};
  for (int failedRead = 0; failedRead < configurationReads.size(); ++failedRead) {
    ScriptedOperations operations;
    operations.steps = configurationReads;
    operations.steps[failedRead].status = 1;
    const auto result = PersistentOverlayLibrary(operations).execute(PersistentOverlayAction::GetConfig, {});
    QVERIFY(!result.succeeded());
    QCOMPARE(result.exitCode, 1);
    QCOMPARE(operations.reads, failedRead + 1);
    QCOMPARE(operations.writes, 0);
  }
  for (const auto action : kWrites) {
    const auto beforeWrite = prerequisites(action);
    for (int failedRead = 0; failedRead < beforeWrite.size(); ++failedRead) {
      ScriptedOperations operations;
      operations.steps = beforeWrite;
      operations.steps[failedRead].status = 1;
      const auto result = PersistentOverlayLibrary(operations).execute(action, contextFor(action));
      QVERIFY(!result.succeeded());
      QCOMPARE(result.exitCode, 1);
      QCOMPARE(operations.reads, failedRead + 1);
      QCOMPARE(operations.writes, 0);
    }
    ScriptedOperations failedWrite;
    failedWrite.steps = beforeWrite;
    failedWrite.steps.append({writeCall(action), planned(action), 1});
    failedWrite.steps.append({confirmationCall(action), planned(action)});
    const auto writeResult = PersistentOverlayLibrary(failedWrite).execute(action, contextFor(action));
    QVERIFY(!writeResult.succeeded());
    QCOMPARE(writeResult.exitCode, 1);
    QCOMPARE(failedWrite.writes, 1);
    QCOMPARE(failedWrite.steps.size(), 1);

    ScriptedOperations failedConfirmation;
    failedConfirmation.steps = beforeWrite;
    failedConfirmation.steps.append({writeCall(action), planned(action)});
    failedConfirmation.steps.append({confirmationCall(action), planned(action), 1});
    const auto confirmationResult = PersistentOverlayLibrary(failedConfirmation).execute(action, contextFor(action));
    QVERIFY(!confirmationResult.succeeded());
    QCOMPARE(confirmationResult.exitCode, 1);
    QCOMPARE(failedConfirmation.writes, 1);
    QVERIFY(failedConfirmation.steps.isEmpty());
  }
}

}  // namespace

QTEST_APPLESS_MAIN(PersistentOverlayLibraryTests)

#include "PersistentOverlayLibraryTests.moc"
