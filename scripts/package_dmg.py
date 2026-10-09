#!/usr/bin/env python3
"""Create a drag-to-Applications macOS disk image from a GUI install stage."""

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=pathlib.Path, required=True,
                        help="directory produced by cmake --install with BUILD_GUI=ON")
    parser.add_argument("--output", type=pathlib.Path, required=True, help="output .dmg path")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("DMG packaging requires macOS and hdiutil")

    stage = args.stage.resolve()
    app = stage / "deskflow-otg-gui.app"
    docs = stage / "share/deskflow-otg"
    if not (app / "Contents/MacOS/deskflow-otg-gui").is_file() or not (
            app / "Contents/MacOS/deskflow-otg").is_file() or not (
            app / "Contents/MacOS/platform-tools/adb").is_file():
        parser.error("--stage must contain the installed GUI app, bridge and bundled ADB")
    if not (docs / "LICENSE").is_file() or not (docs / "licenses").is_dir():
        parser.error("--stage must contain share/deskflow-otg/LICENSE and licenses")

    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="deskflow-otg-dmg-") as temp:
        root = pathlib.Path(temp) / "image"
        root.mkdir()
        bundled_app = root / app.name
        shutil.copytree(app, bundled_app, symlinks=True)
        # The app is moved out of the image on installation, so licenses must
        # travel with it rather than live only beside it in the DMG.
        shutil.copytree(docs, bundled_app / "Contents/Resources/deskflow-otg", symlinks=True)
        (root / "Applications").symlink_to("/Applications", target_is_directory=True)
        subprocess.run(["hdiutil", "create", "-volname", "Deskflow OTG", "-srcfolder",
                        str(root), "-format", "UDZO", "-ov", str(output)], check=True)
    print(output)


if __name__ == "__main__":
    main()
