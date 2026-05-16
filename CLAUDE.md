# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
cmake -B build -G "Unix Makefiles"
cmake --build build
./build/PhoneToolbox
```

Clean build:
```bash
rm -rf build && cmake -B build -G "Unix Makefiles" && cmake --build build
```

## Project Overview

PhoneToolbox is a **Qt6/C++17** cross-platform desktop application for Android device management via ADB, Fastboot, Fastbootd, EDL, and MTK DA modes. It embeds platform-specific ADB/fastboot binaries and extracts them at runtime.

### Dependencies (Linux)

- Qt6 (Core, Widgets, Network)
- libusb-1.0 (via pkg-config)
- pthread

### Directory Structure

```
CMakeLists.txt              # Qt6 + libusb + pthread build config
src/
  main.cpp                  # App entry: QApplication + Fusion style + MainWindow
  core/
    adb_embedded.cpp/h      # Singleton that extracts & manages ADB/fastboot processes
    device_detector.cpp/h   # 2s-timer polling for devices, multi-mode detection
    device_info.cpp/h       # DeviceInfo data class (serial, model, bootloader, etc.)
    restart_tool.cpp/h      # Device reboot operations (system/recovery/bootloader/EDL/shutdown)
  ui/
    main_window.cpp/h       # Top-level window, QSplitter layout, signal wiring
    tool_panel.cpp/h        # Left panel: device list + restart mode selector
    device_info_panel.cpp/h # Top-right panel: selectable device info labels
    output_panel.cpp/h      # Bottom-right panel: timestamped command output log
    device_widget.cpp/h     # Stub (unused)
    info_panel.cpp/h        # Stub (unused)
resources/
  resources.qrc             # Qt resource file bundling ADB/fastboot per platform
third_party/
  adb_binaries/             # Prebuilt adb/fastboot for linux, windows, macos
```

### Architecture & Data Flow

1. **AdbEmbedded** (singleton) extracts the platform's adb/fastboot from Qt resources to a temp directory, starts the ADB server.
2. **DeviceDetector** runs a 2-second QTimer, calling `checkDevices()` which detects devices in order: Fastboot → ADB → EDL → MTK DA. It emits `deviceConnected` / `deviceDisconnected` / `deviceModeChanged` signals.
3. **MainWindow** receives device signals and routes them to **ToolPanel** (update device list) and **OutputPanel** (log messages). Device selection in ToolPanel triggers **DeviceInfoPanel** updates.
4. **RestartTool** handles reboot commands. It reads the device's current mode and target mode, then issues the appropriate ADB/Fastboot command.

### Key Patterns

- **Singleton**: `AdbEmbedded::instance()` for ADB process access
- **Signal/Slot**: All inter-component communication via Qt signals
- **Polling**: `QTimer`-based device detection (2s interval)
- **Embedded Binaries**: Platform-specific ADB/fastboot bundled via `.qrc`, extracted to `QTemporaryDir`
- **Device Modes**: `DeviceDetector::DeviceMode` enum (ADB, FASTBOOT, FASTBOOTD, EDL_9008, MTK_DA, RECOVERY, UNKNOWN)

### Detected Mode Flow

When `DeviceDetector::detectDeviceMode()` runs, it tries ADB first (`shell getprop`), then falls back to Fastboot detection, EDL USB detection, and MTK DA detection. Fastbootd is distinguished from traditional Fastboot via `getvar is-userspace` or the `[fastbootd]` marker in `fastboot devices -l` output.

### Bootloader Lock Status

`DeviceDetector::getBootloaderStatus()` probes in order: `fastboot oem device-info` → `fastboot oem get-bootinfo` → `getvar unlocked` → `getvar oem unlocking`. Supports Xiaomi, Huawei, Google Pixel, and generic devices.

### Restart Mode Mapping

| Target Mode | ADB Command | Fastboot Command |
|---|---|---|
| System | `reboot` | `reboot` |
| Recovery | `reboot recovery` | `reboot recovery` |
| Bootloader | `reboot bootloader` | `reboot bootloader` |
| Fastbootd | `reboot fastboot` | `reboot fastboot` |
| EDL | `reboot edl` | N/A |
| Shutdown | `reboot -p` | `reboot` (might not work) |

### Custom Widgets

- `SelectableLabel` (in device_info_panel.cpp) — QLabel subclass with text selection + right-click copy/select-all
- `CopyableTextEdit` (in output_panel.cpp) — QTextEdit subclass with "copy all" context menu action
