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
#include <QProcess>
#include <QStringDecoder>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

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

PersistentOverlayCommandResult NativePersistentOverlayCommands::execute(const PersistentOverlayAction action) {
  PersistentOverlayCommandResult result;
  try {
    const QStringList commandArguments = arguments(action);
    QProcess process;
    process.setProgram(nativeUwfmgrPath());
    process.setArguments(commandArguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* parameters) { parameters->flags |= CREATE_NO_WINDOW; });
    process.start();
    if (!process.waitForStarted(kStartTimeoutMs)) {
      const QString error = process.errorString();
      stopProcess(process);
      result.output = decodeOutput(process.readAll());
      appendFailure(result.output, QStringLiteral("uwfmgr.exe could not be started: %1").arg(error));
      return result;
    }
    // No action (including a read) is reported successful until the process has
    // finished normally with exit code zero. A timed-out write is uncertain.
    if (!process.waitForFinished(kCommandTimeoutMs)) {
      const bool timedOut = process.error() == QProcess::Timedout;
      const QString error = process.errorString();
      stopProcess(process);
      result.output = decodeOutput(process.readAll());
      appendFailure(result.output,
                    timedOut ? QStringLiteral("uwfmgr.exe timed out after 30 seconds. The command outcome is unknown; inspect the configuration before retrying.")
                             : QStringLiteral("uwfmgr.exe did not finish normally: %1").arg(error));
      return result;
    }
    result.output = decodeOutput(process.readAll());
    if (process.exitStatus() != QProcess::NormalExit) {
      appendFailure(result.output, QStringLiteral("uwfmgr.exe terminated unexpectedly: %1").arg(process.errorString()));
      return result;
    }
    result.exitCode = process.exitCode();
  } catch (const std::exception& error) {
    result.exitCode = -1;
    appendFailure(result.output, QString::fromUtf8(error.what()));
  }
  return result;
}

}  // namespace uwf::api
