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
#include "WindowsFileSystem.h"

#include <windows.h>

#include <system_error>

namespace uwf::winfs {

EntryType inspect(const std::filesystem::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return EntryType::ReparsePoint;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? EntryType::Directory : EntryType::File;
  }

  const DWORD error = GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return EntryType::Missing;
  throw std::system_error(static_cast<int>(error), std::system_category(), "inspect Windows file-system path");
}

}  // namespace uwf::winfs
