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

#include <chrono>
#include <optional>

#include "../app/ApplicationCommand.h"
#include "../app/FileStagingStore.h"

namespace uwf {

// 安全关机/重启已完成暂存提交后，SCM 很快会送达 PRESHUTDOWN。这个进程内
// 一次性交接保存刚完成的最终结果和当时的暂存列表，让服务请求确认“同一轮
// 列表已经处理”而不重复 CommitFile。它不落盘、不跨启动；列表发生任何变化
// 或交接超时后都必须重新执行，不能把取消关机留下的结果用于下一轮。只有
// 暂存存储本身不可读、且用户明确选择继续时，交接才没有可复核输入快照。
class FileStagingHandoff final {
 public:
  static FileStagingHandoff& instance();

  void publish(app::ApplicationCommandResult result, std::optional<QList<app::FileStagingEntry>> sourceEntries);
  [[nodiscard]] std::optional<app::ApplicationCommandResult> consume(const app::FileStagingStore& store);
  void clear();

 private:
  struct Entry {
    app::ApplicationCommandResult result;
    std::optional<QList<app::FileStagingEntry>> sourceEntries;
    std::chrono::steady_clock::time_point publishedAt;
  };

  std::optional<Entry> m_entry;
};

}  // namespace uwf
