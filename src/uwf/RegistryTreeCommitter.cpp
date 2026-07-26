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
#include "RegistryTreeCommitter.h"

#include <exception>
#include <stdexcept>

#include "../util/RegistryKey.h"

namespace uwf {

std::vector<RegistryCommitTarget> planRegistryTreeCommit(const std::string& key) {
  std::vector<RegistryCommitTarget> targets;
  for (const auto& current : regkey::collectKeyTree(regkey::normalize(key))) {
    for (const auto& valueName : regkey::valueNames(current)) targets.push_back({current, valueName});
  }
  return targets;
}

std::vector<RegistryCommitTarget> planRegistryTreeDeletion(const std::string& key) {
  std::vector<RegistryCommitTarget> targets;
  for (const auto& current : regkey::collectKeyTree(regkey::normalize(key))) targets.push_back({current, {}});
  return targets;
}

RegistryTreeCommitter::RegistryTreeCommitter(WmiOperations& session) : m_registry(session) {}

RegistryCommitResult RegistryTreeCommitter::commit(const std::vector<RegistryCommitTarget>& targets) const {
  RegistryCommitResult result;
  result.attempted = targets.size();
  if (targets.empty()) return result;

  const auto row = m_registry.read(api::Session::Current);
  for (const auto& target : targets) {
    try {
      m_registry.commitRegistry(row, target.key, target.valueName);
      ++result.committed;
    } catch (const std::exception& error) {
      result.failures.push_back({target, error.what()});
    } catch (...) {
      result.failures.push_back({target, "non-standard exception"});
    }
  }
  return result;
}

RegistryCommitResult RegistryTreeCommitter::commitDeletion(const std::vector<RegistryCommitTarget>& targets) const {
  RegistryCommitResult result;
  result.attempted = targets.size();
  std::vector<const RegistryCommitTarget*> existingTargets;
  existingTargets.reserve(targets.size());

  for (const auto& target : targets) {
    try {
      const bool exists = target.valueName.empty() ? regkey::keyExists(target.key) : regkey::valueExists(target.key, target.valueName);
      if (!exists) {
        ++result.skipped;
        continue;
      }
      existingTargets.push_back(&target);
    } catch (const std::exception& error) {
      result.failures.push_back({target, error.what()});
    } catch (...) {
      result.failures.push_back({target, "non-standard exception"});
    }
  }

  if (existingTargets.empty()) return result;

  const auto row = m_registry.read(api::Session::Current);
  for (const auto* target : existingTargets) {
    try {
      m_registry.commitRegistryDeletion(row, target->key, target->valueName);
      ++result.committed;
    } catch (const std::exception& error) {
      result.failures.push_back({*target, error.what()});
    } catch (...) {
      result.failures.push_back({*target, "non-standard exception"});
    }
  }
  return result;
}

}  // namespace uwf
