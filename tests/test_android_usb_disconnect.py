#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run companion USB and multi-player policies on a host JVM, without a phone."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    javac, java = shutil.which("javac"), shutil.which("java")
    if not javac or not java:
        print("SKIP: JDK required for Android USB disconnect policy regression")
        return 77
    root = Path(__file__).resolve().parent.parent
    app = root / "android/deskflow-otg/app/src"
    package = "com/github/gh6497/deskflowotg"
    with tempfile.TemporaryDirectory(prefix="otg-usb-disconnect-") as directory:
        subprocess.run([javac, "-encoding", "UTF-8", "-Xlint:all", "-Werror", "-d", directory,
                        str(app / "main/java" / package / "UsbDisconnectMonitor.java"),
                        str(app / "test/java" / package / "UsbDisconnectMonitorTest.java"),
                        str(app / "main/java" / package / "PlaybackPauser.java"),
                        str(app / "test/java" / package / "PlaybackPauserTest.java"),
                        str(app / "main/java" / package / "ConnectionStatus.java"),
                        str(app / "test/java" / package / "ConnectionStatusTest.java"),
                        str(app / "main/java" / package / "AutoplayPolicy.java"),
                        str(app / "test/java" / package / "AutoplayPolicyTest.java")],
                       check=True, timeout=30)
        subprocess.run([java, "-cp", directory,
                        "com.github.gh6497.deskflowotg.UsbDisconnectMonitorTest"],
                       check=True, timeout=10)
        subprocess.run([java, "-cp", directory,
                        "com.github.gh6497.deskflowotg.PlaybackPauserTest"],
                        check=True, timeout=10)
        subprocess.run([java, "-cp", directory,
                        "com.github.gh6497.deskflowotg.ConnectionStatusTest"],
                       check=True, timeout=10)
        subprocess.run([java, "-cp", directory,
                        "com.github.gh6497.deskflowotg.AutoplayPolicyTest"],
                       check=True, timeout=10)
    return 0


if __name__ == "__main__":
    sys.exit(main())
