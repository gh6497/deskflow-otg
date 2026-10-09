#!/usr/bin/env python3
"""Turn an installed Linux GUI distribution into an AppImage."""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=pathlib.Path, required=True,
                        help="absolute-prefix directory produced by cmake --install with BUILD_GUI=ON")
    parser.add_argument("--appimagetool", type=pathlib.Path, required=True,
                        help="path to appimagetool-x86_64.AppImage")
    parser.add_argument("--runtime", type=pathlib.Path, required=True,
                        help="path to a verified runtime-x86_64 binary")
    parser.add_argument("--output", type=pathlib.Path, required=True, help="output .AppImage path")
    args = parser.parse_args()

    stage = args.stage.resolve()
    gui = stage / "bin/deskflow-otg-gui"
    bridge = stage / "bin/deskflow-otg"
    if not gui.is_file() or not bridge.is_file():
        parser.error("--stage must contain bin/deskflow-otg-gui and bin/deskflow-otg "
                     "(build with BUILD_GUI=ON and run cmake --install first)")
    tool = args.appimagetool.resolve()
    if not tool.is_file():
        parser.error("--appimagetool must point to an existing AppImage")
    runtime = args.runtime.resolve()
    if not runtime.is_file():
        parser.error("--runtime must point to an existing runtime-x86_64 binary")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="deskflow-otg-appimage-") as temp:
        appdir = pathlib.Path(temp) / "DeskflowOTG.AppDir"
        shutil.copytree(stage, appdir / "usr", symlinks=True)

        # The portable Qt installation is in usr/lib. The bridge also needs
        # libusb even on machines where the distribution package isn't installed.
        libusb = subprocess.check_output(["ldd", str(bridge)], text=True)
        match = re.search(r"^\s*libusb-1\.0\.so\.0\s+=>\s+(/\S+)", libusb, re.MULTILINE)
        if not match:
            parser.error("could not find the bridge's libusb-1.0.so.0 dependency")
        bundled_libusb = appdir / "usr/lib/libusb-1.0.so.0"
        if not bundled_libusb.exists():
            bundled_libusb.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(pathlib.Path(match.group(1)).resolve(), bundled_libusb)

        apprun = appdir / "AppRun"
        apprun.write_text(
            '#!/bin/sh\n'
            'HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
            'export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
            'exec "$HERE/usr/bin/deskflow-otg-gui" "$@"\n', encoding="utf-8")
        apprun.chmod(0o755)
        (appdir / "deskflow-otg.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=Deskflow OTG\n"
            "Comment=Control an Android device with Deskflow via USB\n"
            "Exec=deskflow-otg-gui\nIcon=deskflow-otg\n"
            "Terminal=false\nCategories=Utility;\n", encoding="utf-8")
        icon = pathlib.Path(__file__).resolve().parent.parent / "packaging/deskflow-otg.svg"
        shutil.copyfile(icon, appdir / icon.name)
        (appdir / ".DirIcon").symlink_to(icon.name)

        env = dict(os.environ, ARCH="x86_64", APPIMAGE_EXTRACT_AND_RUN="1")
        subprocess.run([str(tool), "--runtime-file", str(runtime), str(appdir), str(output)],
                       env=env, check=True)
    print(output)


if __name__ == "__main__":
    main()
