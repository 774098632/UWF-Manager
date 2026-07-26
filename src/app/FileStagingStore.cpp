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
#include "FileStagingStore.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "../util/PathMatch.h"
#include "../util/RegistryKey.h"
#include "../uwf/RegistryTreeCommitter.h"
#include "../uwf/UwfSnapshot.h"
#include "../uwf/api/UwfFilter.h"
#include "../uwf/api/UwfRegistryFilter.h"
#include "../uwf/wmi/WmiError.h"
#include "../uwf/wmi/WmiException.h"

namespace uwf::app {

namespace {

[[nodiscard]] std::vector<std::string> exclusionPaths(const api::UwfRegistryFilter& registry, const api::RegistryFilterRow& row) {
  std::vector<std::string> exclusions;
  for (const auto& exclusion : registry.getExclusions(row)) exclusions.push_back(exclusion.registryKey);
  return exclusions;
}

[[nodiscard]] bool isCovered(const std::vector<std::string>& exclusions) {
  return !findCoveringExclusion(exclusions, std::string(kFileStagingRegistryRoot)).empty();
}

[[nodiscard]] bool ownsExactExclusion(const std::vector<std::string>& exclusions) {
  const std::string root = stripTrailingSep(std::string(kFileStagingRegistryRoot));
  return std::ranges::any_of(exclusions, [&](const std::string& exclusion) {
    const std::string candidate = stripTrailingSep(exclusion);
    return pathIsExcludedBy(root, candidate) && pathIsExcludedBy(candidate, root);
  });
}

[[nodiscard]] bool isNotFound(const WmiException& error) {
  return error.code().category() == wmiErrorCategory() && WmiError(static_cast<int32_t>(error.code().value())) == WmiErrorCode::NotFound;
}

}  // namespace

RegistryFileStagingStore::RegistryFileStagingStore(WmiOperations& session, const UwfCapability capability) : m_session(session), m_capability(capability) {}

QList<FileStagingEntry> RegistryFileStagingStore::load() const {
  const auto records = regkey::readMultiString(kFileStagingRegistryKey, kFileStagingEntriesValue);
  return records ? decodeFileStagingEntries(*records) : QList<FileStagingEntry>{};
}

void RegistryFileStagingStore::replace(const QList<FileStagingEntry>& entries) {
  if (entries.isEmpty()) {
    clear();
    return;
  }

  regkey::writeMultiString(kFileStagingRegistryKey, kFileStagingEntriesValue, encodeFileStagingEntries(entries));
  preserve();
}

void RegistryFileStagingStore::reconcileDurability() {
  if (load().isEmpty())
    clear();
  else
    preserve();
}

void RegistryFileStagingStore::preserve() {
  if (m_capability != UwfCapability::Available) return;

  api::UwfFilter filter(m_session);
  const auto filterState = filter.read();
  api::UwfRegistryFilter registry(m_session);
  const auto rows = registry.readAll();
  const auto* current = api::findBySession(rows, api::Session::Current);
  const auto* next = api::findBySession(rows, api::Session::Next);
  if (!current || !next) throw std::runtime_error("file staging durability requires current and next UWF registry filter records");

  const auto currentExclusions = filterState.currentEnabled ? exclusionPaths(registry, *current) : std::vector<std::string>{};
  const auto nextExclusions = exclusionPaths(registry, *next);
  // 当前会话已受保护且本键尚未由排除项覆盖时，刚写入的列表仍位于覆盖层；
  // 必须先提交现有值，再把根键加入下一会话排除，否则重启后会得到一条排除项
  // 却丢失它本应保护的暂存列表。
  if (filterState.currentEnabled && !isCovered(currentExclusions)) {
    try {
      registry.commitRegistry(*current, std::string(kFileStagingRegistryKey), std::string(kFileStagingEntriesValue));
    } catch (const WmiException& error) {
      // 启动对账时该值可能已经位于真实注册表、当前覆盖层没有对应改动。
      // 此时 provider 返回 NotFound，但刚才的 load() 已确认持久值存在；
      // 将其视为幂等完成，继续补齐下一会话排除关系。
      if (!isNotFound(error)) throw;
    }
  }
  if (!isCovered(nextExclusions)) {
    registry.addExclusion(*next, std::string(kFileStagingRegistryRoot));
  }
}

void RegistryFileStagingStore::clear() {
  if (m_capability != UwfCapability::Available) {
    regkey::deleteTree(kFileStagingRegistryRoot);
    return;
  }

  api::UwfFilter filter(m_session);
  const auto filterState = filter.read();
  api::UwfRegistryFilter registry(m_session);
  const auto rows = registry.readAll();
  const auto* current = api::findBySession(rows, api::Session::Current);
  const auto* next = api::findBySession(rows, api::Session::Next);
  if (!current || !next) throw std::runtime_error("file staging cleanup requires current and next UWF registry filter records");

  const auto currentExclusions = filterState.currentEnabled ? exclusionPaths(registry, *current) : std::vector<std::string>{};
  const auto nextExclusions = exclusionPaths(registry, *next);
  if (regkey::keyExists(kFileStagingRegistryRoot)) {
    if (!filterState.currentEnabled || isCovered(currentExclusions)) {
      // 筛选器未运行时直接删除即落盘；当前键已排除时写入同样绕过覆盖层，
      // CommitRegistryDeletion 反而没有可提交的覆盖层目标。
      regkey::deleteTree(kFileStagingRegistryRoot);
    } else {
      // CommitRegistryDeletion 不会递归，必须按后序计划逐键提交删除。只在
      // 整棵树都处理成功后移除下一会话排除项，否则重启会恢复出一份没有
      // 持久化保障的半清理状态。
      const auto result = RegistryTreeCommitter(m_session).commitDeletion(planRegistryTreeDeletion(std::string(kFileStagingRegistryRoot)));
      if (!result.succeeded()) {
        const auto& first = result.failures.front();
        throw std::runtime_error("file staging registry cleanup failed for " + first.target.key + ": " + first.detail);
      }
    }
  }

  // 只移除本功能拥有的精确排除项；若用户另有祖先排除，不应擅自删除。
  if (ownsExactExclusion(nextExclusions)) {
    registry.removeExclusion(*next, std::string(kFileStagingRegistryRoot));
  }
}

RegistryFileStagingStore& registryFileStagingStore(WmiOperations& session, const UwfCapability capability) {
  static RegistryFileStagingStore store(session, capability);
  return store;
}

}  // namespace uwf::app
