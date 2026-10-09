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
#include "PersistentOverlayNative.h"

#include <windows.h>

#include <QCryptographicHash>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

#include "PersistentOverlayLibrary.h"
#include "UwfFilter.h"
#include "UwfOverlayConfig.h"
#include "UwfVolume.h"
#include "../SystemCheck.h"
#include "../UwfSnapshot.h"
#include "../wmi/WmiClient.h"

namespace uwf::api {
namespace {

// This is the exact Microsoft binary present on the reported target and
// audited against its matching public PDB and implementation. A Windows
// update that changes it must be reviewed before this private ABI is reused.
constexpr auto kAuditedLibrarySha256 = "63f23d7fd2ab74022b696187eb4a7528b036e4d0608f22853df392cd2c85b9f7";

class AuditedOperations final : public PersistentOverlayLibraryOperations {
 public:
  AuditedOperations() {
#if !defined(_M_X64) && !defined(__x86_64__)
    throw std::runtime_error("The audited configuration-library backend requires an x64 application.");
#endif
    std::array<wchar_t, 32768> systemDirectory{};
    const UINT length = GetSystemDirectoryW(systemDirectory.data(), static_cast<UINT>(systemDirectory.size()));
    if (!length || length >= systemDirectory.size()) throw std::runtime_error("Windows system directory could not be resolved.");
    const std::wstring systemPath(systemDirectory.data(), length);
    if (systemPath.size() + std::wstring_view(L"\\drivers\\uwfvol.sys").size() >= MAX_PATH)
      throw std::runtime_error("The UWF installation path exceeds the native feature check limit. No native API was called.");
    const std::wstring libraryPath = systemPath + L"\\uwfcfgmgmt.dll";
    m_libraryFile = CreateFileW(libraryPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (m_libraryFile == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not open the Windows UWF configuration library.");
    const auto closeOnFailure = qScopeGuard([this] {
      if (!m_initialized) release();
    });
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(m_libraryFile, &information) || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
      throw std::runtime_error("The Windows UWF configuration library is not a regular system file.");
    QCryptographicHash digest(QCryptographicHash::Sha256);
    std::array<char, 65536> buffer{};
    DWORD count = 0;
    do {
      if (!ReadFile(m_libraryFile, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
        throw std::runtime_error("Could not verify the Windows UWF configuration library.");
      if (count) digest.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(count)));
    } while (count);
    if (digest.result().toHex() != QByteArray(kAuditedLibrarySha256))
      throw std::runtime_error("This Windows UWF configuration-library build has not been audited. Run the compatibility probe before using it.");

    // The audited wrappers may try to enable the Windows feature when either
    // installation check fails. Require both checks beforehand, so an overlay
    // read or write refuses a missing or inaccessible installation.
    HKEY serviceKey = nullptr;
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\uwfvol", 0, KEY_READ, &serviceKey);
    if (opened != ERROR_SUCCESS) throw std::runtime_error("The installed UWF driver service could not be verified. No native API was called.");
    RegCloseKey(serviceKey);
    m_driverPath = systemPath + L"\\drivers\\uwfvol.sys";
    const DWORD attributes = GetFileAttributesW(m_driverPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
      throw std::runtime_error("The installed UWF driver file could not be verified. No native API was called.");
    m_driverFile = CreateFileW(m_driverPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (m_driverFile == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(m_driverFile, &information) ||
        (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
      throw std::runtime_error("The installed UWF driver file could not be held for verification. No native API was called.");

    // Match the driver's own CDevice::Initialize access and sharing flags.
    // GetReset otherwise treats a missing device as zero; SetReset can then
    // change overlay type. Keep the device open for the whole operation.
    m_device = CreateFileW(L"\\\\.\\UwfvolControl", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_device == INVALID_HANDLE_VALUE)
      throw std::runtime_error("The UWF control device could not be opened. No native API was called.");
    m_module = LoadLibraryExW(libraryPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m_module) throw std::runtime_error("The audited Windows UWF configuration library could not be loaded.");
    m_getFlags = resolve<GetFlags>("UwfCfgGetOverlayFlags");
    m_getReset = resolve<GetReset>("UwfCfgGetResetPersistentOverlay");
    m_setFlags = resolve<SetValue>("UwfCfgSetOverlayFlags");
    m_setReset = resolve<SetValue>("UwfCfgSetResetPersistentOverlay");
    m_initialized = true;
  }

  ~AuditedOperations() override { release(); }
  std::int32_t getFlags(const bool current, std::uint32_t& value) override {
    verifyInstallation();
    DWORD native = 0;
    const HRESULT status = m_getFlags(current, &native);
    if (status == S_OK) value = native;
    return static_cast<std::int32_t>(status);
  }
  std::int32_t getReset(std::uint32_t& value) override {
    verifyInstallation();
    DWORD native = 0;
    const HRESULT status = m_getReset(&native);
    if (status == S_OK) value = native;
    return static_cast<std::int32_t>(status);
  }
  std::int32_t setFlags(const std::uint32_t value) override {
    verifyInstallation();
    return static_cast<std::int32_t>(m_setFlags(value));
  }
  std::int32_t setReset(const std::uint32_t value) override {
    verifyInstallation();
    return static_cast<std::int32_t>(m_setReset(value));
  }

 private:
  // The Microsoft PDB uses C++ bool (one byte), not Win32 BOOL. Public
  // decorated symbols and machine code both establish these exact signatures.
  using GetFlags = HRESULT(__cdecl*)(bool, DWORD*);
  using GetReset = HRESULT(__cdecl*)(DWORD*);
  using SetValue = HRESULT(__cdecl*)(DWORD);
  template <class Function> Function resolve(const char* name) {
    const auto address = GetProcAddress(m_module, name);
    if (!address) throw std::runtime_error(std::string("The audited configuration library is missing ") + name);
    return reinterpret_cast<Function>(address);
  }
  void verifyInstallation() const {
    // Recheck immediately before each export; a prior successful read must not
    // authorize a later call after installation/device availability changes.
    // The held driver file prevents replacement/deletion during this worker.
    HKEY serviceKey = nullptr;
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\uwfvol", 0, KEY_READ, &serviceKey);
    if (opened != ERROR_SUCCESS)
      throw std::runtime_error("The UWF service is no longer accessible. No further native API was called.");
    RegCloseKey(serviceKey);
    const DWORD attributes = GetFileAttributesW(m_driverPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
      throw std::runtime_error("The UWF driver is no longer accessible. No further native API was called.");
    const HANDLE probe = CreateFileW(L"\\\\.\\UwfvolControl", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe == INVALID_HANDLE_VALUE)
      throw std::runtime_error("The UWF control device is no longer accessible. No further native API was called.");
    CloseHandle(probe);
  }
  void release() {
    if (m_module) { FreeLibrary(m_module); m_module = nullptr; }
    if (m_device != INVALID_HANDLE_VALUE) { CloseHandle(m_device); m_device = INVALID_HANDLE_VALUE; }
    if (m_driverFile != INVALID_HANDLE_VALUE) { CloseHandle(m_driverFile); m_driverFile = INVALID_HANDLE_VALUE; }
    if (m_libraryFile != INVALID_HANDLE_VALUE) { CloseHandle(m_libraryFile); m_libraryFile = INVALID_HANDLE_VALUE; }
  }
  HANDLE m_libraryFile = INVALID_HANDLE_VALUE;
  HANDLE m_driverFile = INVALID_HANDLE_VALUE;
  std::wstring m_driverPath;
  HANDLE m_device = INVALID_HANDLE_VALUE;
  HMODULE m_module = nullptr;
  bool m_initialized = false;
  GetFlags m_getFlags = nullptr;
  GetReset m_getReset = nullptr;
  SetValue m_setFlags = nullptr;
  SetValue m_setReset = nullptr;
};
}  // namespace

PersistentOverlayCommandResult executeAuditedOverlayLibrary(const PersistentOverlayAction action) {
  PersistentOverlayCommandResult result;
  result.executionFailed = true;
  try {
    initializeWmiRuntime();
    const auto shutdownWmi = qScopeGuard([] { shutdownWmiRuntime(); });
    if (action != PersistentOverlayAction::GetConfig && !isElevated())
      throw std::runtime_error("Run UWF Manager as administrator to change persistent overlay settings.");
    // Independently re-read the working WMI path inside the child. The GUI
    // snapshot can change between confirmation, backend selection and a write.
    if (probeUwfCapability() != UwfCapability::Available)
      throw std::runtime_error("UWF is unavailable. No native API was called.");
    auto& session = embeddedWmiSession();
    const auto filter = UwfFilter(session).read();
    const auto configs = UwfOverlayConfig(session).readAll();
    PersistentOverlayLibraryContext context{filter.currentEnabled, filter.nextEnabled, false, false, false, false};
    bool hasCurrent = false;
    bool hasNext = false;
    for (const auto& config : configs) {
      (config.currentSession ? hasCurrent : hasNext) = true;
      (config.currentSession ? context.currentDisk : context.nextDisk) = config.type == OverlayType::Disk;
    }
    if (!hasCurrent || !hasNext) throw std::runtime_error("Current or next UWF overlay configuration is missing. No native API was called.");
    // Runtime consumption and exclusion enumeration are unrelated to these
    // preconditions and can be unavailable after disabling the filter.
    if (action == PersistentOverlayAction::Reset) {
      const auto volumes = UwfVolume(session).readAll();
      context.currentProtected = std::ranges::any_of(volumes, [](const VolumeRow& volume) { return volume.currentSession && volume.isProtected; });
      context.nextProtected = std::ranges::any_of(volumes, [](const VolumeRow& volume) { return !volume.currentSession && volume.isProtected; });
    }
    AuditedOperations operations;
    result = PersistentOverlayLibrary(operations).execute(action, context);
    result.output.prepend(QStringLiteral("Backend: Windows configuration library (audited 10.0.26100.8115)\n"));
  } catch (const std::exception& error) {
    result.output = QString::fromUtf8(error.what());
  }
  return result;
}
}  // namespace uwf::api
