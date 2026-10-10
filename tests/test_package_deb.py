# SPDX-License-Identifier: Apache-2.0
"""Check desktop integration in the actual Debian package without installing it."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(shutil.which("dpkg-deb") and shutil.which("dpkg"), "requires Debian tools")
class DebPackageTest(unittest.TestCase):
    def test_desktop_icon(self):
        repository = pathlib.Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            stage = root / "stage"
            (stage / "bin").mkdir(parents=True)
            for name in ("deskflow-otg-gui", "deskflow-otg"):
                (stage / "bin" / name).write_text("mock binary\n")
            package = root / "test.deb"
            subprocess.run([sys.executable, str(repository / "scripts/package_deb.py"),
                            "--stage", str(stage), "--version", "0.1.2+test1",
                            "--output", str(package)], check=True)
            extracted = root / "extracted"
            subprocess.run(["dpkg-deb", "--extract", str(package), str(extracted)], check=True)
            desktop = (extracted / "usr/share/applications/deskflow-otg.desktop").read_text()
            self.assertIn("\nIcon=deskflow-otg\n", desktop)
            icon = extracted / "usr/share/icons/hicolor/scalable/apps/deskflow-otg.svg"
            self.assertEqual(icon.read_bytes(), (repository / "packaging/deskflow-otg.svg").read_bytes())


if __name__ == "__main__":
    unittest.main()
