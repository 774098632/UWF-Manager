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
#include "FileStagingTask.h"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace uwf {

FileStagingTask::FileStagingTask(WmiOperations& session, app::FileStagingStore& store, const UwfCapability capability)
    : m_store(&store), m_capability(capability), m_filter(session), m_committer(session) {}

FileStagingTask::FileStagingTask(WmiOperations& session, QList<app::FileStagingEntry> entries, const UwfCapability capability)
    : m_entries(std::move(entries)), m_capability(capability), m_filter(session), m_committer(session) {}

FileStagingTask::~FileStagingTask() = default;

const std::array<FileStagingTask::Transition, static_cast<std::size_t>(FileStagingTask::State::Count) * static_cast<std::size_t>(FileStagingTask::Event::Count)>&
FileStagingTask::transitions() {
  static constexpr auto table = [] {
    std::array<Transition, static_cast<std::size_t>(State::Count) * static_cast<std::size_t>(Event::Count)> result{};
    const auto set = [&result](const State from, const Event event, const State to) {
      result[static_cast<std::size_t>(from) * static_cast<std::size_t>(Event::Count) + static_cast<std::size_t>(event)] = {to, true};
    };
    set(State::NotStarted, Event::ScanStarted, State::Scanning);
    set(State::NotStarted, Event::CompletedWithoutScan, State::Ready);
    set(State::Scanning, Event::ScanCompleted, State::Ready);
    return result;
  }();
  return table;
}

void FileStagingTask::postEvent(const Event event) {
  const std::size_t index = static_cast<std::size_t>(m_state) * static_cast<std::size_t>(Event::Count) + static_cast<std::size_t>(event);
  const auto& transition = transitions().at(index);
  if (!transition.valid) throw std::logic_error("invalid file staging task state transition");
  m_state = transition.next;
}

bool FileStagingTask::pollPreparation() {
  switch (m_state) {
    case State::Ready:
      return true;
    case State::NotStarted: {
      QList<app::FileStagingEntry> entries;
      if (m_entries) {
        entries = std::move(*m_entries);
        m_entries.reset();
      } else {
        if (!m_store) throw std::logic_error("file staging store is unavailable");
        entries = m_store->load();
      }
      m_sourceEntries = entries;
      if (entries.isEmpty()) {
        finishWithoutCommit();
        return true;
      }

      if (m_capability != UwfCapability::Available) {
        FileStagingCommitResult result;
        result.skippedEntries = static_cast<std::size_t>(entries.size());
        finishWithoutCommit(std::move(result));
        return true;
      }

      const auto filter = m_filter.read();
      if (!filter.currentEnabled) {
        FileStagingCommitResult result;
        result.skippedEntries = static_cast<std::size_t>(entries.size());
        finishWithoutCommit(std::move(result));
        return true;
      }

      auto scanPlan = m_committer.prepareScan(entries);
      std::promise<FileStagingScan> promise;
      m_scanFuture = promise.get_future();
      m_scanThread = std::jthread([this, plan = std::move(scanPlan), promise = std::move(promise)](const std::stop_token stopToken) mutable {
        try {
          promise.set_value(FileStagingCommitter::scan(std::move(plan), stopToken, &m_discoveredDuringScan));
        } catch (...) {
          promise.set_exception(std::current_exception());
        }
      });
      postEvent(Event::ScanStarted);
      return false;
    }
    case State::Scanning:
      break;
    case State::Count:
      throw std::logic_error("invalid file staging task state");
  }

  if (!m_scanFuture.valid()) throw std::logic_error("file staging scan future is unavailable");
  if (m_scanFuture.wait_for(std::chrono::seconds::zero()) != std::future_status::ready) return false;

  FileStagingScan scan;
  try {
    scan = m_scanFuture.get();
  } catch (...) {
    if (m_scanThread.joinable()) m_scanThread.join();
    throw;
  }
  if (m_scanThread.joinable()) m_scanThread.join();
  m_operation.emplace(m_committer.beginCommit(m_committer.prepare(std::move(scan))));
  postEvent(Event::ScanCompleted);
  return true;
}

bool FileStagingTask::preparationFinished() const { return m_state == State::Ready; }

bool FileStagingTask::finished() const {
  if (!preparationFinished()) return false;
  return m_terminalResult.has_value() || (m_operation && m_operation->finished());
}

std::size_t FileStagingTask::discoveredDuringScan() const { return m_discoveredDuringScan.load(std::memory_order_relaxed); }

std::size_t FileStagingTask::totalFiles() const {
  if (!preparationFinished()) throw std::logic_error("file staging preparation is incomplete");
  return m_operation ? m_operation->totalFiles() : 0;
}

const std::optional<QList<app::FileStagingEntry>>& FileStagingTask::sourceEntries() const { return m_sourceEntries; }

FileStagingTaskProgress FileStagingTask::advance() {
  if (!preparationFinished()) throw std::logic_error("file staging preparation is incomplete");
  if (finished()) throw std::logic_error("cannot advance a completed file staging task");
  if (!m_operation) throw std::logic_error("file staging commit operation is unavailable");

  const QString path = m_operation->advance();
  return {m_operation->processedFiles(), m_operation->totalFiles(), path};
}

const FileStagingCommitResult& FileStagingTask::result() const {
  if (!finished()) throw std::logic_error("file staging task result is not final");
  if (m_terminalResult) return *m_terminalResult;
  if (!m_operation) throw std::logic_error("file staging commit result is unavailable");
  return m_operation->result();
}

void FileStagingTask::finishWithoutCommit(FileStagingCommitResult result) {
  m_terminalResult.emplace(std::move(result));
  postEvent(Event::CompletedWithoutScan);
}

}  // namespace uwf
