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
#include "PersistentOverlayCommands.h"

#include <windows.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QStringDecoder>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <cmath>

namespace uwf::api {

namespace {

constexpr int kStartTimeoutMs = 10000;
constexpr int kCommandTimeoutMs = 30000;
constexpr int kKillTimeoutMs = 5000;

QString nativeUwfmgrPath() {
  // Resolve the trusted Windows directory through the OS, not PATH, the current
  // directory, or an environment variable. Sysnative bypasses WOW64 redirection
  // when this application itself is 32-bit on a 64-bit Windows installation.
  std::wstring windowsDir(MAX_PATH, L'\0');
  UINT length = GetSystemWindowsDirectoryW(windowsDir.data(), static_cast<UINT>(windowsDir.size()));
  if (length == 0) throw std::runtime_error("Windows system directory could not be resolved");
  if (length >= windowsDir.size()) {
    windowsDir.resize(static_cast<std::size_t>(length) + 1);
    length = GetSystemWindowsDirectoryW(windowsDir.data(), static_cast<UINT>(windowsDir.size()));
    if (length == 0 || length >= windowsDir.size()) throw std::runtime_error("Windows system directory could not be resolved");
  }
  windowsDir.resize(static_cast<std::size_t>(length));
  BOOL wow64 = FALSE;
  if (!IsWow64Process(GetCurrentProcess(), &wow64)) throw std::runtime_error("Windows process architecture could not be determined");
  if (!windowsDir.ends_with(L'\\')) windowsDir.push_back(L'\\');
  windowsDir += wow64 ? L"Sysnative\\uwfmgr.exe" : L"System32\\uwfmgr.exe";
  return QString::fromStdWString(windowsDir);
}

QString decodeOemOutput(const QByteArray& bytes) {
  if (bytes.isEmpty()) return {};
  if (bytes.size() > std::numeric_limits<int>::max()) throw std::length_error("uwfmgr.exe output is too large to decode");
  const int byteCount = static_cast<int>(bytes.size());
  const int wideCount = MultiByteToWideChar(CP_OEMCP, 0, bytes.constData(), byteCount, nullptr, 0);
  if (wideCount == 0) throw std::runtime_error("uwfmgr.exe output could not be decoded using the Windows OEM code page");
  std::wstring wide(static_cast<std::size_t>(wideCount), L'\0');
  if (MultiByteToWideChar(CP_OEMCP, 0, bytes.constData(), byteCount, wide.data(), wideCount) != wideCount) {
    throw std::runtime_error("uwfmgr.exe output could not be decoded using the Windows OEM code page");
  }
  return QString::fromStdWString(wide);
}

void appendFailure(QString& output, const QString& failure) {
  if (!output.isEmpty() && !output.endsWith(QChar('\n'))) output.append(QChar('\n'));
  output.append(failure);
}

QString windowsFailureStatus(const int exitCode, const QString& programName) {
  const DWORD status = static_cast<DWORD>(exitCode);
  QString detail = QStringLiteral("%1 returned Windows status %2 (0x%3)")
                       .arg(programName).arg(exitCode)
                       .arg(static_cast<qulonglong>(status), 8, 16, QLatin1Char('0'));
  // uwfmgr can return HRESULT_FROM_WIN32 errors, which Qt classifies as
  // CrashExit because the high bit is set. Preserve that status and decode
  // its Win32 portion; it does not by itself prove an application crash.
  if ((status & 0xffff0000UL) == 0x80070000UL) {
    std::wstring message(2048, L'\0');
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, status & 0xffffUL, 0,
                                       message.data(), static_cast<DWORD>(message.size()), nullptr);
    if (length) {
      message.resize(length);
      detail += QStringLiteral(": ") + QString::fromStdWString(message).trimmed();
    }
  }
  return detail;
}

void stopProcess(QProcess& process) {
  if (process.state() == QProcess::NotRunning) return;
  process.kill();
  (void)process.waitForFinished(kKillTimeoutMs);
}

}  // namespace

QString NativePersistentOverlayCommands::decodeOutput(const QByteArray& bytes) {
  // uwfmgr's documentation does not promise one redirected-output encoding.
  // Honor an explicit Unicode signature without guessing from localized text
  // or null-byte frequency. A complete output buffer is decoded statelessly
  // so a truncated Unicode sequence is detected instead of silently dropped.
  std::optional<QStringConverter::Encoding> encoding;
  if (bytes.startsWith("\xEF\xBB\xBF"))
    encoding = QStringConverter::Utf8;
  else if (bytes.startsWith("\xFF\xFE"))
    encoding = QStringConverter::Utf16LE;
  else if (bytes.startsWith("\xFE\xFF"))
    encoding = QStringConverter::Utf16BE;
  if (!encoding) return decodeOemOutput(bytes);

  // Qt's UTF-16 decoder replaces an odd trailing byte without setting
  // hasError(), including in Stateless mode. Reject that truncation explicitly.
  if ((*encoding == QStringConverter::Utf16LE || *encoding == QStringConverter::Utf16BE) && (bytes.size() & 1))
    throw std::runtime_error("uwfmgr.exe output contains a truncated UTF-16 sequence");

  QStringDecoder decoder(*encoding, QStringConverter::Flag::Stateless);
  const QString output = decoder(bytes);
  if (decoder.hasError()) throw std::runtime_error("uwfmgr.exe output contains an invalid or truncated Unicode sequence");
  return output;
}

QStringList NativePersistentOverlayCommands::arguments(const PersistentOverlayAction action) {
  switch (action) {
    case PersistentOverlayAction::GetConfig:
      return {QStringLiteral("overlay"), QStringLiteral("get-config")};
    case PersistentOverlayAction::Enable:
      return {QStringLiteral("overlay"), QStringLiteral("set-persistent"), QStringLiteral("on")};
    case PersistentOverlayAction::Disable:
      return {QStringLiteral("overlay"), QStringLiteral("set-persistent"), QStringLiteral("off")};
    case PersistentOverlayAction::Reset:
      return {QStringLiteral("overlay"), QStringLiteral("reset-persistentstate"), QStringLiteral("on")};
    case PersistentOverlayAction::CancelReset:
      return {QStringLiteral("overlay"), QStringLiteral("reset-persistentstate"), QStringLiteral("off")};
  }
  throw std::invalid_argument("persistent overlay action is invalid");
}

namespace {
PersistentOverlayCommandResult runCommandProcess(const QString& program, const QStringList& commandArguments) {
  PersistentOverlayCommandResult result;
  result.executionFailed = true;
  const QString programName = QFileInfo(program).fileName();
  try {
    QProcess process;
    process.setProgram(program);
    process.setArguments(commandArguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* parameters) { parameters->flags |= CREATE_NO_WINDOW; });
    process.start();
    if (!process.waitForStarted(kStartTimeoutMs)) {
      const QString error = process.errorString();
      stopProcess(process);
      result.output = NativePersistentOverlayCommands::decodeOutput(process.readAll());
      appendFailure(result.output, QStringLiteral("%1 could not be started: %2").arg(programName, error));
      return result;
    }
    // No action (including a read) is reported successful until the process has
    // finished normally with exit code zero. A timed-out write is uncertain.
    if (!process.waitForFinished(kCommandTimeoutMs)) {
      const bool timedOut = process.error() == QProcess::Timedout;
      const QString error = process.errorString();
      stopProcess(process);
      result.output = NativePersistentOverlayCommands::decodeOutput(process.readAll());
      appendFailure(result.output,
                    timedOut ? QStringLiteral("%1 timed out after 30 seconds. The command outcome is unknown; inspect the configuration before retrying.").arg(programName)
                             : QStringLiteral("%1 did not finish normally: %2").arg(programName, error));
      return result;
    }
    // The Windows Qt backend retains GetExitCodeProcess's DWORD even when
    // it labels a returned HRESULT as CrashExit. Capture it before decoding
    // output so a report-format failure cannot hide the native status.
    result.exitCode = process.exitCode();
    result.output = NativePersistentOverlayCommands::decodeOutput(process.readAll());
    if (process.exitStatus() != QProcess::NormalExit) {
      appendFailure(result.output, windowsFailureStatus(result.exitCode, programName));
      return result;
    }
    result.executionFailed = false;
  } catch (const std::exception& error) {
    appendFailure(result.output, QString::fromUtf8(error.what()));
  }
  return result;
}
QString workerAction(const PersistentOverlayAction action) {
  switch (action) {
    case PersistentOverlayAction::GetConfig: return QStringLiteral("get-config");
    case PersistentOverlayAction::Enable: return QStringLiteral("enable");
    case PersistentOverlayAction::Disable: return QStringLiteral("disable");
    case PersistentOverlayAction::Reset: return QStringLiteral("reset");
    case PersistentOverlayAction::CancelReset: return QStringLiteral("cancel-reset");
  }
  throw std::invalid_argument("persistent overlay action is invalid");
}
PersistentOverlayCommandResult runConfigurationWorker(const PersistentOverlayAction action) {
  const auto transport = runCommandProcess(QCoreApplication::applicationFilePath(),
      {QStringLiteral("--internal-overlay-configuration-worker"), workerAction(action)});
  return NativePersistentOverlayCommands::decodeWorkerResult(transport);
}
}  // namespace

PersistentOverlayCommandResult NativePersistentOverlayCommands::decodeWorkerResult(const PersistentOverlayCommandResult& processResult) {
  // Never accept a complete-looking frame from a crashed or timed-out worker:
  // a write outcome may be uncertain until the process finishes normally.
  if (processResult.executionFailed) return processResult;
  QJsonParseError error{};
  const auto document = QJsonDocument::fromJson(processResult.output.toUtf8(), &error);
  const auto object = document.object();
  const auto status = object.value(QStringLiteral("exitCode"));
  const double number = status.toDouble(std::numeric_limits<double>::quiet_NaN());
  const auto valid = error.error == QJsonParseError::NoError && document.isObject() && object.size() == 4 &&
                     object.value(QStringLiteral("protocol")).isDouble() && object.value(QStringLiteral("protocol")).toDouble() == 1 &&
                     status.isDouble() && std::isfinite(number) && std::floor(number) == number &&
                     number >= std::numeric_limits<int>::min() && number <= std::numeric_limits<int>::max() &&
                     object.value(QStringLiteral("executionFailed")).isBool() && object.value(QStringLiteral("output")).isString();
  if (!valid) return {-1, QStringLiteral("The UWF configuration worker returned an invalid report. Its outcome is unknown; inspect configuration before retrying."), true};
  PersistentOverlayCommandResult result{static_cast<int>(number), object.value(QStringLiteral("output")).toString(),
                                         object.value(QStringLiteral("executionFailed")).toBool()};
  if (processResult.exitCode != (result.succeeded() ? 0 : 1))
    return {-1, QStringLiteral("The UWF configuration worker returned an inconsistent status. Its outcome is unknown; inspect configuration before retrying."), true};
  return result;
}

PersistentOverlayCommandResult NativePersistentOverlayCommands::execute(const PersistentOverlayAction action) {
  // Validate the enum before a backend probe can launch a process.
  (void)arguments(action);
  if (action == PersistentOverlayAction::GetConfig && m_backendSelected && m_useConfigurationLibrary) return runConfigurationWorker(action);
  if (action != PersistentOverlayAction::GetConfig) {
    // A failed refresh never grants write permission. Re-read using the chosen
    // backend before each write, even when an earlier dialog read succeeded.
    const auto selection = execute(PersistentOverlayAction::GetConfig);
    if (!selection.succeeded()) return selection;
    if (m_useConfigurationLibrary) return runConfigurationWorker(action);
  }
  const auto result = runCommandProcess(nativeUwfmgrPath(), arguments(action));
  if (action == PersistentOverlayAction::GetConfig) {
    if (result.succeeded()) {
      m_backendSelected = true;
      m_useConfigurationLibrary = false;
    } else if (static_cast<DWORD>(result.exitCode) == 0x80070005UL) {
      const auto fallback = runConfigurationWorker(PersistentOverlayAction::GetConfig);
      if (fallback.succeeded()) {
        m_backendSelected = true;
        m_useConfigurationLibrary = true;
        return fallback;
      }
      return {fallback.exitCode, result.output + QStringLiteral("\n\nConfiguration-library fallback:\n") + fallback.output, true};
    }
  }
  // A write is attempted exactly once. A failed write is never replayed via a
  // different backend, even when its status happens to be Access denied.
  return result;
}

}  // namespace uwf::api
