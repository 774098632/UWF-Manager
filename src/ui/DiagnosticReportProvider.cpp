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
#include "DiagnosticReportProvider.h"

#include <windows.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QLocale>
#include <QScopeGuard>
#include <QStringList>
#include <QSysInfo>
#include <algorithm>
#include <exception>
#include <limits>

#include "../service/EnhancedModeService.h"
#include "../util/Log.h"
#include "../util/WindowsVersion.h"
#include "../uwf/UwfSnapshot.h"
#include "ProcessTokenDiagnostics.h"
#include "uwf_version.h"

namespace uwf::ui {

namespace {

QString yesNo(const bool value) { return value ? QStringLiteral("yes") : QStringLiteral("no"); }

QString overlayTypeText(const core::OverlayType type) {
  switch (type) {
    case core::OverlayType::RAM:
      return QStringLiteral("RAM");
    case core::OverlayType::Disk:
      return QStringLiteral("disk");
    case core::OverlayType::Unknown:
      return QStringLiteral("unknown");
  }
  return QStringLiteral("unknown");
}

QString enhancedModeStateText(const service::EnhancedModeState state) {
  switch (state) {
    case service::EnhancedModeState::Disabled:
      return QStringLiteral("disabled");
    case service::EnhancedModeState::Enabled:
      return QStringLiteral("enabled");
    case service::EnhancedModeState::Stopped:
      return QStringLiteral("stopped");
    case service::EnhancedModeState::RepairRequired:
      return QStringLiteral("repair-required");
  }
  return QStringLiteral("unknown");
}

QString enhancedModeAgentStateText(const service::EnhancedModeAgentState state) {
  switch (state) {
    case service::EnhancedModeAgentState::Unobserved:
      return QStringLiteral("unobserved");
    case service::EnhancedModeAgentState::Connecting:
      return QStringLiteral("connecting");
    case service::EnhancedModeAgentState::Disconnected:
      return QStringLiteral("disconnected");
    case service::EnhancedModeAgentState::Connected:
      return QStringLiteral("connected");
  }
  return QStringLiteral("unknown");
}

DiagnosticSessionSummary captureSession(const core::SessionSnapshot& session) {
  return {
      .filterEnabled = session.filter.enabled,
      .overlayType = overlayTypeText(session.overlay.type),
      .maximumSizeMb = session.overlay.maximumSizeMb,
      .warningThresholdMb = session.overlay.warningThresholdMb,
      .criticalThresholdMb = session.overlay.criticalThresholdMb,
      .persistDomainSecretKey = session.persistDomainSecretKey,
      .persistTscal = session.persistTSCAL,
  };
}

void addSection(QStringList& lines, const QString& title) {
  if (!lines.isEmpty()) lines.append(QString{});
  lines.append(QStringLiteral("[%1]").arg(title));
}

void addValue(QStringList& lines, const QString& key, const QString& value) {
  lines.append(QStringLiteral("%1 = %2").arg(key, value.isEmpty() ? QStringLiteral("<empty>") : value));
}

QString windowsErrorText(const DWORD code) {
  wchar_t* raw = nullptr;
  const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                                     reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
  const auto releaseMessage = qScopeGuard([raw] {
    if (raw) LocalFree(raw);
  });
  return count != 0 && raw ? QString::fromWCharArray(raw, static_cast<qsizetype>(count)).trimmed() : QStringLiteral("Windows error %1").arg(code);
}

QString systemTimeZoneText() {
  DYNAMIC_TIME_ZONE_INFORMATION info{};
  const DWORD state = GetDynamicTimeZoneInformation(&info);
  if (state == TIME_ZONE_ID_INVALID) {
    const DWORD error = GetLastError();
    return windowsErrorText(error);
  }

  const LONG seasonalBias = state == TIME_ZONE_ID_DAYLIGHT ? info.DaylightBias : info.StandardBias;
  const LONG offsetMinutes = -(info.Bias + seasonalBias);
  const LONG absoluteMinutes = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
  const QString offset = QStringLiteral("UTC%1%2:%3")
                             .arg(offsetMinutes >= 0 ? QChar('+') : QChar('-'))
                             .arg(absoluteMinutes / 60, 2, 10, QChar('0'))
                             .arg(absoluteMinutes % 60, 2, 10, QChar('0'));
  QString name = QString::fromWCharArray(info.TimeZoneKeyName).trimmed();
  if (name.isEmpty()) name = QString::fromWCharArray(state == TIME_ZONE_ID_DAYLIGHT ? info.DaylightName : info.StandardName).trimmed();
  return name.isEmpty() ? offset : QStringLiteral("%1 (%2)").arg(name, offset);
}

void appendSession(QStringList& lines, const QString& name, const DiagnosticSessionSummary& session) {
  addValue(lines, name + QStringLiteral(".filter.enabled"), yesNo(session.filterEnabled));
  addValue(lines, name + QStringLiteral(".overlay.type"), session.overlayType);
  addValue(lines, name + QStringLiteral(".overlay.maximum_mb"), QString::number(session.maximumSizeMb));
  addValue(lines, name + QStringLiteral(".overlay.warning_mb"), QString::number(session.warningThresholdMb));
  addValue(lines, name + QStringLiteral(".overlay.critical_mb"), QString::number(session.criticalThresholdMb));
  addValue(lines, name + QStringLiteral(".registry.persist_domain_secret_key"), yesNo(session.persistDomainSecretKey));
  addValue(lines, name + QStringLiteral(".registry.persist_tscal"), yesNo(session.persistTscal));
}

void appendUwfInformation(QStringList& lines, const UwfDiagnosticSummary& summary) {
  addSection(lines, QStringLiteral("Unified Write Filter"));
  addValue(lines, QStringLiteral("capability"), summary.capability);
  addValue(lines, QStringLiteral("snapshot.available"), yesNo(summary.snapshotPresent));
  if (!summary.snapshotPresent) return;
  addValue(lines, QStringLiteral("snapshot.uwf_available"), yesNo(summary.uwfAvailable));
  addValue(lines, QStringLiteral("snapshot.elevated"), yesNo(summary.elevated));
  if (!summary.unavailableReason.isEmpty()) addValue(lines, QStringLiteral("snapshot.unavailable_reason"), summary.unavailableReason);
  if (!summary.uwfAvailable) return;
  addValue(lines, QStringLiteral("runtime.overlay_consumption_mb"), QString::number(summary.overlayConsumptionMb));
  addValue(lines, QStringLiteral("runtime.overlay_available_mb"), QString::number(summary.overlayAvailableMb));
  appendSession(lines, QStringLiteral("current"), summary.current);
  appendSession(lines, QStringLiteral("next"), summary.next);
}

void appendEnhancedModeInformation(QStringList& lines, const EnhancedModeDiagnosticSummary& summary) {
  addSection(lines, QStringLiteral("Enhanced mode"));
  if (!summary.present) {
    addValue(lines, QStringLiteral("status"), QStringLiteral("not available in this application instance"));
    return;
  }
  addValue(lines, QStringLiteral("state"), summary.state);
  addValue(lines, QStringLiteral("service.exists"), yesNo(summary.serviceExists));
  addValue(lines, QStringLiteral("service.registry_present"), yesNo(summary.serviceRegistryPresent));
  addValue(lines, QStringLiteral("service.own_process"), yesNo(summary.ownProcess));
  addValue(lines, QStringLiteral("service.automatic_start"), yesNo(summary.automaticStart));
  addValue(lines, QStringLiteral("service.local_system"), yesNo(summary.localSystemAccount));
  addValue(lines, QStringLiteral("service.running"), yesNo(summary.running));
  addValue(lines, QStringLiteral("service.executable_matches"), yesNo(summary.executableMatches));
  addValue(lines, QStringLiteral("service.preshutdown_timeout_configured"), yesNo(summary.preshutdownTimeoutConfigured));
  addValue(lines, QStringLiteral("service.required_privileges_configured"), yesNo(summary.requiredPrivilegesConfigured));
  addValue(lines, QStringLiteral("service.preshutdown_accepted"), yesNo(summary.preshutdownAccepted));
  addValue(lines, QStringLiteral("service.contract_satisfied"), yesNo(summary.serviceContractSatisfied));
  addValue(lines, QStringLiteral("agent.state"), summary.agentState);
  if (!summary.detail.isEmpty()) addValue(lines, QStringLiteral("detail"), summary.detail);
}

}  // namespace

DiagnosticReportContext DiagnosticReportProvider::capture(const UwfCapability capability, const core::UwfSnapshot* const snapshot,
                                                          const service::EnhancedModeStatus* const enhancedMode, const bool compatibilityMode) {
  DiagnosticReportContext context;
  context.compatibilityMode = compatibilityMode;
  context.uwf.capability = capability == UwfCapability::Available ? QStringLiteral("available") : QStringLiteral("unavailable");
  if (snapshot) {
    context.uwf.snapshotPresent = true;
    context.uwf.uwfAvailable = snapshot->uwfAvailable;
    context.uwf.elevated = snapshot->elevated;
    context.uwf.unavailableReason = QString::fromStdString(snapshot->unavailableReason);
    context.uwf.overlayConsumptionMb = snapshot->runtime.currentConsumptionMb;
    context.uwf.overlayAvailableMb = snapshot->runtime.availableSpaceMb;
    context.uwf.current = captureSession(snapshot->current);
    context.uwf.next = captureSession(snapshot->next);
  }
  if (enhancedMode) {
    context.enhancedMode = {
        .present = true,
        .state = enhancedModeStateText(enhancedMode->state),
        .serviceExists = enhancedMode->serviceExists,
        .serviceRegistryPresent = enhancedMode->serviceRegistryPresent,
        .ownProcess = enhancedMode->ownProcess,
        .automaticStart = enhancedMode->automaticStart,
        .localSystemAccount = enhancedMode->localSystemAccount,
        .running = enhancedMode->running,
        .executableMatches = enhancedMode->executableMatches,
        .preshutdownTimeoutConfigured = enhancedMode->preshutdownTimeoutConfigured,
        .requiredPrivilegesConfigured = enhancedMode->requiredPrivilegesConfigured,
        .preshutdownAccepted = enhancedMode->preshutdownAccepted,
        .serviceContractSatisfied = enhancedMode->serviceContractSatisfied(),
        .agentState = enhancedModeAgentStateText(enhancedMode->agentState),
        .detail = enhancedMode->detail,
    };
  }
  return context;
}

QString DiagnosticReportProvider::diagnosticText(const DiagnosticReportContext& context) {
  QStringList lines;
  lines.append(QStringLiteral("UWF Manager diagnostic report"));
  addValue(lines, QStringLiteral("generated.local"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
  addValue(lines, QStringLiteral("generated.utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));

  addSection(lines, QStringLiteral("Application"));
  addValue(lines, QStringLiteral("version"), QString::fromLatin1(UWF_VER_STRING));
  addValue(lines, QStringLiteral("qt.runtime"), QString::fromLatin1(qVersion()));
  addValue(lines, QStringLiteral("qt.build"), QString::fromLatin1(QT_VERSION_STR));
#if defined(NDEBUG)
  addValue(lines, QStringLiteral("build.configuration"), QStringLiteral("release"));
#else
  addValue(lines, QStringLiteral("build.configuration"), QStringLiteral("debug"));
#endif
  addValue(lines, QStringLiteral("executable"), QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
  addValue(lines, QStringLiteral("process.id"), QString::number(GetCurrentProcessId()));
  addValue(lines, QStringLiteral("process.architecture"), QSysInfo::buildCpuArchitecture());
  addValue(lines, QStringLiteral("compatibility_mode"), yesNo(context.compatibilityMode));

  addSection(lines, QStringLiteral("Windows"));
  try {
    const auto& version = windowsVersionInfo();
    addValue(lines, QStringLiteral("windows.product"), QString::fromStdString(version.productName));
    addValue(lines, QStringLiteral("windows.edition_id"), QString::fromStdString(version.editionId));
    addValue(lines, QStringLiteral("windows.display_version"), QString::fromStdString(version.displayVersion));
    addValue(lines, QStringLiteral("windows.version"),
             QStringLiteral("%1.%2.%3.%4").arg(version.major).arg(version.minor).arg(version.build).arg(version.revision));
    addValue(lines, QStringLiteral("windows.ltsc"), yesNo(version.longTermServicing));
  } catch (const std::exception& error) {
    addValue(lines, QStringLiteral("windows.error"), QString::fromUtf8(error.what()));
  }
  addValue(lines, QStringLiteral("system.architecture"), QSysInfo::currentCpuArchitecture());
  addValue(lines, QStringLiteral("system.locale"), QLocale::system().name());
  addValue(lines, QStringLiteral("system.ui_languages"), QLocale::system().uiLanguages().join(QStringLiteral(", ")));
  addValue(lines, QStringLiteral("system.time_zone"), systemTimeZoneText());
  const quint64 uptimeMs = GetTickCount64();
  addValue(lines, QStringLiteral("system.uptime_ms"), QString::number(uptimeMs));
  const quint64 boundedUptime = std::min(uptimeMs, static_cast<quint64>(std::numeric_limits<qint64>::max()));
  addValue(lines, QStringLiteral("system.boot_time.local"),
           QDateTime::currentDateTime().addMSecs(-static_cast<qint64>(boundedUptime)).toString(Qt::ISODateWithMs));

  addSection(lines, QStringLiteral("Security and process token"));
  for (const DiagnosticField& field : collectProcessTokenDiagnostics()) addValue(lines, field.key, field.value);

  appendUwfInformation(lines, context.uwf);
  appendEnhancedModeInformation(lines, context.enhancedMode);

  addSection(lines, QStringLiteral("Recent application log"));
  constexpr std::size_t kDiagnosticLogLines = 64;
  constexpr std::size_t kDiagnosticLogLineBytes = 1024;
  const auto logs = recentLogLines(kDiagnosticLogLines, kDiagnosticLogLineBytes);
  addValue(lines, QStringLiteral("lines.count"), QString::number(logs.size()));
  lines.append(QStringLiteral("--- begin log ---"));
  for (const auto& line : logs) lines.append(QString::fromStdString(line));
  lines.append(QStringLiteral("--- end log ---"));
  return lines.join(QChar('\n'));
}

}  // namespace uwf::ui
