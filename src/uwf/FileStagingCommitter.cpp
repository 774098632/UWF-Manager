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
#include "FileStagingCommitter.h"

#include <QDir>
#include <QSet>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "../util/DriveLetter.h"
#include "../util/PathMatch.h"
#include "../util/WindowsFileSystem.h"
#include "wmi/WmiError.h"
#include "wmi/WmiException.h"

namespace uwf {

namespace {

QString normalizedPath(const QString& path) { return QDir::toNativeSeparators(QDir::cleanPath(QDir::fromNativeSeparators(path))); }

void recordFailure(QList<FileStagingCommitFailure>& failures, const QString& path, const FileStagingCommitFailureKind kind, const QString& detail = {}) {
  failures.append({path, kind, detail});
}

bool isNotFound(const WmiException& error) {
  return error.code().category() == wmiErrorCategory() && WmiError(static_cast<int32_t>(error.code().value())) == WmiErrorCode::NotFound;
}

bool isMissing(const std::error_code& error) { return error == std::errc::no_such_file_or_directory; }

bool isDriveRoot(const QString& path) {
  const drive::PathSplit split = drive::split(path.toStdString());
  return !split.letter.empty() && stripTrailingSep(split.rest).empty();
}

bool isExcluded(const QString& path, const QStringList& exclusions) {
  QString normalized = normalizedPath(path);
  while (normalized.size() > 3 && normalized.endsWith('\\')) normalized.chop(1);
  return std::ranges::any_of(exclusions, [&](const QString& exclusion) {
    QString prefix = normalizedPath(exclusion);
    while (prefix.size() > 3 && prefix.endsWith('\\')) prefix.chop(1);
    if (QString::compare(normalized, prefix, Qt::CaseInsensitive) == 0) return true;
    return normalized.size() > prefix.size() && normalized.startsWith(prefix, Qt::CaseInsensitive) && normalized.at(prefix.size()) == u'\\';
  });
}

QString absoluteExclusionPath(const api::VolumeRow& volume, const api::ExcludedFile& exclusion) {
  const QString path = normalizedPath(QString::fromStdString(exclusion.fileName));
  if (path.startsWith('\\')) return QString::fromStdString(volume.driveLetter) + path;
  return path;
}

}  // namespace

FileStagingCommitter::FileStagingCommitter(WmiOperations& session) : m_volume(session) {}

FileStagingScanPlan FileStagingCommitter::prepareScan(const QList<app::FileStagingEntry>& entries) const {
  FileStagingScanPlan plan;
  QList<app::FileStagingEntry> candidates;
  for (const auto& entry : entries) {
    if (!app::isAbsoluteLocalFileStagingPath(entry.path)) {
      recordFailure(plan.m_failures, entry.path, FileStagingCommitFailureKind::InvalidPath);
      continue;
    }
    candidates.append(entry);
  }
  if (candidates.isEmpty()) return plan;

  try {
    plan.m_volumes = m_volume.readAll();
    for (const auto& volume : plan.m_volumes) {
      if (!volume.currentSession || !volume.isProtected) continue;
      for (const auto& exclusion : m_volume.getExclusions(volume)) {
        if (!exclusion.fileName.empty()) plan.m_fileExclusions.append(absoluteExclusionPath(volume, exclusion));
      }
    }
  } catch (const std::exception& error) {
    recordFailure(plan.m_failures, {}, FileStagingCommitFailureKind::ProviderFailure, QString::fromUtf8(error.what()));
    return plan;
  } catch (...) {
    recordFailure(plan.m_failures, {}, FileStagingCommitFailureKind::UnknownFailure);
    return plan;
  }

  for (const auto& entry : candidates) {
    std::string driveLetter;
    try {
      driveLetter = drive::fromPath(entry.path.toStdString());
    } catch (const std::exception& error) {
      recordFailure(plan.m_failures, entry.path, FileStagingCommitFailureKind::MissingDriveLetter, QString::fromUtf8(error.what()));
      continue;
    }
    if (driveLetter.empty()) {
      recordFailure(plan.m_failures, entry.path, FileStagingCommitFailureKind::MissingDriveLetter);
      continue;
    }

    const auto* volume = api::findBySession(plan.m_volumes, api::Session::Current, [&](const api::VolumeRow& row) { return row.driveLetter == driveLetter; });
    if (!volume || !volume->isProtected) {
      ++plan.m_skippedEntries;
      continue;
    }
    if (isExcluded(entry.path, plan.m_fileExclusions)) {
      ++plan.m_skippedEntries;
      continue;
    }
    plan.m_entries.append(entry);
  }
  return plan;
}

FileStagingScan FileStagingCommitter::scan(FileStagingScanPlan plan, const std::stop_token stopToken, std::atomic_size_t* const discoveredProgress) {
  FileStagingScan scan;
  scan.m_volumes = std::move(plan.m_volumes);
  scan.m_fileExclusions = std::move(plan.m_fileExclusions);
  scan.m_skippedEntries = plan.m_skippedEntries;
  scan.m_failures = std::move(plan.m_failures);
  QSet<QString> seenTargets;

  const auto addFile = [&](const QString& rawPath) {
    const QString path = normalizedPath(rawPath);
    if (isExcluded(path, scan.m_fileExclusions)) {
      ++scan.m_skippedFiles;
      return;
    }
    const QString identity = path.toCaseFolded();
    if (seenTargets.contains(identity)) return;
    seenTargets.insert(identity);
    scan.m_files.append(path);
    if (discoveredProgress) discoveredProgress->fetch_add(1, std::memory_order_relaxed);
  };

  for (const auto& entry : std::as_const(plan.m_entries)) {
    if (stopToken.stop_requested()) return scan;

    const QString path = normalizedPath(entry.path);
    const std::filesystem::path nativePath(path.toStdWString());
    winfs::EntryType entryType;
    try {
      entryType = winfs::inspect(nativePath);
    } catch (const std::exception& error) {
      recordFailure(scan.m_failures, path, FileStagingCommitFailureKind::PathInspectionFailed, QString::fromUtf8(error.what()));
      continue;
    }
    if (entryType == winfs::EntryType::Missing) {
      // 软件升级、缓存清理或卸载都可能让持久化列表中的路径自然消失。此时
      // 没有任何覆盖层内容需要提交，保持列表并允许电源操作继续。
      ++scan.m_skippedEntries;
      continue;
    }
    if (entryType == winfs::EntryType::ReparsePoint) {
      recordFailure(scan.m_failures, path, FileStagingCommitFailureKind::ReparsePoint);
      continue;
    }

    if (entry.kind == app::FileStagingKind::File) {
      if (entryType != winfs::EntryType::File) {
        recordFailure(scan.m_failures, path, FileStagingCommitFailureKind::FileTypeChanged);
        continue;
      }
      addFile(path);
      continue;
    }

    if (entryType != winfs::EntryType::Directory) {
      recordFailure(scan.m_failures, path, FileStagingCommitFailureKind::DirectoryTypeChanged);
      continue;
    }
    try {
      if (drive::fromPath(path.toStdString()).empty()) {
        recordFailure(scan.m_failures, path, FileStagingCommitFailureKind::MissingDriveLetter);
        continue;
      }
    } catch (const std::exception& error) {
      recordFailure(scan.m_failures, path, FileStagingCommitFailureKind::MissingDriveLetter, QString::fromUtf8(error.what()));
      continue;
    }
    if (isDriveRoot(path)) {
      recordFailure(scan.m_failures, path, FileStagingCommitFailureKind::VolumeRoot);
      continue;
    }

    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(nativePath, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) {
      if (isMissing(error)) {
        ++scan.m_skippedEntries;
        continue;
      }
      recordFailure(scan.m_failures, path, FileStagingCommitFailureKind::DirectoryEnumerationFailed, QString::fromStdString(error.message()));
      continue;
    }
    while (iterator != end) {
      if (stopToken.stop_requested()) return scan;

      const auto currentPath = iterator->path();
      winfs::EntryType childType;
      try {
        childType = winfs::inspect(currentPath);
      } catch (const std::exception& inspectionError) {
        recordFailure(scan.m_failures, QString::fromStdWString(currentPath.wstring()), FileStagingCommitFailureKind::DirectoryEnumerationFailed,
                      QString::fromUtf8(inspectionError.what()));
        iterator.disable_recursion_pending();
        iterator.increment(error);
        if (error) break;
        continue;
      }
      const QString childPath = QString::fromStdWString(currentPath.wstring());
      if (isExcluded(childPath, scan.m_fileExclusions)) {
        iterator.disable_recursion_pending();
        if (childType == winfs::EntryType::File) ++scan.m_skippedFiles;
        iterator.increment(error);
        if (error) {
          recordFailure(scan.m_failures, childPath, FileStagingCommitFailureKind::DirectoryEnumerationFailed, QString::fromStdString(error.message()));
          break;
        }
        continue;
      }
      if (childType == winfs::EntryType::Missing || childType == winfs::EntryType::ReparsePoint) {
        // Windows junction、符号链接和其他 reparse point 都是递归边界。无论
        // 目标类型如何都禁止 iterator 在下一步进入，避免暂存目录越界；并发
        // 消失的条目同样不可再进入，但仍继续核验迭代器能否完整遍历其余项。
        iterator.disable_recursion_pending();
      }
      if (childType == winfs::EntryType::File) {
        addFile(QString::fromStdWString(currentPath.wstring()));
      }

      iterator.increment(error);
      if (error) {
        recordFailure(scan.m_failures, QString::fromStdWString(currentPath.wstring()), FileStagingCommitFailureKind::DirectoryEnumerationFailed,
                      QString::fromStdString(error.message()));
        break;
      }
    }
  }

  return scan;
}

FileStagingCommitPlan FileStagingCommitter::prepare(FileStagingScan scan) const {
  FileStagingCommitPlan plan;
  plan.m_discoveredFiles = static_cast<std::size_t>(scan.m_files.size());
  plan.m_skippedFiles = scan.m_skippedFiles;
  plan.m_skippedEntries = scan.m_skippedEntries;
  plan.m_failures = std::move(scan.m_failures);
  plan.m_files = std::move(scan.m_files);
  plan.m_volumes = std::move(scan.m_volumes);
  return plan;
}

FileStagingCommitOperation::FileStagingCommitOperation(const api::UwfVolume& volume, FileStagingCommitPlan plan)
    : m_volume(volume), m_files(std::move(plan.m_files)), m_volumes(std::move(plan.m_volumes)) {
  m_result.discoveredFiles = plan.m_discoveredFiles;
  m_result.skippedFiles = plan.m_skippedFiles;
  m_result.skippedEntries = plan.m_skippedEntries;
  m_result.failures = std::move(plan.m_failures);
}

QString FileStagingCommitOperation::advance() {
  if (finished()) throw std::logic_error("cannot advance a completed file staging operation");

  const QString path = m_files.at(m_nextFile);
  ++m_nextFile;

  winfs::EntryType entryType;
  try {
    entryType = winfs::inspect(std::filesystem::path(path.toStdWString()));
  } catch (const std::exception& error) {
    recordFailure(m_result.failures, path, FileStagingCommitFailureKind::PathInspectionFailed, QString::fromUtf8(error.what()));
    return path;
  }
  if (entryType == winfs::EntryType::Missing) {
    ++m_result.skippedFiles;
    return path;
  }
  if (entryType == winfs::EntryType::ReparsePoint) {
    recordFailure(m_result.failures, path, FileStagingCommitFailureKind::ReparsePoint);
    return path;
  }
  if (entryType != winfs::EntryType::File) {
    recordFailure(m_result.failures, path, FileStagingCommitFailureKind::FileTypeChanged);
    return path;
  }

  std::string driveLetter;
  try {
    driveLetter = drive::fromPath(path.toStdString());
  } catch (const std::exception& error) {
    recordFailure(m_result.failures, path, FileStagingCommitFailureKind::MissingDriveLetter, QString::fromUtf8(error.what()));
    return path;
  }
  const auto* volume = api::findBySession(m_volumes, api::Session::Current, [&](const api::VolumeRow& row) { return row.driveLetter == driveLetter; });
  if (!volume || !volume->isProtected) {
    // 计划和执行之间卷状态理论上不会在同一 Windows 会话中变化；仍在边界处
    // 重新核验，避免对不再受保护的卷调用 provider。
    ++m_result.skippedFiles;
    return path;
  }

  try {
    m_volume.commitFile(*volume, path.toStdString());
    ++m_result.committedFiles;
  } catch (const WmiException& error) {
    // CommitFile 对“不在覆盖层中”的现存文件返回 NotFound。暂存列表会跨多次
    // 关机保留，因此这是正常幂等结果，不能让之后每次关机都被永久阻止。
    if (isNotFound(error)) {
      ++m_result.skippedFiles;
    } else {
      recordFailure(m_result.failures, path, FileStagingCommitFailureKind::ProviderFailure, QString::fromUtf8(error.what()));
    }
  } catch (const std::exception& error) {
    recordFailure(m_result.failures, path, FileStagingCommitFailureKind::ProviderFailure, QString::fromUtf8(error.what()));
  } catch (...) {
    recordFailure(m_result.failures, path, FileStagingCommitFailureKind::UnknownFailure);
  }

  return path;
}

FileStagingCommitOperation FileStagingCommitter::beginCommit(FileStagingCommitPlan plan) const { return FileStagingCommitOperation(m_volume, std::move(plan)); }

}  // namespace uwf
