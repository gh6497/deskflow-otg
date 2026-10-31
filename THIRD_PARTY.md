# Desktop distribution dependencies

The desktop package includes the deskflow-otg bridge (Apache-2.0), dynamically
linked Qt runtime libraries/plugins, and Android Debug Bridge. **Deskflow itself
is not distributed**; the application runs the user's separately installed copy.

## Qt 6.8.3

Qt Core, Gui, Widgets and Network, and the runtime plugins deployed by Qt's
deployment tools, are provided by The Qt Company and their respective authors.
The application dynamically links Qt under the LGPLv3 option. Users may replace
the shared Qt libraries with compatible modified builds and debug those changes.
The application source and CMake build instructions are in this repository.

- Source and bundled third-party notices:
  <https://download.qt.io/archive/qt/6.8/6.8.3/single/>
- Qt license and attribution documentation:
  <https://doc.qt.io/qt-6.8/licenses-used-in-qt.html>
- LGPLv3: <https://www.gnu.org/licenses/lgpl-3.0.html>
- GPLv3 (incorporated by LGPLv3): <https://www.gnu.org/licenses/gpl-3.0.html>

Release staging includes these license texts in `share/deskflow-otg/licenses`.

## Android Debug Bridge 36.0.2

ADB and, on Windows, its accompanying DLLs are extracted from Google's
platform-tools archives. `platform-tools/NOTICE.txt` and `source.properties`
are retained beside the executable. These notices describe the component
licenses, including Apache-2.0 and BSD-style licenses.

- Sources: <https://android.googlesource.com/platform/packages/modules/adb/>
- Binary distribution: <https://developer.android.com/tools/releases/platform-tools>
- Pinned archive URLs and SHA-256 hashes: `scripts/fetch_adb.py`.

## scrcpy Android server 5.0.1

Audio-enabled builds bundle the official `scrcpy-server` from the scrcpy 5.0.1
release. It is used on Android through ADB and is licensed under Apache-2.0.

- Release file: <https://github.com/Genymobile/scrcpy/releases/download/v5.0.1/scrcpy-server-v5.0.1>
- SHA-256: `764eb6f79811d5211fe9df341120882ba9994c7a61b897d7bf3fb662e53bc536`
- Update helper: `scripts/fetch_scrcpy_server.py`.

## libusb

The bridge uses libusb (LGPL-2.1-or-later). Windows CLI builds statically link
the vcpkg package; macOS GUI packaging deploys its shared library. The bridge
source and build recipe allow relinking against a modified libusb.

- Sources and notices: <https://github.com/libusb/libusb>
- License: <https://github.com/libusb/libusb/blob/master/COPYING>

## Linux Bluetooth runtime

Linux BLE builds dynamically link GLib/GIO (LGPL-2.1-or-later). BlueZ and
util-linux runuser are system dependencies, not vendored implementations.
Desktop administrator authentication uses the system's pkexec/Polkit agent;
no privileged helper with persistent authorization is installed.

- GLib source and license: <https://gitlab.gnome.org/GNOME/glib>
- GLib license text is included by `scripts/fetch_licenses.py` for release staging.
- BlueZ: <https://www.bluez.org/>
- util-linux: <https://github.com/util-linux/util-linux>
