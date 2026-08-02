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

#include <QString>
#include <cstdint>

namespace uwf {

enum class UwfCapability;

namespace core {
struct UwfSnapshot;
}

namespace service {
struct EnhancedModeStatus;
}

namespace ui {

struct DiagnosticSessionSummary {
  bool filterEnabled = false;
  QString overlayType;
  std::uint32_t maximumSizeMb = 0;
  std::uint32_t warningThresholdMb = 0;
  std::uint32_t criticalThresholdMb = 0;
  bool persistDomainSecretKey = false;
  bool persistTscal = false;
};

struct UwfDiagnosticSummary {
  QString capability = QStringLiteral("unavailable");
  bool snapshotPresent = false;
  bool uwfAvailable = false;
  bool elevated = false;
  QString unavailableReason;
  std::uint32_t overlayConsumptionMb = 0;
  std::uint32_t overlayAvailableMb = 0;
  DiagnosticSessionSummary current;
  DiagnosticSessionSummary next;
};

struct EnhancedModeDiagnosticSummary {
  bool present = false;
  QString state;
  bool serviceExists = false;
  bool serviceRegistryPresent = false;
  bool ownProcess = false;
  bool automaticStart = false;
  bool localSystemAccount = false;
  bool running = false;
  bool executableMatches = false;
  bool preshutdownTimeoutConfigured = false;
  bool requiredPrivilegesConfigured = false;
  bool preshutdownAccepted = false;
  bool serviceContractSatisfied = false;
  QString agentState;
  QString detail;
};

// 诊断报告只持有实际展示的标量字段，不复制卷、排除项或 overlay 文件等完整
// UWF 快照。MainWindow 在用户点击“系统信息”时创建该 DTO。
struct DiagnosticReportContext {
  bool compatibilityMode = false;
  UwfDiagnosticSummary uwf;
  EnhancedModeDiagnosticSummary enhancedMode;
};

class DiagnosticReportProvider {
 public:
  [[nodiscard]] static DiagnosticReportContext capture(UwfCapability capability, const core::UwfSnapshot* snapshot,
                                                       const service::EnhancedModeStatus* enhancedMode, bool compatibilityMode);
  [[nodiscard]] static QString diagnosticText(const DiagnosticReportContext& context);
};

}  // namespace ui
}  // namespace uwf
