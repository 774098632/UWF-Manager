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
#include "FileStagingHandoff.h"

#include <stdexcept>
#include <utility>

namespace uwf {

namespace {

// ShutdownSystem / RestartSystem 成功后 PRESHUTDOWN 应紧接着到达。保留一个
// 足以跨过系统调度抖动、但不会覆盖下一次独立关机意图的短窗口。
constexpr auto kHandoffLifetime = std::chrono::seconds{30};

}  // namespace

FileStagingHandoff& FileStagingHandoff::instance() {
  static FileStagingHandoff handoff;
  return handoff;
}

void FileStagingHandoff::publish(app::ApplicationCommandResult result, std::optional<QList<app::FileStagingEntry>> sourceEntries) {
  if (!result.authorizesPreshutdownRelease()) {
    throw std::logic_error("file staging handoff requires a result that authorizes preshutdown release");
  }
  if (result.outcome != app::ApplicationCommandOutcome::ContinuationApproved && !sourceEntries) {
    throw std::logic_error("completed file staging handoff requires its source snapshot");
  }
  m_entry = Entry{std::move(result), std::move(sourceEntries), std::chrono::steady_clock::now()};
}

std::optional<app::ApplicationCommandResult> FileStagingHandoff::consume(const app::FileStagingStore& store) {
  if (!m_entry) return std::nullopt;
  if (std::chrono::steady_clock::now() - m_entry->publishedAt > kHandoffLifetime) {
    m_entry.reset();
    return std::nullopt;
  }
  if (m_entry->sourceEntries) {
    const auto currentEntries = store.load();
    if (*m_entry->sourceEntries != currentEntries) {
      m_entry.reset();
      return std::nullopt;
    }
  }
  auto result = std::move(m_entry->result);
  m_entry.reset();
  return result;
}

void FileStagingHandoff::clear() { m_entry.reset(); }

}  // namespace uwf
