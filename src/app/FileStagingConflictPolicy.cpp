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
#include "FileStagingConflictPolicy.h"

#include <QDir>
#include <algorithm>
#include <utility>

namespace uwf::app {

namespace {

QString normalizedPath(const QString& path) { return QDir::toNativeSeparators(QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()))); }

void normalizeUnique(QStringList& paths) {
  for (auto& path : paths) path = normalizedPath(path);
  paths.removeIf([](const QString& path) { return path.isEmpty(); });
  std::ranges::sort(paths, [](const QString& left, const QString& right) { return QString::compare(left, right, Qt::CaseInsensitive) < 0; });
  for (qsizetype index = paths.size() - 1; index > 0; --index) {
    if (QString::compare(paths.at(index), paths.at(index - 1), Qt::CaseInsensitive) == 0) paths.removeAt(index);
  }
}

bool pathsOverlap(const QString& left, const QString& right) {
  const auto contains = [](QString path, QString parent) {
    while (path.size() > 3 && path.endsWith('\\')) path.chop(1);
    while (parent.size() > 3 && parent.endsWith('\\')) parent.chop(1);
    if (QString::compare(path, parent, Qt::CaseInsensitive) == 0) return true;
    return path.size() > parent.size() && path.startsWith(parent, Qt::CaseInsensitive) && path.at(parent.size()) == u'\\';
  };
  const QString normalizedLeft = normalizedPath(left);
  const QString normalizedRight = normalizedPath(right);
  return contains(normalizedLeft, normalizedRight) || contains(normalizedRight, normalizedLeft);
}

std::optional<QString> firstConflict(const QString& candidate, const QStringList& existing) {
  const auto match = std::ranges::find_if(existing, [&](const QString& path) { return pathsOverlap(candidate, path); });
  return match == existing.end() ? std::nullopt : std::optional{*match};
}

}  // namespace

void FileStagingConflictPolicy::setFileExclusions(QStringList exclusions) {
  normalizeUnique(exclusions);
  m_fileExclusions = std::move(exclusions);
}

void FileStagingConflictPolicy::setStagedEntries(const QList<FileStagingEntry>& entries) {
  QStringList paths;
  paths.reserve(entries.size());
  for (const auto& entry : entries) paths.append(entry.path);
  normalizeUnique(paths);
  m_stagedPaths = std::move(paths);
}

std::optional<QString> FileStagingConflictPolicy::conflictingExclusion(const QString& stagedPath) const { return firstConflict(stagedPath, m_fileExclusions); }

std::optional<QString> FileStagingConflictPolicy::conflictingStagedPath(const QString& exclusionPath) const {
  return firstConflict(exclusionPath, m_stagedPaths);
}

}  // namespace uwf::app
