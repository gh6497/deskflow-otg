#!/usr/bin/env python3
"""Compile the real Android transfer core on a host JDK; no phone or SDK required."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "android/deskflow-otg/app/src"
PACKAGE = Path("com/github/gh6497/deskflowotg")


def main():
    if not shutil.which("javac") or not shutil.which("java"):
        return 77
    # Manifest is part of the security boundary; only passive network-state
    # inspection is needed by the wireless ADB helper.
    manifest = ET.parse(APP / "main/AndroidManifest.xml").getroot()
    android = "{http://schemas.android.com/apk/res/android}"
    provider = next(p for p in manifest.findall("application/provider")
                    if p.get(android + "name") == ".TransferProvider")
    assert provider.get(android + "readPermission") == "android.permission.DUMP"
    assert provider.get(android + "writePermission") == "android.permission.DUMP"
    assert provider.get(android + "grantUriPermissions") == "false"
    permissions = [p.get(android + "name") for p in manifest.findall("uses-permission")]
    assert permissions == ["android.permission.ACCESS_NETWORK_STATE"]
    for language in ("values", "values-zh-rCN"):
        resources = ET.parse(APP / f"main/res/{language}/strings.xml").getroot()
        names = [s.get("name") for s in resources if s.get("translatable") != "false"]
        assert len(names) == len(set(names))
        if language == "values":
            base = set(names)
        else:
            assert set(names) == base
    # Keep temporary files outside the repository, including under CTest/CI.
    temp_root = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
    with tempfile.TemporaryDirectory(prefix="android-transfer-", dir=temp_root) as temp:
        sources = [APP / "main/java" / PACKAGE / f"{name}.java"
                   for name in ("TransferWire", "TransferStore")]
        sources.append(APP / "test/java" / PACKAGE / "TransferTest.java")
        subprocess.run(["javac", "-encoding", "UTF-8", "-d", temp, *map(str, sources)], check=True)
        subprocess.run(["java", "-cp", temp, "com.github.gh6497.deskflowotg.TransferTest", temp], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
