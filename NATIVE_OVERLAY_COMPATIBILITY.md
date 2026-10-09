# Audited native persistent-overlay compatibility

This fallback addresses a reported installation where UWF WMI operations work,
but every `uwfmgr.exe` command, including `help`, returns `0x80070005` without
output in both administrator and SYSTEM contexts. It does not patch Windows,
install features, replace system files, change licensing data, or retry as SYSTEM.

## Evidence and compatibility boundary

Static analysis used original Microsoft binaries and matching public PDBs.
No UWF exports, setters, IOCTLs, registry writes, or reboots were executed during
the analysis. Target hashes came from the supplied diagnostic report.

| Component | Version | SHA-256 | Evidence |
| --- | --- | --- | --- |
| uwfcfgmgmt.dll | 10.0.26100.8115, x64 | `63F23D7FD2AB74022B696187EB4A7528B036E4D0608F22853DF392CD2C85B9F7` | Exact target match |
| uwfmgr.exe | fixed version 10.0.26100.9278 | `1BDED4CD3E1E302E8B45A08102F60D0F08BB5A692D29BA5036F2880C3AFA0C67` | Exact target match; displayed version string differs |
| uwfvol.sys | 10.0.26100.9444 | `04FC7F09C3CA2FDA6FED8783D6FC9F902959852D6FE2DD073182B4D9850153F7` | Exact target match |
| uwfrtl.sys | 10.0.26100.9444 | `DB91635CA387F502173B32CE57E4CC6398ACCEFEEBC7A78BA2E195A0FB60984F` | Microsoft reference; target file was not included in the report |

Microsoft primary artifacts:

- [Configuration DLL](https://msdl.microsoft.com/download/symbols/uwfcfgmgmt.dll/93F5911832000/uwfcfgmgmt.dll), [matching PDB](https://msdl.microsoft.com/download/symbols/UwfCfgMgmt.pdb/5AA74F07A6770A945EE8B039D9E3EB131/UwfCfgMgmt.pdb).
- [CLI binary](https://msdl.microsoft.com/download/symbols/uwfmgr.exe/4A65064237000/uwfmgr.exe), [matching PDB](https://msdl.microsoft.com/download/symbols/uwfmgr.pdb/72F5DF870EDB61283255924EE1F86CCF1/uwfmgr.pdb).
- [Volume driver](https://msdl.microsoft.com/download/symbols/uwfvol.sys/9999215C33000/uwfvol.sys), [matching PDB](https://msdl.microsoft.com/download/symbols/uwfvol.pdb/B49BC94815F9998EA8DC0EB13C660A671/uwfvol.pdb).
- [Runtime driver reference](https://msdl.microsoft.com/download/symbols/uwfrtl.sys/81DA772316000/uwfrtl.sys), [matching PDB](https://msdl.microsoft.com/download/symbols/uwfrtl.pdb/FB8701BBB10DE5020773E1A59E731DA71/uwfrtl.pdb).

These are research references, not files redistributed in the application.
Production accepts only the exact configuration DLL hash above. Export presence,
Windows edition, version strings, and a broad OS build range do not prove ABI
compatibility. A Windows update changing the DLL requires a new audit.

## Why the CLI can fail while WMI works

The matched CLI's `wmain` at RVA `0x1318c` calls `IsFeatureEnabled` (`0x75c4`)
before argument parsing, banner output, or COM initialization. False returns
`0x80070005` immediately. The check references
`EmbeddedFeature-UnifiedWriteFilter-Enabled`. This path explains the observed
empty `help` result; the actual licensing-policy value was not read from the
target, so a SKU-specific refusal remains an inference.

The four configuration DLL wrappers do not use this CLI gate. This difference
supports the fallback; it does not prove every transplanted installation or
every write will work. The existing WMI path remains responsible for filter,
volume, overlay-type, and restart operations.

## Verified private ABI

Microsoft public PDB names and x64 register use establish these signatures:

```cpp
HRESULT __cdecl UwfCfgGetOverlayFlags(bool currentSession, DWORD* flags);
HRESULT __cdecl UwfCfgGetResetPersistentOverlay(DWORD* mode);
HRESULT __cdecl UwfCfgSetOverlayFlags(DWORD flags);
HRESULT __cdecl UwfCfgSetResetPersistentOverlay(DWORD mode);
```

`bool` is C++ one-byte bool, not Win32 BOOL. `DWORD` and HRESULT are 32-bit.
The matching decorated getter symbol is
`?UwfCfgGetOverlayFlags@@YAJ_NPEAK@Z`; true selects current, false selects next.
No handle or explicit initialization argument is required. Only `S_OK` is
accepted; positive HRESULTs cannot authorize writes or restarts.

Persistent mode is bit `0x2`. Bit `0x4` denotes read-only media/HORM configuration,
not proof that HORM is currently enabled. All other bits are preserved.
[Microsoft's CSP documentation](https://learn.microsoft.com/en-us/windows/client-management/mdm/unifiedwritefilter-csp)
documents these flag meanings and reset modes; it does not document this C ABI.

## Wrapper behavior and guards

The exported wrappers call an installation quick-check. If the check fails,
they may enter a DISM feature-enable path. Before loading/calling them, the
worker requires the same token to open the `uwfvol` service registry key with
KEY_READ and find `System32\drivers\uwfvol.sys`. The native check uses a
MAX_PATH buffer, so the application also refuses paths that exceed that limit.
The DLL and driver files are held with FILE_SHARE_READ, and installation
availability is checked immediately before every export. An absent or
inaccessible installation fails instead of invoking an export to install it.

Before each reset get/set, the worker opens and holds `\\.\UwfvolControl` with
GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING,
FILE_ATTRIBUTE_NORMAL. Without this guard, GetReset can report zero for a
missing device, and SetReset can normalize overlay type instead of scheduling
a genuine reset.

Flags get/set do not require this device. The matched volume driver's DriverEntry
at `0x2d03c` skips device creation when global configuration UWFEnabled is zero;
the mapping of that field to UWFEnabled is established by the runtime-driver
reference. The DLL's management initialization at `0x17ae0` permits device-open
failure, and flags get/set operate on configuration. When current WMI filter
state is disabled, the report reads only flags and explicitly labels reset
state unavailable. This permits initial persistence setup without treating a
missing device as a confirmed zero reset mode.

The wrapper's setup sends IOCTL `0x2249c3`; the matched volume driver dispatches
it to `UwfrtlCfgApiBreakpoint`. In the Microsoft runtime-driver reference above,
the function at RVA `0xb170` emits TRACE and returns zero. The target runtime
driver hash was not independently reported, so this last step is reference
evidence rather than an exact target match.

Reading can initialize Windows' current-configuration cache: it copies the
selected Copy0/1 to CopyV and creates a volatile UsingCopyV marker when absent.
CopyV itself is not proved volatile. The analysed path does not modify baseline
Copy0/1, UpdatedSettings, PersistentParameters, protected volumes, overlay flags,
or file staging. This specific cache initialization is part of runtime reads;
the compatibility diagnostic package only enumerates exports and never invokes
them. Getters are not described as unconditionally registry-immutable.

SetOverlayFlags requires the *current* filter to be disabled. Setting only the
next filter to disabled is insufficient. Enable/disable reads next flags,
changes only bit `0x2`, writes once, and requires an exact read-back match.

The native flags setter also updates the corresponding UWF boot-persistence
BCD element (`0x16000085`). BCD open/set failures propagate as HRESULT failures;
the static flags may already have changed when this occurs. A later auxiliary
UWF registry-driver commit return is not propagated by the native setter.
Read-back confirms next configuration flags, not independent BCD persistence
or boot behavior. The required disabled-filter session avoids intentionally
scheduling this configuration change through an active protected overlay;
real reboot validation remains necessary.

Restore requires refreshed current/next WMI state with enabled filters, Disk
overlay and protected volumes, plus persistent bit `0x2` in both configurations.
Read-only media/HORM configurations are refused. Reset uses literal mode 1;
cancellation uses 0. Mode 255 resolves saved-mode behavior and is never sent to
the setter. Each write requires successful read-back before further action.

## Isolation and verification limits

A fixed-action child process executes the private API before any single-instance,
GUI, enhanced-service or staging initialization. The parent enforces a bounded
timeout and validates both the JSON report and normal process completion.
Crashes, timeouts, unknown modes, read-back mismatch and invalid reports stop
further action. A failed write is never replayed via another backend.

The existing Restore controller still requires user confirmation and enhanced
service acknowledgement to skip preshutdown staging. Failed or unverifiable
reset requests do not authorize reboot. A failed reboot after a verified reset
can leave that reset scheduled; cancellation remains available.

In-memory tests cover the ABI adapter's policy, flag preservation, failures,
read-back checks, process-report protocol, UI gates and staging suppression.
They do not call the real DLL or prove persistence/recovery on hardware. The
user's target installation must still verify ordinary reboot retention and
explicit Restore. [Microsoft marks persistent overlay experimental](https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwfoverlay).
