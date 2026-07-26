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
#include "FileStagingCodec.h"

#include <QByteArray>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "../util/DriveLetter.h"
#include "../util/StringUtil.h"

namespace uwf::app {

namespace {

constexpr std::string_view kFilePrefix = "F|";
constexpr std::string_view kDirectoryPrefix = "D|";

std::string_view prefixFor(const FileStagingKind kind) {
  switch (kind) {
    case FileStagingKind::File:
      return kFilePrefix;
    case FileStagingKind::Directory:
      return kDirectoryPrefix;
  }
  throw std::invalid_argument("file staging entry has an unknown type");
}

}  // namespace

bool isAbsoluteLocalFileStagingPath(const QString& path) {
  if (path.isEmpty() || !std::filesystem::path(path.toStdWString()).is_absolute()) return false;
  if (path.startsWith(QStringLiteral(R"(\\.\)")) || path.startsWith(QStringLiteral(R"(\\?\)"))) return false;
  return !drive::split(path.toStdString()).letter.empty();
}

std::vector<std::string> encodeFileStagingEntries(const QList<FileStagingEntry>& entries) {
  std::vector<std::string> records;
  records.reserve(static_cast<std::size_t>(entries.size()));
  for (const auto& entry : entries) {
    if (entry.path.contains(QChar(u'\0'))) throw std::invalid_argument("file staging paths cannot contain NUL characters");
    if (!isAbsoluteLocalFileStagingPath(entry.path)) {
      throw std::invalid_argument("file staging entries require an absolute path on a local drive");
    }
    const QByteArray encodedPath = entry.path.toUtf8();
    std::string record(prefixFor(entry.kind));
    record.append(encodedPath.constData(), static_cast<std::size_t>(encodedPath.size()));
    records.push_back(std::move(record));
  }
  return records;
}

QList<FileStagingEntry> decodeFileStagingEntries(const std::vector<std::string>& records) {
  QList<FileStagingEntry> entries;
  entries.reserve(static_cast<qsizetype>(records.size()));
  for (const auto& record : records) {
    FileStagingKind kind;
    std::size_t prefixLength = 0;
    if (record.starts_with(kFilePrefix)) {
      kind = FileStagingKind::File;
      prefixLength = kFilePrefix.size();
    } else if (record.starts_with(kDirectoryPrefix)) {
      kind = FileStagingKind::Directory;
      prefixLength = kDirectoryPrefix.size();
    } else {
      throw std::runtime_error("file staging registry data contains an unknown entry type");
    }

    const QString path = QString::fromStdWString(utf8ToWide(record.substr(prefixLength)));
    if (path.contains(QChar(u'\0'))) throw std::runtime_error("file staging registry data contains a NUL character");
    if (!isAbsoluteLocalFileStagingPath(path)) {
      throw std::runtime_error("file staging registry data contains a path outside a local drive");
    }
    entries.append({kind, path});
  }
  return entries;
}

}  // namespace uwf::app
