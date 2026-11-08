# deskflow-otg

[简体中文](README.zh-CN.md)

A bridge connecting [Deskflow](https://github.com/deskflow/deskflow) (keyboard and mouse sharing) with [scrcpy](https://github.com/Genymobile/scrcpy)'s **OTG mode** (USB AOA HID).

Run a Deskflow **server** on the host computer and then run this program. With the phone connected to the host by USB, moving the mouse onto the "phone" screen lets you control the Android phone directly with the host keyboard and mouse.
The USB HID data path **does not require adb or USB debugging** (similar to `scrcpy --otg`). On Windows, however, the phone's USB interface must still be bound to a driver usable by libusb. See [Windows USB OTG device recognition](docs/WINDOWS_USB.md) for Windows limitations and troubleshooting.

Both a command-line interface and an optional **Qt desktop GUI** are provided. The GUI can optionally use per-device ADB enhancements to detect dimensions and orientation, configure and start an installed Deskflow server, and connect to the phone with one click. USB debugging is only needed for the selected ADB enhancements; manually entered device information and automatic USB connection for remembered phones do not require it. See [Desktop GUI](#desktop-gui) for details.

```
┌─────────────────────────────── Host computer ───────────────────────────┐
│                                                                        │
│   deskflow server (with the real keyboard and mouse)                   │
│        │                                                               │
│        │  Deskflow protocol (TCP :24800)                               │
│        ▼                                                               │
│   deskflow-otg  ──(USB AOA HID)──▶ Android phone                       │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

In Deskflow, the phone appears as a normal "screen" that can be placed to the left, right, above, or below the host screen.

## How it works

- **Deskflow side:** This program implements a minimal Deskflow *client* (secondary screen) in C. It speaks the Deskflow/Synergy/Barrier TCP binary protocol directly, including the handshake, screen information exchange, keyboard and mouse event stream, keep-alive, and CNOP replies. The protocol format is based on `src/lib/deskflow/ProtocolTypes.h` from Deskflow.
- **scrcpy/OTG side:** Through libusb, the program sends **AOA 2.0 HID** control requests directly to the phone (`ACCESSORY_REGISTER_HID`=54 / `SET_HID_REPORT_DESC`=56 / `SEND_HID_EVENT`=57), encoding keyboard and mouse events as standard USB HID reports. USB transport, keyboard, and relative mouse code are based on scrcpy's `app/src/usb/aoa_hid.c`, `app/src/hid/hid_keyboard.c`, and `app/src/hid/hid_mouse.c`. The default pointer uses an **HID digitizer absolute coordinate** device with hover support, mapping Deskflow coordinates directly to the phone's full screen to avoid coordinate drift caused by Android mouse acceleration.

The complete path is: `host keyboard and mouse → deskflow → this program → AOA HID → phone`.

The experimental Linux/Windows audio mode uses scrcpy's Android Server: AOA HID/BLE HID carries keyboard and mouse input, while ADB independently forwards Android audio to the host (ALSA on Linux and WASAPI on Windows). See [Audio forwarding](docs/AUDIO.md) for build and usage instructions. Without `--audio`, the original HID mode, which does not require USB debugging, is unchanged.

Optional [text clipboard sharing](docs/CLIPBOARD.md) follows Deskflow's ownership and screen-switch synchronization rules. It reads and writes the phone clipboard through the ADB/scrcpy control channel and does not depend on the system clipboard of the computer running the GUI. After building with `-DBUILD_CLIPBOARD=ON`, enable it per device in the GUI or use `--clipboard`; it is disabled by default. Clipboard sharing is independent of audio but uses the same device ADB/server session. When enabled, copying on the phone in the background may also update the shared clipboard.
The audio ADB transport can be selected independently with `--adb-serial`, for example `192.168.1.20:5555`, while `--serial` continues to identify only the USB AOA HID device. This is useful on Windows when the ADB USB driver and WinUSB cannot be used at the same time. BLE connections require an explicit wireless ADB target for the same phone.
On Windows, the GUI's **Wireless ADB setup** wizard discovers mDNS pairing/connection services, runs `adb pair` and `adb connect`, and saves the selected `host:port` target. Pairing still requires the one-time code shown by Android; the companion app cannot read or bypass that code.

Linux also provides a **Bluetooth LE HID** transport, allowing the phone to be controlled without USB. The CLI and GUI use the same `deskflow-otg` executable; the persistent GATT service and bridge session are separate processes, not additional executables. USB remains the default transport. macOS currently has no Bluetooth backend. Windows provides an [experimental BLE backend](docs/WINDOWS_BLE.md) that must be explicitly enabled at build time and can be used from either the CLI or GUI.

The standalone [minimal test prototype](experiments/windows-ble-hid/README.md) for native Windows BLE HID is retained for step-by-step real-device validation. Manual keyboard, hover, click, drag, and wheel tests have passed on a Nova 9, but this does not constitute acceptance of continuous Deskflow input, reconnection, or compatibility with all phones.

## Dependencies

- Linux, macOS (Intel / Apple Silicon), or Windows (x64)
- GCC / Clang / MSVC (C11)
- CMake ≥ 3.16
- libusb-1.0 development files and OpenSSL 1.1.1+/3.x development files

The Linux BLE HID backend additionally requires BlueZ and GLib/GIO development files:

```bash
sudo apt install libusb-1.0-0-dev libssl-dev libglib2.0-dev bluez
```

The BLE backend is enabled by default on Linux. To build only the original USB CLI, use `cmake -S . -B build -DBUILD_BLE=OFF`; GLib/GIO and pkg-config are then not required. BLE is disabled by default on non-Linux systems.

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake libusb-1.0-0-dev libssl-dev

# macOS (install Homebrew first)
xcode-select --install
brew install cmake libusb openssl@3

# Windows: Visual Studio C++ toolchain, CMake, and vcpkg
vcpkg install libusb:x64-windows-static openssl:x64-windows-static
cmake -S . -B build `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
```

For a GUI build, also install `openssl:x64-windows-static-md`; the GUI uses
the `/MD` OpenSSL build required by Qt. If OpenSSL is installed elsewhere,
set `OPENSSL_ROOT_DIR` when configuring.

The command-line bridge does not require Qt, FFmpeg, or SDL. This is a major advantage over directly linking the Deskflow/scrcpy libraries (scrcpy's `common.h → compat.h` pulls in FFmpeg headers, while Deskflow pulls in Qt). The optional GUI additionally requires a C++17 compiler and Qt 6.8+ (Widgets, Network, and LinguistTools; the Test module is also required for tests).

## Desktop GUI

The GUI supports Windows x64, Linux x86_64, and macOS Intel / Apple Silicon. GUI release packages include the bridge program, Qt runtime, and ADB 36.0.2; **Deskflow is not included** and must be installed separately. At startup, the GUI looks for the bundled ADB first, and an external ADB can also be selected in the interface.

The GUI supports Chinese and English. The language can be changed in the upper-right corner and takes effect immediately while being saved automatically. On first launch, the system language is used (Chinese on Chinese systems and English elsewhere). Changing the language does not change device or connection parameters or interrupt a connection; existing logs and raw ADB/Deskflow/bridge subprocess output are not translated again.
The GUI follows the desktop environment's light/dark color scheme and updates when the system theme changes; it does not save a separate theme override. It uses Qt's official ID-based internationalization, with strings referenced in code through stable IDs such as `qtTrId("otg.action.connect")`.
Linux distributions also bundle the GTK and desktop-portal theme plugins when available in the build's Qt installation. Qt selects the integration for the current desktop; the application does not force GTK or override theme environment variables. If no suitable plugin or optional runtime is available, Qt's default appearance remains usable. DEB packages recommend GTK 3 only when the GTK plugin is bundled. Appearance follows the target desktop's installed theme, so different distributions need not look identical; third-party KDE/Qt style plugins are not bundled.
Chinese and English are maintained in `gui/translations/deskflow_otg_zh_CN.xml` and `deskflow_otg_en.xml`. They are compiled and embedded during the build with `lrelease -idbased -nounfinished`, so no translation files need to be copied separately. The English catalog is always the fallback layer; missing Chinese translations display English rather than the ID.

The files still use the Qt TS XML format, but have an `.xml` suffix so that IDEs do not mistake them for TypeScript. When adding a string, add the same `<message id="otg.…">` to both XML files, use the same English text in `<source>`, and fill in the Chinese and English `<translation>` elements respectively. Changing wording does not require changing the ID. Maintain the catalogs directly or with Qt Linguist; do not regenerate them from the code with `lupdate` (the code contains no source strings). The `gui_logic` tests check ID uniqueness, bilingual coverage, code references, placeholders, and the actual lookup results after compilation.

### Usage flow

1. Start `bin/deskflow-otg-gui` (`.exe` on Windows); on macOS, open `deskflow-otg-gui.app`.
2. The device page has a per-device **ADB enhancements** switch. Leave it off for phones without USB debugging; USB discovery uses `usb-scan`, dimensions are entered manually, and HID remains fully available. When it is on, Linux/macOS may use USB ADB discovery and the device's ADB target for screen information and optional features. The ADB executable path remains global.
3. Select the phone. With ADB enhancements enabled, the GUI reads `wm size`, prefers Override size, and swaps width and height according to the screen orientation. With ADB disabled, enter the dimensions manually. On Windows, USB/HID discovery always uses `usb-scan`; screen dimensions can be read before USB insertion by entering a wireless ADB `host:port` target in the ADB enhancements section, or by using **Wireless ADB setup** to pair and connect it. The result is treated as the "original screen size". A "sensitivity multiplier" (1–10×) can proportionally reduce the virtual size, and the effective virtual size is shown live. When "Follow screen rotation" is enabled, the ADB session used by audio or clipboard monitors orientation changes and updates the screen size dynamically without restarting the audio, clipboard, or HID session. This option is disabled by default; when disabled, disconnect and reconnect after refreshing or swapping width and height.
4. Select "Automatically configure and start local Deskflow" and choose whether the phone is to the left, right, above, or below the computer. Common installation locations are searched automatically. If none is found, select the **deskflow-core / deskflow-server executable**, not the Deskflow GUI. On macOS it is usually under `/Applications/Deskflow.app/Contents/MacOS/`.
5. Click "Connect". The state changes to "Connected" only after USB HID initialization succeeds and Deskflow accepts the screen information.
6. Closing the window hides it in the system tray without interrupting the connection. Click the tray icon or "Show window" to restore it; the tray menu also provides "Disconnect".
7. Move the mouse back to the computer and choose "Quit" from the tray menu. The GUI releases bridge keyboard and mouse input before stopping the server it started. If the desktop has no system tray, closing the window still stops the connection and exits.

When there is only one ADB device, it is selected automatically; with multiple devices, select one manually. The GUI queries the ADB device path and passes the serial number to the bridge core for an exact match with the actual USB descriptor; it does not fall back to an arbitrary device. If the vendor's ADB and USB identifiers differ, manually correct the value using the candidate USB serial numbers listed in the bridge log. Without USB debugging, use manual mode to enter the serial number and dimensions. The native Windows ADB USB backend may return an `unknown` path; in that case, dimension reading continues and the bridge core verifies the USB identity.

### Optional Android companion app

 The USB device page provides **Install / update APK**, **Open phone setup**, and **Check permission** for the [Android companion app](docs/ANDROID_COMPANION.md). The companion home screen also provides a **Wireless ADB helper** that opens Android's Wireless debugging/Wi-Fi settings and displays or copies the phone's IPv4 address. Version 0.1.12 monitors USB connection state as well as accessory removal, immediately requesting pause for all playing/buffering sessions on disconnect, exposes a DUMP-protected `KeyguardManager` lock-state query for the optional ADB-assisted unlock flow, and includes the wireless ADB helper. Version 0.1.10 and earlier do not provide that stable lock-state query; the desktop falls back to `dumpsys` heuristics. A manifest receiver handles the event even if the notification-listener service is temporarily not bound; the ROM must still allow the app to run and must not force-stop it. Brief re-enumeration or unplug/replug may also pause playback. This applies to all USB data connections, not only Deskflow. Setup requires authorized ADB and manual notification-access approval on the phone; no APK is installed automatically, and keyboard/mouse operation does not depend on it. Starting with desktop 0.1.7, GUI release packages for Linux, Windows, and both macOS architectures bundle the same CI-built, release-signed companion APK. The APK field defaults to that bundled file, also replacing empty or missing paths saved by older installations; valid custom APK selections are preserved. Local builds may supply `-DCOMPANION_APK=/path/to/signed.apk`. Release CI requires the signing secrets described in the guide and fails rather than shipping an unsigned/debug APK or omitting it. Permission checks do not prove ROM compatibility or that a player accepted the pause request. See the linked guide for target selection, packaging, and limitations.

**Phone-side requirements:** On ROMs with auto-start/background-start controls, allow deskflow-otg to start and run in the background, and set its battery policy to **Unrestricted / No restrictions** or equivalent. Open the app once after installation or an APK update and enable notification access. The manifest USB receiver is independent from notification-listener service binding, but a force-stopped or aggressively blocked app cannot receive broadcasts; if media access was revoked, pausing is unavailable. The GUI permission check does not verify auto-start, battery settings, process launch, or player compatibility. See [phone-side requirements and background restrictions](docs/ANDROID_COMPANION.md#手机端生效条件与后台限制).

Companion **0.1.4** also displays the actual Deskflow session state reported by the updated desktop GUI over already-authorized ADB. Only the completed Deskflow handshake counts as connected; reports expire to **Status unknown** after 15 seconds without confirmation. This optional status channel does not require notification access, does not connect/pair ADB automatically, and does not affect HID or immediate USB-disconnect pause. Open the app once after installation. CLI sessions and connections without ADB cannot report status; phone-side broadcast delivery still requires real-device validation.

The GUI's **Middle button action → Android Home (Win+Enter)** and **Right button action → Android Back (Win+Backspace)** send Android system keyboard shortcuts directly over HID instead of the corresponding mouse button. **No companion app, accessibility service or ADB is required.** The two mappings are independent, saved per device and default to normal mouse buttons. CLI: `--middle-button-action normal|home` and `--right-button-action normal|back`. USB and supported BLE transports share the same implementation; the phone ROM must support these system shortcuts.

Each mouse press sends a complete press/release sequence, so holding the mouse button does not keep Meta or Enter/Backspace pressed. Existing keyboard state is preserved. If the shortcut key is already physically held or all six keyboard slots are occupied, the click is consumed without a shortcut rather than releasing/repressing physical keys. Previous desktop builds used F13/F14 with the companion accessibility service; that legacy service remains in APK 0.1.8, but is not used by the updated desktop mappings.

### Share to computer and file transfer

With companion app **0.1.6+** and an already-authorized ADB connection, the desktop **Files & sharing**
window can receive phone shares and send images/files to the phone. Share text, one image, or multiple
files through Android's **Share to computer**, then refresh, select and receive them on the desktop.
Text shares are UTF-8 files, not clipboard updates. Files sent to the phone remain private until saved
through the companion app's system file picker. Transfers are explicit, cancellable and SHA-256 checked;
files are not overwritten or opened automatically. Limits: 32 files, 1 GiB each, 2 GiB phone storage total.
USB or already-connected wireless ADB is supported on Linux/Windows/macOS; BLE HID requires an explicit
separate ADB target. No notification access is required. See [File sharing](docs/FILE_SHARING.md).

### Multiple devices

The GUI manages the device list on the left and provides three tabs on the right: "Device configuration", "Screen layout", and "Shared service".

1. Before connecting, click "Add device" and enter an independent USB serial number (or BLE identity address), unique screen name, dimensions, and sensitivity for each phone. The ADB discovery list identifies the currently selected device; it does not automatically add every phone to the configuration.
2. In "Screen layout", select the neighboring screen and direction for the current device. A layout such as `computer ↔ android ↔ android-2` is supported, as are phones placed on different sides of the computer. The preview is not scaled to actual resolutions. Duplicate names, edge conflicts, overlapping screens, unknown neighbors, and layouts disconnected from the computer are rejected.
3. Configure the Deskflow executable, computer screen name, address, and port once under "Shared service". Automatic mode starts one Deskflow server; each phone starts an independent bridge process using the same `deskflow-otg` executable, without opening a terminal. With an external service, configure all phone screens and the layout in Deskflow yourself; the GUI does not modify or stop external services.
4. Devices can be connected or disconnected individually, or all at once. Selecting a device on the left only changes the object being viewed and does not interrupt connections. Parameters for connected devices are read-only, while dimensions and connection parameters for disconnected devices remain editable.
5. Disconnect all devices and wait for cleanup before adding or removing a device or changing a screen name, layout, or shared service. Before removing a device referenced by another screen, update those neighboring-screen settings first.
6. Logs can be filtered by device or shared service. Closing the window keeps all connections; "Disconnect all" in the tray ends every session. On exit, input is released from each device before the self-started Deskflow server is stopped. A failed bridge does not stop other devices.

Multiple USB phones, or USB plus **one BLE phone**, are supported. Multiple concurrent BLE phones on one adapter are not supported, and an idle Windows BLE provider continues to reserve its target. The Linux persistent service still follows the existing fixed-identity and lifecycle rules. Multiple devices means multiple simultaneous connections controlled by Deskflow screen switching; it is **not broadcasting the same keyboard and mouse operations to several phones**. Multi-process, layout, and window regression tests use simulated devices and do not constitute real-device acceptance with multiple phones.

### Windows Bluetooth connection (experimental)

Build with both `-DBUILD_GUI=ON -DBUILD_BLE_WIN=ON` to make "Bluetooth Low Energy (Windows, experimental)" available in the GUI's "Connection method" selector. The option is not shown in the default USB build or on macOS.

1. Enter the target phone BLE address reported by Windows and its screen dimensions; this is not the computer adapter address. Use the last address segment in `BluetoothLE#BluetoothLE<computer-address>-<phone-address>`.
2. Configure automatic startup or an existing Deskflow service, click "Connect", and pair/connect the computer from the phone's Bluetooth settings. HID keyboard/mouse control does not require ADB or USB debugging; ADB audio, clipboard or other enhancements require an ADB target for the same phone. The default Bluetooth adapter and absolute pointer are used; Linux isolation and persistent-service controls are not available.
3. The state becomes "Connected" only when the phone identity, pairing, keyboard subscription, pointer subscription, and complete Deskflow handshake are ready. If the phone or Deskflow disconnects in the same process, the GUI waits for reconnection; pairing can be canceled with "Disconnect".
4. "Disconnect" only closes the Deskflow session: unsent actions are discarded, release reports are processed, and the same Windows GATT process and subscriptions are retained. Keep the GUI running and click "Connect" again to reuse the service; the address, screen name, and dimensions can also be updated. Closing the window hides it in the tray and keeps the service. The GATT created by this GUI is stopped only by "Stop idle Bluetooth service" or "Quit". Stop the old service explicitly before switching phones or bridge programs. Unlike Linux, the GUI does not leave an independent persistent service after exit; phone GATT cache issues may still occur after process or system restarts, so arbitrary restarts are not guaranteed to avoid re-pairing.
   Do not use it together with the standalone prototype or a background CLI instance.

This integration does not constitute real-device acceptance of continuous input, reconnection, sleep, or restart behavior. See [Windows BLE usage and limitations](docs/WINDOWS_BLE.md).

### Automatic USB connection on insertion (without USB debugging)

In the GUI, enable "Automatically connect this device when USB is inserted" for the target device (disabled by default), then connect the phone manually once. The program remembers the USB serial number and its configuration only after the state becomes "Connected"; phones whose forms were merely saved by an older version must also complete one successful connection. USB debugging is not required: enter the USB serial number and original screen dimensions directly. If the serial number is unknown, run `deskflow-otg --usb-scan` and inspect the `serial` field of readable devices.

- The program prefers libusb hotplug notifications (backend capability is detected at runtime; Linux/macOS usually support this). If the backend does not support it, startup fails, or the watcher exits unexpectedly, it falls back to polling every two seconds. The current detection method is shown in the log; event mode does not periodically enumerate USB while idle.
- Devices already connected are scanned at startup. After hotplug events, notifications arriving close together are coalesced and a limited number of short retries wait for USB drivers and permissions to become ready. Once a phone is identified, its screen dimensions, sensitivity, pointer mode, Deskflow address/port, screen name, position, and automatic-start setting are restored and it is connected automatically. ADB is never called during this process.
- The GUI must remain running, though it can be hidden in the tray. Remembered phones already connected when the GUI starts are also connected automatically. This is not a boot-time autostart feature; detection stops after the program exits completely.
- After a manual disconnect or failed connection, the same USB insertion is not retried automatically. Click "Connect" manually or unplug and reconnect the USB cable. Multiple remembered phones with automatic connection enabled can connect independently. If a serial number is duplicated in the USB snapshot or GUI configuration, no device using that serial is selected automatically. The shared layout must be configured beforehand.
- With audio or clipboard enabled, the bridge retries a failed scrcpy-server upload within a five-second total budget, allowing ADB to recover after transport startup. This does not restart HID, change the selected ADB target, or restart the ADB server. If upload still fails, the normal failure policy applies; HID-only connections do not use ADB.
- Configuration is saved per phone. When "Follow screen rotation" is enabled together with audio or clipboard, ADB detects rotation during the connection and updates the current phone's Deskflow screen dimensions. Without ADB or without this option, resolution and orientation cannot be updated automatically; disconnect, adjust the dimensions manually, and reconnect after rotation.
- USB permissions, a compatible Windows driver, and a readable USB serial number are prerequisites. If the serial changes with the USB mode, connect manually once with the new serial. An empty serial in `--usb-scan` means that it cannot be read or the device has no serial, and the device is not attempted. In polling fallback mode, a rapid unplug/replug may require another cycle and a short wait if the USB address did not change and the removal was not observed; event mode resets the insertion record on removal notification.

### Deskflow startup and version compatibility

- The actual startup interface is detected from `--help`; compatibility is not restricted by a version allowlist.
- The newer `deskflow-core server --settings` interface (including the 1.26 interface) and the older `deskflow-server --no-daemon --name --config --address` interface are supported. Connection security mode is configured according to the detected arguments.
- Independent temporary settings and screen layouts are generated without overwriting the user's existing Deskflow configuration. An automatically started server listens only on `127.0.0.1`; TLS options are written to the temporary settings, and a port conflict is reported clearly.
- Uncheck "Automatically configure and start local Deskflow" to enter a server address and connect to an existing server. In that case, configure the phone screen name, position, and TLS yourself; the GUI does not stop external services.
- New Deskflow versions can continue to work as long as their protocol, startup arguments, and settings format remain compatible. An unknown startup interface prompts the user to use an existing-service mode.
- Device, dimension, path, and connection parameters are saved automatically. The first automatic startup may require permission for Deskflow to capture keyboard and mouse input; macOS requires Accessibility permission and Wayland may ask the desktop portal for confirmation. Windows USB drivers must satisfy both ADB and libusb access requirements; ADB visibility alone does not prove AOA availability.

### Building the GUI from source

Install Qt 6.8+ and set `CMAKE_PREFIX_PATH` to the Qt installation prefix. Keeping `BUILD_GUI=OFF` (the default) continues to build only the pure-C command-line program.

For Windows GUI builds using `x64-windows-static`, also install `openssl:x64-windows-static-md` for the GUI's certificate generator. Qt uses the dynamic MSVC runtime; the bridge keeps its static-runtime dependencies.

```bash
cmake -S . -B build -DBUILD_GUI=ON -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/gcc_64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

On Linux and Windows, the GUI and bridge program are placed in the same output directory. On macOS, the result is an `.app` and the bridge is copied into `Contents/MacOS`. Windows still requires the vcpkg/libusb toolchain options described in this README; the GUI uses dynamic Qt and the dynamic MSVC runtime, while the bridge uses a static libusb build.

The full build also copies the configured `scrcpy-server` and matching `SCRCPY_VERSION.txt` into the GUI's runtime search path when audio or clipboard is enabled. If `ADB_BUNDLE_DIR` is set, its contents are copied into `platform-tools` beside the GUI. This allows testing directly from the build directory without installing over an existing version. Keep these runtime files with the executable; `gui_runtime_files` checks their presence and consistency without accessing a phone. Missing scrcpy runtime files produce a dedicated error, not an invalid-device-parameters error.

For packaging, download fixed versions and verify their SHA-256 hashes while retaining ADB licenses and required DLLs:

```bash
python scripts/fetch_adb.py --output adb-bundle
cmake -S . -B build -DBUILD_GUI=ON \
    -DADB_BUNDLE_DIR="$PWD/adb-bundle/platform-tools"
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix "$PWD/stage"
python scripts/fetch_licenses.py --output stage/share/deskflow-otg/licenses
```

The resulting directory package can be extracted and run. The Windows entry point is in `stage/bin`; on macOS it is `stage/*.app`. The install prefix must be absolute (required by Qt when generating `qt.conf`); PowerShell can use `--prefix "$pwd/stage"`. Linux also needs system libusb and desktop GUI libraries; a Wayland desktop can run the GUI through XWayland. The macOS app bundle packages the bridge's libusb dependency. See [THIRD_PARTY.md](THIRD_PARTY.md) for third-party notices.

Tests cover device-output parsing, resolution overrides and orientation, layout generation, port conflicts, subprocess connection/rejection/cancellation/exit, preserving external services, and simulated ADB detection in the complete window. Simulated tests do not replace real-device USB validation on the three platforms.

## Building

### Reproducible feature profiles

The default configuration is intentionally a manual/minimal CLI configuration:
`BUILD_GUI`, `BUILD_AUDIO`, and `BUILD_CLIPBOARD` are off, while Linux BLE
follows `BUILD_BLE`'s platform default. These options are compile-time features;
they cannot be added to an already-built executable. Use the fixed local
`build/` directory for normal development. CI or
parallel platform builds may use separate directories, but local builds should
not accumulate `build-*` variants.

For a full build containing every feature supported by the current platform,
use `OTG_PROFILE=full`:

```bash
# Linux: GUI + audio + clipboard + Linux BLE
cmake -S . -B build -DOTG_PROFILE=full -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# macOS: GUI + clipboard (audio and Linux BLE are not supported)
cmake -S . -B build -DOTG_PROFILE=full -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# Windows: GUI + audio + clipboard + experimental Windows BLE
cmake -S . -B build -DOTG_PROFILE=full
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

`build/BUILD_FEATURES.json` and the installed copy record the exact feature set.
For a deliberately small USB-only build, reconfigure the same `build/` directory
and explicitly disable optional features, for example
`-DBUILD_BLE=OFF -DBUILD_GUI=OFF -DBUILD_AUDIO=OFF -DBUILD_CLIPBOARD=OFF`.

```bash
git clone https://your-host/deskflow-otg.git
cd deskflow-otg
cmake -S . -B build -DBUILD_GUI=OFF -DBUILD_AUDIO=OFF \
    -DBUILD_CLIPBOARD=OFF -DBUILD_BLE=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
# Output: build/deskflow-otg
```

The macOS build command is the same. By default it searches for libusb under `/opt/homebrew` on Apple Silicon and `/usr/local` on Intel. For a custom Homebrew path, or when libusb is not found:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix libusb)"
cmake --build build
```

The compiler and libusb must use the same architecture; do not mix an Intel Homebrew installation under Rosetta with a native Apple Silicon compiler. Run `ctest --test-dir build --output-on-failure` to verify the build and socket regression tests. CI covers Linux, Intel macOS, Apple Silicon macOS, and Windows, but does not test USB hardware with a phone.

### Publishing prebuilt packages

#### Unified version management

The CLI and GUI share the `VERSION` file at the repository root (currently `0.1.7`) and do not maintain separate versions. `deskflow-otg --version`, `deskflow-otg-gui --version`, and the GUI's "About" dialog show the version and Git commit. Development builds use `0.1.7+git.<commit>`, append `.dirty` when tracked files have uncommitted changes, and use `unknown` when Git information is unavailable. Version information is generated during CMake configuration; rerun `cmake -S . -B ...` after changing the commit or source.

For a release, modify and commit `VERSION` first, then create a matching `vX.Y.Z` tag. CI uses `-DOTG_RELEASE_TAG=vX.Y.Z` to verify that the tag matches the version file and that tracked files are clean; release builds use the plain `X.Y.Z` version. Local release builds must pass the same option, while development builds leave it empty. `share/deskflow-otg/BUILD_VERSION.txt` in the install directory records the actual build version. DEB and Windows packaging scripts read it by default, and `--version` is used only for consistency checks (manual specification is allowed when an old install has no such file). This prevents a new package version from being applied to an old binary.

After pushing a version tag beginning with `v`, GitHub Actions builds, tests, and packages all four platforms. When everything succeeds, it automatically creates the corresponding [Release](https://github.com/gh6497/deskflow-otg/releases) and places Linux x86_64, macOS arm64 / x86_64, and Windows x86_64 packages in **Assets**. For example:

```bash
git tag v0.1.0
git push origin v0.1.0
```

Commits on ordinary branches and pull requests build the CLI, GUI, and tests and check desktop-package deployment, but do not create a Release. A tag release provides both `deskflow-otg-*` command-line packages and `deskflow-otg-gui-*` desktop packages. Linux CLI packages require the libusb-1.0 runtime; macOS CLI archives require `brew install libusb` first. Windows packages use statically linked libusb.

Windows GUI releases also provide `deskflow-otg-gui-windows-x86_64-setup.exe`. It installs per-user under `%LOCALAPPDATA%\Programs\Deskflow OTG`, can be started from the Start menu or removed from "Installed apps", and does not require administrator privileges; the portable ZIP remains available. The installation includes the bridge, Qt, ADB, and dependency licenses, but not Deskflow. Configure the Windows USB driver yourself as described below. The installer is not signed, so Windows may show an unknown-publisher warning; verify the download source. During upgrades, the installer uninstalls the old version first. Exit the GUI before installing or uninstalling. To create an installer locally, build the GUI, run `cmake --install`, collect licenses on Windows, and install NSIS (`makensis` must be in `PATH`):

```powershell
python scripts/package_windows.py --stage stage --output dist/deskflow-otg-gui-windows-x86_64-setup.exe
```

macOS GUI releases also provide `deskflow-otg-gui-macos-arm64.dmg` and `deskflow-otg-gui-macos-x86_64.dmg`. Choose the image matching the Mac architecture, open it, and drag `deskflow-otg-gui.app` to `Applications`. The app contains the bridge, Qt, libusb, ADB, and dependency licenses, but not Deskflow. Homebrew libusb is not needed for the GUI (the standalone CLI archive still needs it). The image is currently unsigned and not notarized; macOS Gatekeeper may block it on first launch. After verifying the source, choose "Open Anyway" under "System Settings → Privacy & Security". Create an image locally with:

```bash
python3 scripts/package_dmg.py --stage stage \
    --output dist/deskflow-otg-gui-macos-arm64.dmg
```

Build with `BUILD_GUI=ON`, install to `stage`, download ADB, and collect licenses as described above. The filename architecture must match the build machine. The image provides drag-and-drop installation only; it does not modify system settings or install Deskflow automatically.

Linux GUI releases also provide an amd64 `.deb` containing the GUI, its required bridge program, and Qt/ADB from the release directory. The CLI command is not packaged or installed separately. After installation, start it from the application menu or run `deskflow-otg-gui`; Deskflow must still be installed separately, and USB access still requires root or a udev rule. Install a downloaded release package with:

```bash
sudo apt install ./deskflow-otg-gui-<version>-linux-amd64.deb
```

You can also create a DEB from a local GUI install directory. First download ADB, build with `BUILD_GUI=ON`, and collect licenses as described above; `stage` must be the GUI install directory:

```bash
cmake --install build --prefix "$PWD/stage"
python3 scripts/package_deb.py --stage stage \
    --output dist/deskflow-otg-gui-0.1.0-$(dpkg --print-architecture).deb
```

The DEB installs to `/opt/deskflow-otg` and provides a GUI entry point in `/usr/bin`; do not mix it with a version manually installed at the same path. GUI packaging requires desktop runtime libraries; Deskflow is not included.

Linux GUI releases also provide the installation-free `deskflow-otg-gui-linux-x86_64.AppImage`, containing the bridge, Qt, libusb, and ADB (but not Deskflow). Run it with:

```bash
chmod +x deskflow-otg-gui-linux-x86_64.AppImage
./deskflow-otg-gui-linux-x86_64.AppImage
```

The system needs a graphical desktop and FUSE; when FUSE is unavailable, add `--appimage-extract-and-run`. USB permission rules are the same as for the DEB. To create an AppImage from a local GUI install directory, first download and verify `appimagetool-x86_64.AppImage` and `runtime-x86_64` (CI pins their SHA-256 hashes), then run:

```bash
python3 scripts/package_appimage.py --stage stage \
    --appimagetool appimagetool-x86_64.AppImage \
    --runtime runtime-x86_64 \
    --output dist/deskflow-otg-gui-linux-x86_64.AppImage
```

Windows (PowerShell, using the Visual Studio x64 toolchain; `VCPKG_ROOT` points to the vcpkg directory):

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/Release/deskflow-otg.exe --help
```

## Usage

### 1. Configure the Deskflow server

On the Deskflow host, configure the "phone" as a screen whose name matches `--name`, and place it next to the host screen. For example:

- Screen name: `android`
- Position: **right** of the host screen

### 2. Connect the phone and run the program

Connect the phone to the host with a USB cable, then run:

```bash
sudo ./build/deskflow-otg \
    --host 127.0.0.1 \
    --port 24800 \
    --name android \
    --width 1080 --height 1920
```

> **Linux:** Root or a udev rule is required to access USB devices. To avoid `sudo`, configure a udev rule as described below. **macOS:** Run without `sudo` first; macOS does not use udev, see the notes below.

**Windows:** In PowerShell, run `./build/Release/deskflow-otg.exe --host 127.0.0.1 --name android --width 1080 --height 1920 -s <phone USB serial>`. Install a USB driver usable by libusb (for example, bind the phone's USB interface to WinUSB with Zadig). Replacing the driver may affect MTP/ADB, so verify the selected device/interface and back up the original driver. If the phone is not recognized, first see [Windows USB OTG device recognition](docs/WINDOWS_USB.md). Configure the fingerprint and certificate as described above when TLS is required.

### macOS operation and permissions

```bash
./build/deskflow-otg --host 127.0.0.1 --name android \
    --width 1080 --height 1920 -s <phone USB serial>
```

- In "System Settings → Privacy & Security", grant **Deskflow** Accessibility permission. If macOS requests Input Monitoring permission, grant that too. These permissions let Deskflow capture the host keyboard and mouse; this program only receives TCP events.
- Use a USB cable that supports data transfer. If macOS asks whether to allow an accessory, unlock the Mac and choose Allow. USB debugging on the phone is not required.
- Other USB devices connected to the Mac may also have serial numbers, so specifying the phone with `-s` is recommended. Without it, the program lists candidates and does not select among multiple candidates automatically.
- If opening the USB device fails or reports `ACCESS/BUSY`, close other programs using the phone (such as scrcpy or Android file-transfer tools), unplug and reconnect, and try again. `sudo` does not fix every device-lock or macOS accessory-authorization problem; do not disable SIP or remove system drivers.
- The macOS build and runtime paths are supported, but AOA HID, unplug detection, and absolute-pointer compatibility with specific phones still require real-device validation.

### 3. Control the phone through Linux BLE

See the [Linux BLE handover document](docs/LINUX_BLE_HANDOVER.md) for development handover information, validated scope, and the order of the next real-device checks.

Linux can advertise the computer as a BLE HID peripheral and connect to it from the phone as an HID host. This mode does not depend on USB, but the phone must pair and subscribe to both the keyboard and pointer HID input characteristics; a Bluetooth connection or successful Deskflow handshake alone does not mean the input channel is ready. While running, the program temporarily sets the adapter name to "Deskflow OTG" and restores the original name on exit. It does not automatically pair, trust, or remove pairings. The adapter mode is changed temporarily only with explicit `--ble-isolate`; ordinary `--transport ble` does not switch it.

```bash
./build/deskflow-otg --transport ble --adapter hci0 --ble-peer AA:BB:CC:DD:EE:FF \
    --host 127.0.0.1 --port 24800 --name android \
    --width 1080 --height 2340
```

Connect to the advertised "Deskflow OTG" in the phone's Bluetooth settings and confirm pairing as prompted before moving the mouse onto the phone screen. `--ble-peer` must specify the target phone's identity address in BlueZ (find it with `bluetoothctl devices`; it is not the rotating advertising address). Input is allowed only when the target is paired and it is the adapter's only connection. Keyboard and mouse Value signals are sent only to BlueZ and are not broadcast on the system D-Bus. If there are multiple connections, input is disabled rather than guessing the target.
This is not an isolation guarantee for arbitrary multi-client environments; a dedicated adapter is recommended. Dual-mode classic HIDP compatibility and complete real-device regression testing are still pending. Pure pointer-movement reports are sent every 15 ms by default. When the isolation launcher below obtains the actual connection interval, the send period follows it (for example, 7.5/12.5/15 ms); keyboard and wheel reports are not throttled this way.
If the link is disconnected, delivery of release reports on the already-disconnected link is not guaranteed.
The BLE event pump also runs during continuous Deskflow input and incomplete packet reads, with a maximum 1 ms wait while input is active. It does not depend on the old 500 ms USB idle check to send the last coordinate. Input IPC for the persistent session is non-blocking with at most one request in flight; consecutive movement/drag coordinates are merged before sending while keyboard, button, wheel, and range changes retain their order. Status is queried on demand and the service wakes immediately when its socket is ready. An oversized input queue or unresponsive service ends the session and releases input.

#### Native BLE-only launch and automatic recovery (without Python)

When the phone treats the dual-mode computer as an audio device, native `--ble-isolate` can be used explicitly. It requires sudo, BlueZ, and util-linux's `/usr/sbin/runuser`; it does not depend on Python, a standalone validation directory, or test scripts.
At startup, all Bluetooth connections on the adapter are temporarily disconnected, the state is saved, and BR/EDR is disabled. The bridge subprocess is demoted with `runuser` and continues to run as the ordinary user who invoked sudo; the root parent manages only isolation and recovery for this adapter. Stop any old bridge and wait for recovery before starting, and never run multiple isolation entry points at once.

```bash
sudo ./build/deskflow-otg --transport ble --ble-isolate --adapter hci0 --ble-peer AA:BB:CC:DD:EE:FF \
  --host 127.0.0.1 --port 24800 --name android --width 1080 --height 2340
```

After receiving an HID-ready notification through dedicated IPC, the root launcher requests 7.5–15 ms and `latency=0` only for the one observed LE connection on the adapter that is encrypted and has the computer as peripheral. It does not accept a connection handle or arbitrary HCI command from the child process, and it does not guess the target when there are multiple connections. HCI monitoring does not accept arbitrary handles from the session; the input side is separately bound to the phone identity by `--ble-peer`.

Only a log entry such as `BLE actual interval=... (controller event)` represents an observed actual parameter. `requesting interval` means only that a request was sent. If the phone rejects it or no confirmation arrives within five seconds, the connection and observed parameters are retained; the program does not retry in a loop, modify permanent Bluetooth settings, install a service, or grant HCI privileges to the bridge.
The bridge can still connect when run directly without `--ble-isolate`, but it will not automatically request low-latency parameters.

**Do not remove pairing or stop and restart isolation for ordinary reconnection.** When the phone temporarily disconnects, the program keeps the GATT service, advertisement, and existing pairing. It closes the Deskflow TCP connection so the computer regains the cursor, discards old keys and coordinates, and waits for the original phone to reconnect. If BlueZ retains CCC subscriptions for the paired device, the phone does not need to trigger `StartNotify` again. After reconnection and confirmation that the actual link and subscriptions are ready, the program reconnects to Deskflow automatically.
The system agent still confirms the first pairing; the program does not automatically accept an unknown device. The GUI isolation option requires confirmation of its scope followed by system pkexec authentication and does not save the administrator password. USB automatic connection and tray behavior are unchanged.

The GATT service uses fixed handles `0x0100..0x0111`, includes keyboard LED output and the HID Control Point, and supports offset reads of the Report Map to avoid handle drift on every registration. A range conflict is reported instead of silently selecting new handles. Different GATT layouts from older prototypes, phone caches, or inconsistent keys on the two sides still require separate diagnosis; these code fixes must not be treated as completed real-device re-pairing-free acceptance.

On complete exit, the native launcher verifies and restores the original BR/EDR, LE, power, security, discoverable/pairable, and name settings. The snapshot is removed only after successful recovery. An abnormal exit or failed recovery leaves the root-only `/run/deskflow-otg-ble-hci0.state`; a later start refuses to overwrite it. Restore with the same binary:

```bash
sudo ./build/deskflow-otg --ble-restore --adapter hci0
```

The snapshot is bound to the adapter address. If the controller still fails after power-on retries, the program reports the failure clearly instead of claiming success; it does not restart bluetoothd or delete Bluetooth data. SIGKILL and power loss cannot be cleaned up automatically, and the `/run` snapshot does not survive reboot. This command cannot read JSON snapshots from the old Python experiment; do not run the two mechanisms together.

#### Persistent GATT and restartable bridge sessions (recommended)

Android may cache HID descriptors and not rewrite CCC on reconnection; BlueZ removes the service's CCC when the application unregisters. Therefore fixed handles alone do not guarantee re-pairing-free operation after the CLI exits. The following service/session separation is recommended:

1. End the old BLE bridge normally and confirm recovery succeeded with no `.state` file left behind. Stop testing if an MGMT/HCI timeout occurs.
2. In a terminal as a normal user (**without sudo**), run the persistent service and keep the terminal open:

   ```bash
   ./build/deskflow-otg --ble-service --adapter hci0
   ```

3. In another terminal, start the session. The native root supervisor still performs isolation and restores the mode when the session exits:

   ```bash
    sudo ./build/deskflow-otg --transport ble --ble-session --ble-isolate --adapter hci0 \
      --ble-peer AA:BB:CC:DD:EE:FF --host 127.0.0.1 --port 24800 --name android --width 1080 --height 2340
   ```

   If the system is already configured for BLE, remove `sudo` and `--ble-isolate`; the adapter mode is not changed.

The session connects to the service through the private Unix seqpacket socket `/run/user/UID/deskflow-otg/ble-hci0.sock`. The directory is mode 0700 and the socket is mode 0600; both UIDs, the protocol version, and packet lengths are checked. Only one active session is allowed per adapter and user.
`--ble-service` runs in the foreground as a normal user. The first session registers GATT and later sessions reuse it. The same HID descriptor must be used; only the absolute pointer is currently supported, while dimensions may be changed per session.

- When the session disconnects or the bridge crashes/receives EOF, keys are released, pointer range ends, pending coordinates are discarded, advertising stops, and the name is restored. Five seconds without session IPC also releases input. The root supervisor checks that service cleanup is complete before attempting to restore the original mode; if it cannot confirm cleanup, it does not continue switching modes or claim recovery and keeps the snapshot for explicit recovery after the session is stopped.
- Ending a session **does not unregister GATT**. The service retains the real StartNotify state and still requires the original phone to be online and paired, with both channels subscribed and not suspended, before it is ready. A phone merely being connected is not presented as successful subscription.
- Stopping the persistent service unregisters GATT and loses this CCC continuity. Recovery after a service crash or BlueZ/host restart is still unresolved. Damaged or lost CCC state from the old prototype or Android cache cannot be recreated; if there is still no subscription, capture packets for diagnosis rather than deleting the pairing to claim acceptance.
- The local AX210/BlueZ 5.72 + Redmi K20 Pro (Android 14) setup has validated native isolation, normal bridge restart, two phone disconnect/reconnect cycles, automatic recovery after a bridge subprocess SIGKILL, and reconnection without re-pairing. The controller confirmed an actual connection interval of 12.5–15 ms. Migrating the initial old cache still once required toggling the phone's Bluetooth.
  This is an experimental lifecycle fix, not a fix for AX210 HCI Reset timeouts. It does not represent acceptance of complete input, long-running operation, persistent-service/BlueZ/host restart, or other devices, and does not replace the existing recovery-snapshot mechanism.

#### GUI and packages

1. In a Linux BLE GUI build, choose "Bluetooth Low Energy" under "Connection method" and enter an adapter name such as `hci0`, the phone identity address, and the screen dimensions. ADB is not required; USB automatic connection is paused in Bluetooth mode.
2. Click connect. The GUI checks for a same-user service, reuses an idle one, or starts the same program in `--ble-service` mode in the background when none exists. It reports an error if another session owns it or if the protocol is incompatible, and does not terminate the old process. Complete first pairing/connection in the system Bluetooth settings.
3. When the phone is treated as an audio device, explicitly select "Temporarily use BLE-only mode". Confirm that all connections on the adapter will be disconnected before authenticating as administrator. `/usr/bin/pkexec`, a desktop authentication agent, and a wired keyboard/mouse or dedicated adapter are required.
4. After the phone disconnects, the GUI displays a reconnection wait state without ending the session or deleting pairing. Manual disconnect ends the session and waits for the supervisor to restore the mode. The GUI does not force-kill a privileged process responsible for recovery; see the log for the failure reason.
5. Exiting the GUI retains the normal-user persistent service (but it does not continue advertising or sending input). To change the target phone, update the service binary, or stop it completely, end the session first, then click "Stop idle Bluetooth service" and confirm that subscription continuity will be lost. The CLI equivalents are:

   ```bash
   deskflow-otg --ble-status --adapter hci0
   deskflow-otg --ble-stop-service --adapter hci0
   ```

The service remains bound to its target throughout its lifecycle; another session cannot silently switch phones. Configuration is stored in the GUI's QSettings. Automatic background-service logs are stored as `ble-hci0.log` in Qt's AppLocalDataLocation (usually `~/.local/share/deskflow-otg/deskflow-otg-gui/`, depending on the application identifier). When migrating from the old experimental service protocol, explicitly end the old session/service first; the GUI does not silently replace a running service.

DEB packages automatically include a CLI shortcut and the optional `deskflow-otg-ble@.service` user unit. Based on build metadata, BLE packages declare BlueZ, GLib, and pkexec dependencies. Packages **do not automatically start or enable** the service and do not change system Bluetooth settings. To manage it with user systemd, explicitly run `systemctl --user start deskflow-otg-ble@hci0` when no persistent service already exists; the GUI reuses that service and no root persistent daemon is required.
AppImage and extracted distributions still depend on host BlueZ, GLib, and the desktop authentication agent. A FUSE-mounted AppImage may not permit root execution; when BLE-only isolation is required, prefer a DEB or extract to a fixed directory rather than promising privilege escalation compatibility from a mounted AppImage.

This CLI/GUI integration and packaging regression does not constitute additional hardware acceptance. Subscription recovery after service/BlueZ/system restart, complete release of held keys on failure, lock-screen behavior, and long-term stability still require real-device confirmation.

When an input device is shown but there is no cursor, inspect `BLE waiting:` in the persistent-service log. Identity matching uses the `Address` of the online BlueZ Device1 and does not require its object path to match the identity MAC; the online object must still be paired, and the pairing flag from another offline object cannot be used. `waiting for both HID subscriptions` means that the phone has not made the real subscriptions, not that Deskflow is disconnected. Keep the pairing and check the phone's input-device switch or reconnect. Android cache recovery after a service restart may still require toggling the phone's Bluetooth, so arbitrary restarts are not guaranteed to be manual-operation-free. Once ready, also confirm that the GUI server address and port match the actual Deskflow listener.

### 4. Control the phone

Move the mouse across the edge of the host screen toward the "phone" screen; the keyboard and mouse are then injected into the phone. Move the mouse back across the host edge to return to the computer.

The default is `--mouse-mode absolute`: movement hovers, holding the left button produces continuous button-down reports for dragging, and leaving the phone screen releases buttons. For natural movement, set `--width/--height` to the phone's current-orientation resolution. Absolute mode maps the complete virtual screen to the phone display, so a dimension mismatch does not accumulate relative-movement error.

> **To exit:** When the mouse is on the "phone" screen, keyboard input, including Ctrl+C, is forwarded to the phone instead of the terminal. Move the mouse back to the computer first, then press Ctrl+C; alternatively unplug the phone. The program detects USB removal, exits automatically, and returns the mouse to the computer.

## Command-line options

```
  -H, --host HOST       Deskflow server address (default: 127.0.0.1)
  -p, --port PORT       Deskflow server port (default: 24800)
  -n, --name NAME       Screen name reported to the server (default: android)
  -s, --serial SERIAL   Android device USB serial (auto-detect if omitted)
      --transport MODE usb (default) or ble (Linux)
      --adapter NAME    Bluetooth adapter (default: hci0)
      --ble-isolate     Native sudo BLE-only launch; restore on exit, no Python
      --ble-restore     Restore the adapter from a native snapshot; do not bridge
      --ble-peer MAC    Target phone BlueZ identity address (required for BLE bridge)
      --ble-service     Persistent foreground GATT service as a normal user
      --ble-session     Run a bridge session through the persistent service (absolute only)
      --ble-status      Query service JSON status without acquiring a session
      --ble-stop-service Stop an idle service; subscription continuity is lost
      --width W         Virtual screen width, 1..32767 (default: 1080)
      --height H        Virtual screen height, 1..32767 (default: 1920)
      --mouse-mode MODE absolute (default) or relative (compatibility mode)
      --gui              GUI subprocess mode: JSON status on stdout; stdin byte/EOF requests exit
      --usb-scan         Output a USB device JSON snapshot; no ADB or HID registration
      --usb-watch        Output USB hotplug JSON events; stdin byte/EOF stops; exit code 3 if unsupported
  -v, --version         Software version and Git commit
  -h, --help            Help
```

### Low mouse sensitivity and tiring vertical movement

**GUI:** After confirming the "original screen size", set the "sensitivity multiplier" to `1.5×` or `2×`. The virtual width and height are calculated as **original dimensions ÷ multiplier**, rounded to the nearest integer with a minimum of 1. The calculation always starts from the original dimensions, so repeated adjustments do not compound scaling; `1×` restores the original size. Changing the original dimensions manually, swapping width and height, or detecting dimensions again through ADB reapplies the current multiplier. Original dimensions and the multiplier are saved separately, and the new setting takes effect after reconnecting.
The multiplier applies only to absolute coordinates. Relative mode uses the original dimensions and retains the multiplier for a later switch back to absolute mode.

If the pointer can reach the full screen but moving from top to bottom requires a large wrist movement, this may not be stutter or latency; the virtual screen may simply be large and therefore have low sensitivity. Absolute mode maps virtual coordinates to the complete phone screen, so `--width/--height` do not have to match the phone's real resolution: **proportionally reducing the virtual dimensions increases movement sensitivity while still covering the full phone screen**.

For example, with a phone resolution of `1080 × 2340`, first try `720 × 1560`, which gives approximately 1.5× the original movement sensitivity. If it is still tiring, try `540 × 1170`, approximately 2×:

```bash
sudo ./build/deskflow-otg --host 127.0.0.1 --port 24800 --name android \
    --width 720 --height 1560 -s <phone USB serial>
```

Adjust width and height proportionally to avoid an imbalance between horizontal and vertical movement. Before changing parameters, move the mouse back to the computer, press Ctrl+C to stop the old process, and start it again. This adjustment targets the default absolute mode and does not require switching to the relative mode affected by Android mouse acceleration.

### The mouse reaches an invisible boundary too early

The old version used the difference between Deskflow absolute coordinates directly as relative HID movement. Android changes the actual distance according to mouse speed and acceleration, so the server could believe the pointer had reached the boundary while the phone pointer was still in the middle. Resetting at entry, splitting large movements, or changing the resolution alone cannot guarantee removal of this error.

The new default absolute-coordinate mode uses a digitizer's `Tip Switch + In Range + ABS X/Y`; every report contains both the current position and button state and does not depend on the previous pointer position. It still uses AOA and does not require adb. Android identifies it as a digitizer/pen with mouse buttons, so some applications may respond differently to a pen and a mouse.
If a device does not support this mode, use the original relative mouse with `--mouse-mode relative`; it remains affected by Android acceleration and cannot guarantee that the pointer boundary matches Deskflow's boundary.

## Rootless USB access on Linux (udev rule)

`/etc/udev/rules.d/51-android.rules`:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", MODE="0666", GROUP="plugdev"
```

(`18d1` is Google's USB vendor ID; add the corresponding vendor IDs for Android devices from other manufacturers.) Then run `sudo udevadm control --reload-rules && sudo udevadm trigger`.

## Directory structure

```
src/
  main.c              Entry point, argument parsing, callback wiring
  deskflow_client.c   Minimal Deskflow client protocol (TCP handshake + message loop)
  aoa_hid.c           USB AOA HID (libusb enumeration/open/register/report sending)
  usb_scan.c          USB device snapshots and libusb hotplug event monitoring
  bridge.c            Keyboard/mouse state machine + HID report generation (scrcpy hid_*)
  keymap.c            deskflow KeyID → USB HID usage mapping
platform/
  posix/
    scrcpy_session.c  ADB/scrcpy-server/forward and audio/control socket lifecycle
  linux/
    audio_player.c    scrcpy RAW PCM audio frames and ALSA playback
    ble_hid.c         Linux GATT/HID, stable handles, and paired-device reconnection
    ble_supervisor.c  Native isolation, demoted subprocess, mode snapshot, and recovery
    ble_link.c        HCI parameter monitoring and low-latency requests by the root parent
    ble_session.c     Persistent GATT service and local bridge sessions
  windows/
    scrcpy_session.c  Windows ADB/scrcpy-server/forward and socket lifecycle
    audio_player.c    scrcpy RAW PCM audio frames and WASAPI playback
    ble_win.cpp       Experimental native Windows BLE HID backend
    ble_win.h         C interface for the Windows BLE backend
    ble_win_control.c/.h Windows GUI BLE session control parser
gui/
  main_window.cpp     Qt Widgets UI, automatic ADB detection, and settings persistence
  session.cpp         Deskflow configuration/startup, bridge state, and process lifecycle
  device_info.cpp     Device/dimension/orientation parsing and screen-layout generation
  process_utils.cpp   Asynchronous command execution, timeouts, and executable discovery
  usb_auto_connect.cpp USB hotplug notifications, polling fallback, and remembered-phone auto-connect policy
scripts/
  fetch_adb.py        Fixed-version ADB download and SHA-256 verification
  fetch_scrcpy_server.py Fixed-version scrcpy-server download, caching, and SHA-256 verification
  fetch_licenses.py   Dependency-license collection for release packages
third_party/
  scrcpy/scrcpy-server-v5.0.1  Android Server for audio (Apache-2.0)
```

## Relationship to scrcpy / Deskflow (source attribution)

| Repository file | Corresponding source |
|---|---|
| `src/aoa_hid.c` | scrcpy `app/src/usb/aoa_hid.c` + `app/src/usb/usb.c` |
| `src/bridge.c` (keyboard, relative mouse) | scrcpy `app/src/hid/hid_keyboard.c` + `app/src/hid/hid_mouse.c` |
| `src/bridge.c` (absolute pointer) | HID Digitizer specification; Android `TouchInputMapper::dispatchPointerStylus` |
| `src/keymap.c` (KeyID constants) | Deskflow `src/lib/deskflow/KeyTypes.h` |
| `src/deskflow_client.c` (protocol format) | Deskflow `src/lib/deskflow/ProtocolTypes.h` |

The relevant scrcpy code is licensed under Apache-2.0, and this repository is also licensed under Apache-2.0 (see `LICENSE`).

## TLS

**Status: ✅ Completed.** TLS client support, server SHA-256 fingerprint verification, and GUI client-certificate generation are implemented. The Linux GUI build and all 20 tests passed; on 2026-11-03, the user confirmed the client-certificate workflow in the updated Linux DEB works.

The bridge supports native TLS from Deskflow 1.26 onward. TLS uses a Deskflow-compatible OpenSSL client implementation; protocol handshake and keyboard/mouse/clipboard data still use the existing Deskflow frame format. Because Deskflow normally uses self-signed certificates and a SHA-256 fingerprint database, a trusted server fingerprint must be provided when TLS is enabled:

```bash
sudo ./build/deskflow-otg --host 127.0.0.1 --port 24800 --name android \
    --width 1080 --height 1920 -s <phone serial> --tls \
    --tls-fingerprint 001122...aabbccdd
```

The Deskflow fingerprint database (`v2:sha256:...`) can also be used directly:

```bash
./build/deskflow-otg ... --tls --tls-fingerprint-file ~/.config/deskflow/tls/trustedServers.txt
```

If the server enables client-certificate authentication, add `--tls-cert <PEM file>`; the PEM must contain both the client certificate and private key. Choose exactly one of `--tls-fingerprint` and `--tls-fingerprint-file`. The GUI connection settings provide the same options and save them per device.

In the GUI's TLS section, click **Generate client certificate** to create a self-signed RSA certificate and private key (2048 or 4096 bits) and fill the client PEM path. This works with an external Deskflow server too. If the path is empty, the file is saved as `tls/client.pem` under the application's configuration directory; an existing file requires confirmation before replacement. Trust the displayed **client** SHA-256 fingerprint in Deskflow when connecting. The trusted **server** fingerprint and fingerprint database are not changed by client certificate generation.

## Current limitations

- Keyboard mapping uses the **US layout**; some symbol keys may be wrong with other layouts.
- Clipboard support is optional ADB text sharing only; image/file clipboards, file drag-and-drop, and screenshots are not supported. Explicit image/file transfer is available separately through the [companion app](docs/FILE_SHARING.md).
- Only keyboard and mouse are supported; gamepads (AOA gamepad) are not implemented.
- The default absolute pointer depends on Android HID digitizer support; compatibility with specific devices requires real-device validation.
- `--mouse-mode relative` is a compatibility mode; Android mouse acceleration may cause drift and early boundary contact.
- USB access must be available first (root or a udev rule on Linux, USB accessory permission on macOS, and a libusb-compatible driver on Windows).
