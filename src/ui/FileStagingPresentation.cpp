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
#include "FileStagingPresentation.h"

#include <algorithm>

#include "I18n.h"

namespace uwf::ui {

QString fileStagingFailureReason(const FileStagingCommitFailure& failure) {
  switch (failure.kind) {
    case FileStagingCommitFailureKind::InvalidPath:
      return I18n::tr("Only absolute paths on local volumes can be staged.");
    case FileStagingCommitFailureKind::ReparsePoint:
      return I18n::tr("Reparse points are not supported.");
    case FileStagingCommitFailureKind::PathInspectionFailed:
      return failure.detail.isEmpty() ? I18n::tr("The staged path could not be inspected.") : failure.detail;
    case FileStagingCommitFailureKind::FileTypeChanged:
      return I18n::tr("The staged file now refers to a different path type.");
    case FileStagingCommitFailureKind::DirectoryTypeChanged:
      return I18n::tr("The staged folder now refers to a different path type.");
    case FileStagingCommitFailureKind::DirectoryEnumerationFailed:
      return failure.detail.isEmpty() ? I18n::tr("The staged folder could not be enumerated completely.") : failure.detail;
    case FileStagingCommitFailureKind::MissingDriveLetter:
      return failure.detail.isEmpty() ? I18n::tr("The path has no drive letter.") : failure.detail;
    case FileStagingCommitFailureKind::VolumeRoot:
      return I18n::tr("A volume root cannot be processed recursively.");
    case FileStagingCommitFailureKind::ProviderFailure:
      return failure.detail.isEmpty() ? I18n::tr("The UWF provider rejected the commit.") : failure.detail;
    case FileStagingCommitFailureKind::UnknownFailure:
      return I18n::tr("The commit failed with an unknown error.");
  }
  return I18n::tr("The commit failed with an unknown error.");
}

QString renderFileStagingResult(const FileStagingCommitResult& result) {
  constexpr qsizetype kVisibleFailures = 8;
  QString text = I18n::tr("Discovered files: %1 · Committed: %2 · Skipped files: %3 · Skipped entries: %4 · Failed: %5")
                     .arg(static_cast<qulonglong>(result.discoveredFiles))
                     .arg(static_cast<qulonglong>(result.committedFiles))
                     .arg(static_cast<qulonglong>(result.skippedFiles))
                     .arg(static_cast<qulonglong>(result.skippedEntries))
                     .arg(static_cast<qlonglong>(result.failures.size()));
  const qsizetype visible = std::min(result.failures.size(), kVisibleFailures);
  for (qsizetype index = 0; index < visible; ++index) {
    const auto& failure = result.failures[index];
    const QString target = failure.path.isEmpty() ? I18n::tr("File staging preparation") : failure.path;
    text += QStringLiteral("\n\n• %1\n  %2").arg(target, fileStagingFailureReason(failure));
  }
  if (result.failures.size() > visible) {
    text +=
        QStringLiteral("\n\n") + I18n::tr("%1 additional failure(s) were written to the log.").arg(static_cast<qlonglong>(result.failures.size() - visible));
  }
  return text;
}

app::ApplicationCommandResult toApplicationCommandResult(const FileStagingCommitResult& result) {
  return {result.succeeded() ? app::ApplicationCommandOutcome::Succeeded : app::ApplicationCommandOutcome::CompletedWithFailures,
          result.discoveredFiles,
          result.committedFiles,
          result.skippedFiles,
          result.skippedEntries,
          static_cast<std::size_t>(result.failures.size()),
          result.succeeded() ? QString{} : renderFileStagingResult(result)};
}

}  // namespace uwf::ui
