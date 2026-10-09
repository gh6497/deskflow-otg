#!/usr/bin/env python3
"""Package an installed Linux distribution tree as a Debian package."""

import argparse
import pathlib
import re
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=pathlib.Path, required=True,
                        help="absolute-prefix directory produced by cmake --install")
    parser.add_argument("--version", required=True, help="Debian package version, e.g. 0.1.0")
    parser.add_argument("--output", type=pathlib.Path, required=True, help="output .deb path")
    args = parser.parse_args()

    if not re.fullmatch(r"[0-9][A-Za-z0-9.+:~\-]*", args.version):
        parser.error("--version must be a valid Debian version starting with a digit")
    stage = args.stage.resolve()
    if not (stage / "bin/deskflow-otg-gui").is_file() or not (stage / "bin/deskflow-otg").is_file():
        parser.error("--stage must contain bin/deskflow-otg-gui and bin/deskflow-otg "
                     "(build with BUILD_GUI=ON and run cmake --install first)")
    arch = subprocess.check_output(["dpkg", "--print-architecture"], text=True).strip()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="deskflow-otg-deb-") as temp:
        root = pathlib.Path(temp) / "root"
        app = root / "opt/deskflow-otg"
        shutil.copytree(stage, app, symlinks=True)
        docs = root / "usr/share/doc/deskflow-otg"
        docs.mkdir(parents=True)
        shutil.copyfile(pathlib.Path(__file__).resolve().parent.parent / "LICENSE",
                        docs / "copyright")
        bindir = root / "usr/bin"
        bindir.mkdir(parents=True)
        (bindir / "deskflow-otg-gui").symlink_to("/opt/deskflow-otg/bin/deskflow-otg-gui")
        applications = root / "usr/share/applications"
        applications.mkdir(parents=True)
        (applications / "deskflow-otg.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=Deskflow OTG\n"
            "Comment=Control an Android device with Deskflow via USB\n"
            "Exec=/opt/deskflow-otg/bin/deskflow-otg-gui\n"
            "Terminal=false\nCategories=Utility;\n", encoding="utf-8")

        control = root / "DEBIAN"
        control.mkdir()
        # Qt and ADB are bundled, but their platform plugins still need system libraries.
        dependencies = ("libusb-1.0-0, libc6, libstdc++6, libgl1, libegl1, "
                        "libx11-6, libxcb1, libxkbcommon0, libxcb-cursor0")
        (control / "control").write_text(
            f"Package: deskflow-otg-gui\nVersion: {args.version}\nArchitecture: {arch}\n"
            "Maintainer: deskflow-otg contributors <noreply@github.com>\n"
            f"Depends: {dependencies}\n"
            "Section: utils\nPriority: optional\n"
            "Homepage: https://github.com/gh6497/deskflow-otg\n"
            "Description: Deskflow to Android USB HID bridge\n"
            " Control an Android device with a Deskflow server via USB AOA HID.\n",
            encoding="utf-8")
        subprocess.run(["dpkg-deb", "--build", "--root-owner-group", str(root), str(output)],
                       check=True)
    print(output)


if __name__ == "__main__":
    main()
