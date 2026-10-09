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
#include <QJsonDocument>
#include <QJsonObject>
#include <limits>
#include <stdexcept>

#include "uwf/api/PersistentOverlayCommands.h"

namespace {

using uwf::api::NativePersistentOverlayCommands;
using uwf::api::PersistentOverlayAction;
using uwf::api::PersistentOverlayCommandResult;
using uwf::api::PersistentOverlayCommands;

QJsonObject workerFrame(const PersistentOverlayCommandResult& result) {
  return {{QStringLiteral("protocol"), 1},
          {QStringLiteral("exitCode"), result.exitCode},
          {QStringLiteral("executionFailed"), result.executionFailed},
          {QStringLiteral("output"), result.output}};
}

PersistentOverlayCommandResult workerTransport(const QJsonObject& frame, const int processExitCode) {
  const QByteArray markedUtf8 = QByteArray::fromHex("efbbbf") + QJsonDocument(frame).toJson(QJsonDocument::Compact);
  return {processExitCode, NativePersistentOverlayCommands::decodeOutput(markedUtf8), false};
}

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
  void explicitUnicodeBomPreservesChineseAndSurrogatePairs();
  void unmarkedOutputStillUsesTheOemFallback();
  void truncatedMarkedUnicodeIsRejected();
  void workerProtocolPreservesSuccessFailureAndChineseOutput();
  void incompleteWorkerExecutionCannotAuthorizeSuccess();
  void malformedWorkerProtocolIsRejected();
  void workerProcessAndPayloadStatusMustAgree();
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
  // A transport/decoding failure cannot authorize the next destructive step,
  // even if the process status retained alongside it happens to be zero.
  QVERIFY((!PersistentOverlayCommandResult{0, QStringLiteral("Invalid output"), true}.succeeded()));
  QVERIFY((!PersistentOverlayCommandResult{-2147024891, QStringLiteral("Access denied"), true}.succeeded()));
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

void PersistentOverlayBehaviorTests::explicitUnicodeBomPreservesChineseAndSurrogatePairs() {
  const QString expected = QString::fromUtf8("启用\r\n🧪");
  const QByteArray utf16le = QByteArray::fromHex("fffe2f5428750d000a003ed8eadd");
  const QByteArray utf16be = QByteArray::fromHex("feff542f7528000d000ad83eddea");
  const QByteArray utf8 = QByteArray::fromHex("efbbbf") + expected.toUtf8();
  QCOMPARE(NativePersistentOverlayCommands::decodeOutput(utf16le), expected);
  QCOMPARE(NativePersistentOverlayCommands::decodeOutput(utf16be), expected);
  QCOMPARE(NativePersistentOverlayCommands::decodeOutput(utf8), expected);
  for (const QByteArray& bom : {QByteArray::fromHex("fffe"), QByteArray::fromHex("feff"), QByteArray::fromHex("efbbbf")}) {
    QVERIFY(NativePersistentOverlayCommands::decodeOutput(bom).isEmpty());
  }
}

void PersistentOverlayBehaviorTests::unmarkedOutputStillUsesTheOemFallback() {
  QVERIFY(NativePersistentOverlayCommands::decodeOutput({}).isEmpty());
  QCOMPARE(NativePersistentOverlayCommands::decodeOutput(QByteArray("Persistent overlay: OFF\r\n")), QStringLiteral("Persistent overlay: OFF\r\n"));
  // Unmarked NUL bytes do not trigger a UTF-16 heuristic, which could otherwise
  // reinterpret incomplete or mixed console evidence as a different encoding.
  QCOMPARE(NativePersistentOverlayCommands::decodeOutput(QByteArray("A\0B\0", 4)), QString::fromLatin1("A\0B\0", 4));
}

void PersistentOverlayBehaviorTests::truncatedMarkedUnicodeIsRejected() {
  for (const QByteArray& bytes : {QByteArray::fromHex("fffe2f"), QByteArray::fromHex("feff54"), QByteArray::fromHex("efbbbfe590")}) {
    QVERIFY_THROWS_EXCEPTION(std::runtime_error, static_cast<void>(NativePersistentOverlayCommands::decodeOutput(bytes)));
  }
}

void PersistentOverlayBehaviorTests::workerProtocolPreservesSuccessFailureAndChineseOutput() {
  const QString configuration = QString::fromUtf8("本次会话：持久覆盖层已启用\n下次启动：恢复🧪\n");
  const auto success = NativePersistentOverlayCommands::decodeWorkerResult(workerTransport(workerFrame({0, configuration, false}), 0));
  QVERIFY(success.succeeded());
  QCOMPARE(success.output, configuration);
  QCOMPARE(success.exitCode, 0);
  QVERIFY(!success.executionFailed);

  const QString denied = QString::fromUtf8("访问被拒绝。保留原始状态 0x80070005。\n");
  const auto failure = NativePersistentOverlayCommands::decodeWorkerResult(workerTransport(workerFrame({-2147024891, denied, true}), 1));
  QVERIFY(!failure.succeeded());
  QCOMPARE(failure.output, denied);
  QCOMPARE(failure.exitCode, -2147024891);
  QVERIFY(failure.executionFailed);

  // The protocol transports the full signed Windows status range, while an
  // execution failure remains a failure even if its retained status is zero.
  for (const int status : {std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), 0, 1}) {
    const auto result = NativePersistentOverlayCommands::decodeWorkerResult(workerTransport(workerFrame({status, denied, true}), 1));
    QVERIFY(!result.succeeded());
    QCOMPARE(result.exitCode, status);
    QCOMPARE(result.output, denied);
    QVERIFY(result.executionFailed);
  }
  const auto normalFailure = NativePersistentOverlayCommands::decodeWorkerResult(workerTransport(workerFrame({5, denied, false}), 1));
  QVERIFY(!normalFailure.succeeded());
  QCOMPARE(normalFailure.exitCode, 5);
  QVERIFY(!normalFailure.executionFailed);
}

void PersistentOverlayBehaviorTests::incompleteWorkerExecutionCannotAuthorizeSuccess() {
  const auto completeFrame = workerTransport(workerFrame({0, QStringLiteral("Reset scheduled"), false}), 0);
  // Zero can accompany a decoding/transport failure; a timeout retains the
  // sentinel; CrashExit can retain an access violation or an HRESULT. None
  // may consume the JSON frame, even if all success fields look complete.
  for (const int retainedStatus : {0, -1, -1073741819, -2147024891}) {
    const PersistentOverlayCommandResult failedTransport{retainedStatus, completeFrame.output, true};
    const auto result = NativePersistentOverlayCommands::decodeWorkerResult(failedTransport);
    QVERIFY(!result.succeeded());
    QVERIFY(result.executionFailed);
    QCOMPARE(result.exitCode, retainedStatus);
    QCOMPARE(result.output, failedTransport.output);
  }
}

void PersistentOverlayBehaviorTests::malformedWorkerProtocolIsRejected() {
  const auto validFrame = workerFrame({0, QStringLiteral("Success"), false});
  const auto changedField = [&validFrame](const QString& field, const QJsonValue& value) {
    auto frame = validFrame;
    frame.insert(field, value);
    return frame;
  };
  QList<QJsonObject> invalidFrames;
  auto missingOutput = validFrame;
  missingOutput.remove(QStringLiteral("output"));
  invalidFrames.append(missingOutput);
  auto extraField = validFrame;
  extraField.insert(QStringLiteral("unrecognized"), true);
  invalidFrames.append(extraField);
  auto wrongField = validFrame;
  wrongField.remove(QStringLiteral("output"));
  wrongField.insert(QStringLiteral("stdout"), QStringLiteral("Success"));
  invalidFrames.append(wrongField);
  for (const QJsonValue& protocol : {QJsonValue(0), QJsonValue(2), QJsonValue(QStringLiteral("1")), QJsonValue(true), QJsonValue()})
    invalidFrames.append(changedField(QStringLiteral("protocol"), protocol));
  for (const QJsonValue& status : {QJsonValue(0.5), QJsonValue(static_cast<double>(std::numeric_limits<int>::max()) + 1.0),
                                 QJsonValue(static_cast<double>(std::numeric_limits<int>::min()) - 1.0),
                                 QJsonValue(QStringLiteral("0")), QJsonValue(false), QJsonValue()})
    invalidFrames.append(changedField(QStringLiteral("exitCode"), status));
  for (const QJsonValue& flag : {QJsonValue(QStringLiteral("false")), QJsonValue(0), QJsonValue()})
    invalidFrames.append(changedField(QStringLiteral("executionFailed"), flag));
  for (const QJsonValue& output : {QJsonValue(0), QJsonValue(false), QJsonValue(), QJsonValue(QJsonObject{})})
    invalidFrames.append(changedField(QStringLiteral("output"), output));
  for (const auto& frame : invalidFrames) {
    const auto result = NativePersistentOverlayCommands::decodeWorkerResult(workerTransport(frame, 0));
    QVERIFY(!result.succeeded());
    QVERIFY(result.executionFailed);
    QCOMPARE(result.exitCode, -1);
    QVERIFY(result.output.contains(QStringLiteral("invalid report")));
  }

  const QString validJson = QString::fromUtf8(QJsonDocument(validFrame).toJson(QJsonDocument::Compact));
  const QStringList invalidJson = {QString{}, QStringLiteral("{}"), QStringLiteral("[]"), QStringLiteral("null"),
                                  validJson.left(validJson.size() - 1), validJson + QStringLiteral(" trailing data"),
                                  QStringLiteral("{\"protocol\":1,\"exitCode\":NaN,\"executionFailed\":false,\"output\":\"Success\"}"),
                                  QStringLiteral("{\"protocol\":1,\"exitCode\":1e400,\"executionFailed\":false,\"output\":\"Success\"}")};
  for (const auto& output : invalidJson) {
    const auto result = NativePersistentOverlayCommands::decodeWorkerResult({0, output, false});
    QVERIFY(!result.succeeded());
    QVERIFY(result.executionFailed);
    QCOMPARE(result.exitCode, -1);
    QVERIFY(result.output.contains(QStringLiteral("invalid report")));
  }
}

void PersistentOverlayBehaviorTests::workerProcessAndPayloadStatusMustAgree() {
  const auto successFrame = workerFrame({0, QStringLiteral("Reset scheduled"), false});
  const auto failureFrame = workerFrame({-2147024891, QStringLiteral("Access denied"), true});
  for (const int processStatus : {1, 2, -1}) {
    const auto result = NativePersistentOverlayCommands::decodeWorkerResult(workerTransport(successFrame, processStatus));
    QVERIFY(!result.succeeded());
    QVERIFY(result.executionFailed);
    QCOMPARE(result.exitCode, -1);
    QVERIFY(result.output.contains(QStringLiteral("inconsistent status")));
  }
  for (const int processStatus : {0, 2, -1}) {
    const auto result = NativePersistentOverlayCommands::decodeWorkerResult(workerTransport(failureFrame, processStatus));
    QVERIFY(!result.succeeded());
    QVERIFY(result.executionFailed);
    QCOMPARE(result.exitCode, -1);
    QVERIFY(result.output.contains(QStringLiteral("inconsistent status")));
  }
}

}  // namespace

QTEST_APPLESS_MAIN(PersistentOverlayBehaviorTests)

#include "PersistentOverlayBehaviorTests.moc"
