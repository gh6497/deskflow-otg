#!/usr/bin/env python3
"""Download and cache the pinned scrcpy-server used by audio forwarding.

The output file is copied from a verified local cache whenever possible.
"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import tempfile
import urllib.request


VERSION = "5.0.1"
FILENAME = f"scrcpy-server-v{VERSION}"
URL = f"https://github.com/Genymobile/scrcpy/releases/download/v{VERSION}/{FILENAME}"
SHA256 = "764eb6f79811d5211fe9df341120882ba9994c7a61b897d7bf3fb662e53bc536"


def checksum(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verified(path: Path) -> bool:
    return path.is_file() and checksum(path) == SHA256


def download(cache: Path) -> None:
    cache.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with urllib.request.urlopen(URL, timeout=120) as response:
            with tempfile.NamedTemporaryFile(dir=cache.parent, delete=False) as target:
                temporary = Path(target.name)
                shutil.copyfileobj(response, target, length=1024 * 1024)
                target.flush()
                os.fsync(target.fileno())
        if checksum(temporary) != SHA256:
            raise SystemExit("scrcpy-server checksum mismatch")
        os.replace(temporary, cache)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="destination file passed to CMake as SCRCPY_SERVER")
    parser.add_argument("--cache-dir", type=Path, default=Path(".cache/scrcpy"),
                        help="cache directory (default: .cache/scrcpy)")
    args = parser.parse_args()

    output = args.output.resolve()
    cache = (args.cache_dir / FILENAME).resolve()
    if not verified(cache):
        if verified(output):
            cache.parent.mkdir(parents=True, exist_ok=True)
            if output != cache:
                shutil.copyfile(output, cache)
        else:
            print(f"Downloading {URL}")
            download(cache)
    else:
        print(f"Using cached {cache}")

    output.parent.mkdir(parents=True, exist_ok=True)
    if output != cache:
        shutil.copyfile(cache, output)
    output.chmod(0o644)
    print(output)


if __name__ == "__main__":
    main()
