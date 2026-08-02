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

#include <string_view>

#include "FileStagingCodec.h"

namespace uwf {
class WmiOperations;
enum class UwfCapability;
}  // namespace uwf

namespace uwf::app {

inline constexpr std::string_view kFileStagingRegistryRoot = R"(HKEY_LOCAL_MACHINE\SOFTWARE\HsingYun\UWF Manager)";
inline constexpr std::string_view kFileStagingRegistryKey = R"(HKEY_LOCAL_MACHINE\SOFTWARE\HsingYun\UWF Manager\FileStaging)";
inline constexpr std::string_view kFileStagingEntriesValue = "Entries";

// 文件暂存只依赖一份完整快照的读取与原子替换。UI 在内存副本上完成增删后一次
// replace，电源流程只读；两者都不需要知道生产数据位于注册表。
class FileStagingStore {
 public:
  virtual ~FileStagingStore() = default;
  [[nodiscard]] virtual QList<FileStagingEntry> load() const = 0;
  virtual void replace(const QList<FileStagingEntry>& entries) = 0;
};

class RegistryFileStagingStore final : public FileStagingStore {
 public:
  RegistryFileStagingStore(WmiOperations& session, UwfCapability capability);

  [[nodiscard]] QList<FileStagingEntry> load() const override;
  void replace(const QList<FileStagingEntry>& entries) override;
  // 仅在生产 UI 启动阶段执行一次，修复上次异常中断可能留下的注册表排除状态。
  // 普通 UI 刷新是只读事务，不得隐式触发这项系统写操作。
  void reconcileDurability();

 private:
  void preserve();
  void clear();

  WmiOperations& m_session;
  UwfCapability m_capability;
};

// 进程内唯一生产存储。注册表位置：
// HKLM\SOFTWARE\HsingYun\UWF Manager\FileStaging，值名 Entries。生产进程要求
// 管理员权限，避免低完整性进程篡改随后会被提升权限提交的路径。
RegistryFileStagingStore& registryFileStagingStore(WmiOperations& session, UwfCapability capability);

}  // namespace uwf::app
