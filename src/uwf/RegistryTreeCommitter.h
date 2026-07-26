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

#include <string>
#include <vector>

#include "api/UwfRegistryFilter.h"

namespace uwf {

struct RegistryCommitTarget {
  std::string key;
  std::string valueName;
};

struct RegistryCommitFailure {
  RegistryCommitTarget target;
  std::string detail;
};

struct RegistryCommitResult {
  std::size_t attempted = 0;
  std::size_t committed = 0;
  std::size_t skipped = 0;
  std::vector<RegistryCommitFailure> failures;

  [[nodiscard]] bool succeeded() const { return failures.empty(); }
};

// 计划生成直接复用 regkey::collectKeyTree/valueNames 的生产递归能力；这里不
// 实现第二棵注册表遍历器。计划与执行分离，使服务删除前可以先冻结键清单，
// 再在 SCM 标记删除后、最终释放删除屏障前按同一清单提交删除。
[[nodiscard]] std::vector<RegistryCommitTarget> planRegistryTreeCommit(const std::string& key);
[[nodiscard]] std::vector<RegistryCommitTarget> planRegistryTreeDeletion(const std::string& key);

class RegistryTreeCommitter final {
 public:
  explicit RegistryTreeCommitter(WmiOperations& session);

  [[nodiscard]] RegistryCommitResult commit(const std::vector<RegistryCommitTarget>& targets) const;
  [[nodiscard]] RegistryCommitResult commitDeletion(const std::vector<RegistryCommitTarget>& targets) const;

 private:
  api::UwfRegistryFilter m_registry;
};

}  // namespace uwf
