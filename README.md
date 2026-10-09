# UWF Manager

A Qt GUI for the Windows Unified Write Filter (UWF) — a convenient graphical front end for inspecting and configuring UWF state, alongside the built-in `uwfmgr.exe` command line.

[简体中文](README.zh_CN.md)

![Screenshot](snapshot/snapshot.1.png)

![Screenshot](snapshot/snapshot.2.png)

## About UWF

The Unified Write Filter is a sector-level write-protection driver shipped with Windows Enterprise, Education, IoT Enterprise, and the LTSC variants. When a volume is protected, every write to it is intercepted and redirected to an *overlay* — either a region of RAM or a sparse file on a designated disk volume — instead of touching the underlying sectors. By default the overlay is discarded on reboot; persistent Disk overlay can instead retain it until a manual reset. File and registry-key exclusions can be declared so that specific paths bypass the overlay and write through to disk; overlay contents can also be selectively *persisted* (committed back to the underlying media) before reboot.

UWF is configured per-volume and operates with two parallel state sets: the *current* session (read-only, reflecting what the driver is enforcing right now) and the *next* session (writable, taking effect after reboot). All configuration changes — enabling protection, setting the overlay type and size, adding exclusions — apply to the next session.

UWF is the supported successor to the older Enhanced Write Filter (EWF) and File-Based Write Filter (FBWF), and is the standard mechanism for building stateless kiosks, point-of-sale terminals, ATMs, medical devices, classroom workstations, and digital signage on Windows.

> **Note**: *Servicing mode* (`uwfmgr filter enable-servicing` / `disable-servicing`, used to suspend the filter for Windows Update or scheduled maintenance) is not planned.

## Features

- Filter enable / disable, current and next session
- Overlay configuration: type (RAM / Disk), maximum size, warning and critical thresholds
- Per-volume protection toggle and drive-letter / volume-ID binding
- File and directory exclusion lists per volume
- Registry exclusion list (system volume)
- DomainSecretKey / TSCAL persistence switches, shown and toggled inline in the registry exclusion list
- Persist overlay contents back to disk or registry (files, directories, file deletions, registry keys)
- File staging: persist a user-defined file/folder list in the registry and commit its current contents automatically before safe shutdown or restart
- Read-only enumeration of overlay file entries
- Import uwfmgr commands — paste or load a command script and stage each line as a pending change
- Persistent disk overlay and manual restore: preserve changes across normal restarts; schedule/cancel a next-boot reset or restore and restart without File staging commits
- System restart and shutdown
- In-app log viewer
- Portable by default — file staging is the only feature that stores application state (its path list is kept in the registry); no configuration files are created, and the in-app log remains in memory and is discarded when the process exits

## Out of scope

- HORM (Hibernate Once / Resume Many)
- Servicing mode
- Free-space passthrough, read-only media mode, swapfile creation

## Persistent overlay and manual restore (added in this fork)

Open **Manual restore** in the toolbar. Boot with UWF disabled in the current session, apply a Disk overlay using the existing settings, enable persistence, and follow the prerequisite instructions to enable UWF and protect volumes. Restart for configuration changes to take effect. Normal restarts then retain overlay data; **Restore and restart** requests a reset for the next boot and restarts without committing File staging. Restoration still requires a reboot.

Excluded data and previously committed changes remain on physical storage. The enhanced-mode service must acknowledge skipping preshutdown staging before a restore restart can proceed. The dialog shows the native Windows configuration report without parsing localized text into assumed state. Microsoft marks persistent overlay as experimental; validate on a test device and monitor accumulating overlay usage. See [Microsoft documentation](https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwfoverlay) and the [Chinese setup and acceptance guide](MANUAL_RESTORE.zh_CN.md).

If the CLI configuration read returns Access denied, an isolated worker can use an audited system configuration DLL. This fallback accepts one exact x64 DLL hash and verifies writes by reading back; unknown versions stop for diagnosis. No Windows components are bundled or replaced. See the [native compatibility audit](NATIVE_OVERLAY_COMPATIBILITY.md).

## Requirements

- Windows 10 / 11 Enterprise, Education, IoT Enterprise, or LTSC variants
- "Unified Write Filter" Windows feature enabled
- Administrator privileges — needed to apply changes

Note: the program still starts when these conditions are not met.

## Build

- C++20
- CMake ≥ 3.16
- Qt 6 (Widgets, Svg, LinguistTools)
- A Windows-targeting compiler (MSVC, clang-cl, MSYS2 clang64, mingw-w64)

```sh
cmake -S . -B build
cmake --build build --config Release
```

The fork's Windows x64 GitHub Actions workflow builds and runs isolated behavior tests, then packages the application with its Qt dependencies, exact commit source, and SHA256 checksums. Extract the entire portable ZIP before running `UWF.exe`. CI sets `UWF_SANITIZE=OFF` and `UWF_STATIC_RUNTIME=OFF` for shared MSYS2 CLANG64 Qt; the upstream static-runtime default is preserved.

## AI-generated code

Most of the source in this repository was produced by AI coding assistants under human review and direction. Treat the code accordingly: it is functional but you should verify any non-obvious behavior against the source, the WMI documentation, or the running system before relying on it.

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).
