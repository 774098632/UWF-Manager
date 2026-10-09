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

#include <cstdint>

#include "PersistentOverlayCommands.h"

namespace uwf::api {

// The native adapter owns DLL/version/device checks. This boundary accepts
// typed values only and never reads configuration from localized command text.
class PersistentOverlayLibraryOperations {
 public:
  virtual ~PersistentOverlayLibraryOperations() = default;
  [[nodiscard]] virtual std::int32_t getFlags(bool current, std::uint32_t& flags) = 0;
  [[nodiscard]] virtual std::int32_t getReset(std::uint32_t& mode) = 0;
  [[nodiscard]] virtual std::int32_t setFlags(std::uint32_t flags) = 0;
  [[nodiscard]] virtual std::int32_t setReset(std::uint32_t mode) = 0;
};

// Obtain these booleans from a fresh, successfully decoded WMI snapshot before
// a write. False defaults deliberately refuse actions with unmet prerequisites.
struct PersistentOverlayLibraryContext {
  bool currentEnabled = false;
  bool nextEnabled = false;
  bool currentDisk = false;
  bool nextDisk = false;
  bool currentProtected = false;
  bool nextProtected = false;
};

class PersistentOverlayLibrary final {
 public:
  explicit PersistentOverlayLibrary(PersistentOverlayLibraryOperations& operations) : m_operations(operations) {}

  [[nodiscard]] PersistentOverlayCommandResult execute(PersistentOverlayAction action, const PersistentOverlayLibraryContext& context);

 private:
  PersistentOverlayLibraryOperations& m_operations;
};

}  // namespace uwf::api
