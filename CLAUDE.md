# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
cmake -B build -G Ninja
cmake --build build
./build/PhoneToolbox
```

Clean build:
```bash
rm -rf build && cmake -B build -G Ninja && cmake --build build
```

Windows static build (requires vcpkg with Qt6 + libusb):
```powershell
.\build_windows.ps1 -Static
```

## Architecture Overview

PhoneToolbox is a **Qt6/C++17** desktop app for Android device management across 6 modes: ADB, Fastboot, Fastbootd, EDL 9008, MTK DA, Recovery.

### Key Subsystems

```
src/
  core/
    adb_embedded.cpp/h       # ADB acquisition: PATH → ANDROID_HOME → Google download
    device_detector.cpp/h    # 2s-timer polling, multi-mode detection, bootloader status
    device_info.cpp/h        # DeviceInfo model (serial, model, patch, SDK, CPU, battery...)
    flash_tool.cpp/h         # Flash/burn core: routes to fastboot/ADB/EDL/MTK per mode
    filename_parser.cpp/h    # ROM name parser: 18+ ROM types, CSV model DB lookup
    restart_tool.cpp/h       # Reboot to system/recovery/bootloader/fastbootd/EDL/shutdown
    modes/
      edl_9008.cpp/h         # EDL USB detection (libusb VID/PID enumeration)
      edl_handler.cpp/h      # Sahara protocol + Firehose XML over USB (based on bkerler/edl)
      mtk_handler.cpp/h      # MTK DA JSON-RPC bridge to mtk_binary (based on bkerler/mtkclient)
      normal_mode.cpp/h      # Placeholder
  ui/
    main_window.cpp/h        # QSplitter layout, signal wiring, drag-drop ROM loading
    tool_panel.cpp/h         # Left panel: device list + tool selector (4 tools) + restart
    device_info_panel.cpp/h  # Device info display with SelectableLabel
    flash_panel.cpp/h        # Flash UI: partition list, flash/erase/format/FRP/brick-repair
    system_tool_panel.cpp/h  # System tools: 7 categories (perf/UI/partition/apps/security/debug/Xposed)
    live_chart_widget.cpp/h  # Real-time multi-series line chart widget
    vuln_panel.cpp/h         # Vulnerability scanning & exploitation UI
    output_panel.cpp/h       # Timestamped color-coded log output
  vuln_db/
    vuln_entry.cpp/h         # VulnEntry model: CVE, AffectedRange, ExploitScript, PayloadFile
    vuln_db.cpp/h            # In-memory DB, JSON serialize/deserialize
    vuln_matcher.cpp/h       # Device-to-CVE matching engine (version/patch/SDK/platform)
    exploit_engine.cpp/h     # Three-phase ADB shell exploit runner (detect→exploit→verify)
    vuln_importer.h          # Abstract importer interface
    importers/
      local_importer.cpp/h   # Local JSON file + URL importer
```

### Data Flow

1. **AdbEmbedded** (singleton) finds or downloads `adb`/`fastboot`, starts ADB server.
2. **DeviceDetector** polls every 2s, probes ADB → Fastboot → EDL USB → MTK DA in sequence. Emits deviceConnected/Disconnected/ModeChanged signals.
3. **MainWindow** routes device signals to ToolPanel (device list) and OutputPanel (log). Tool selection switches QStackedWidget index.
4. **FlashTool** dispatches operations to the appropriate protocol handler based on device mode.
5. **ExploitEngine** runs ADB shell scripts against matched CVEs through QProcess.

### Device Mode Detection Order

`checkDevices()` runs this order:
1. **ADB**: `adb shell getprop ro.build.version.sdk`
2. **Fastboot/Fastbootd**: `fastboot devices -l` + `getvar is-userspace` to distinguish
3. **EDL 9008**: libusb VID/PID match (0x05c6:0x9008 etc.)
4. **MTK DA**: libusb VID/PID match (vendor-specific)

### Bootloader Lock Detection

`getBootloaderStatus()` probes in order:
- `fastboot oem device-info` (Google Pixel)
- `fastboot oem get-bootinfo` (Huawei)
- `getvar unlocked` (generic)
- `getvar oem unlocking` (fallback)

### Key Patterns

- **Singleton**: `AdbEmbedded::instance()` for ADB process management
- **Signal/Slot**: All cross-component communication via Qt signals
- **Polling**: 2s QTimer for device detection; 0.5s for performance monitoring
- **Protocol Bridging**: mtk_handler spawns `mtk_bridge` as a subprocess, communicates via JSON-RPC over stdin/stdout
- **QProcess for ADB**: All device communication through `adb shell` / `fastboot` processes

## Git Submodules

```bash
git submodule update --init
```

The repo includes two GPLv3 submodules for reference/license:
- `edl/` → https://github.com/bkerler/edl (Sahara/Firehose protocol)
- `mtkclient/` → https://github.com/bkerler/mtkclient (MTK DA protocol)

## Dependencies (Linux)

```
pacman -S qt6-base qt6-tools cmake ninja pkg-config
# Ubuntu: apt install qt6-base-dev qt6-tools-dev cmake ninja-build pkg-config libusb-1.0-0-dev
```

## Project Conventions

- C++17
- Qt6 (Core, Widgets, Network)
- AUTOMOC enabled — Q_OBJECT classes auto-handled
- Member variables prefixed `m_`
- UI panels emit `outputMessage(QString, bool isError)` and `switchToDeviceInfo()` signals
- No unit tests yet
