#!/usr/bin/env python3
"""Collect upstream dependency license texts for release staging."""
import argparse
from pathlib import Path
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
sources = {
    "Qt-LGPL-3.0.txt": "https://raw.githubusercontent.com/qt/qtbase/v6.8.3/LICENSES/LGPL-3.0-only.txt",
    "Qt-GPL-3.0.txt": "https://raw.githubusercontent.com/qt/qtbase/v6.8.3/LICENSES/GPL-3.0-only.txt",
    "libusb-LGPL-2.1.txt": "https://raw.githubusercontent.com/libusb/libusb/v1.0.27/COPYING",
}
for name, url in sources.items():
    with urllib.request.urlopen(url, timeout=60) as response:
        (args.output / name).write_bytes(response.read())
