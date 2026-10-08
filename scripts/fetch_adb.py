#!/usr/bin/env python3
"""Fetch pinned platform-tools, verify SHA-256, retain binaries and notices.

Usage: python scripts/fetch_adb.py --platform linux --output build/adb
The resulting build/adb/platform-tools is passed as ADB_BUNDLE_DIR.
"""

import argparse
import hashlib
import io
from pathlib import Path
import sys
import urllib.request
import zipfile

VERSION = "36.0.2"
ARCHIVES = {
    "linux": ("linux", "3afdea91441815ab41254193df0343d92c1b1c0d0237165c3a345c8af8891c31"),
    "darwin": ("darwin", "106a5d31fad8c1c0c5a180d06f5779767d129d7d5edbe629005c11a85eec5b4b"),
    "win32": ("win", "b024d4f319d6ad3004de1ba7b96a5c7c5f3512e8b14126308d598b4ab93dcead"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=ARCHIVES, default=sys.platform)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--archive", type=Path, help="Use an already downloaded archive (still verified)")
    args = parser.parse_args()
    suffix, expected = ARCHIVES[args.platform]
    url = f"https://dl.google.com/android/repository/platform-tools_r{VERSION}-{suffix}.zip"
    if args.archive:
        data = args.archive.read_bytes()
    else:
        with urllib.request.urlopen(url, timeout=120) as response:
            data = response.read()
    if hashlib.sha256(data).hexdigest() != expected:
        raise SystemExit("platform-tools checksum mismatch")
    destination = args.output.resolve() / "platform-tools"
    destination.mkdir(parents=True, exist_ok=True)
    keep = {"adb", "adb.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll", "NOTICE.txt", "source.properties"}
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for info in archive.infolist():
            path = Path(info.filename)
            if path.parent.as_posix() != "platform-tools" or path.name not in keep:
                continue
            target = destination / path.name
            target.write_bytes(archive.read(info))
            target.chmod(0o755 if path.name == "adb" else 0o644)
    binary = destination / ("adb.exe" if args.platform == "win32" else "adb")
    if not binary.is_file() or not (destination / "NOTICE.txt").is_file():
        raise SystemExit("archive missing ADB or license notices")
    print(destination)


if __name__ == "__main__":
    main()
