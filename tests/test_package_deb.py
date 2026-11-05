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
    def test_ble_dependencies_and_service(self):
        repository = pathlib.Path(__file__).resolve().parent.parent
        for ble in (True, False):
            with self.subTest(ble=ble), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                stage = root / "stage"
                (stage / "bin").mkdir(parents=True)
                for name in ("deskflow-otg-gui", "deskflow-otg"):
                    (stage / "bin" / name).write_text("mock binary\n")
                metadata = stage / "share/deskflow-otg"
                metadata.mkdir(parents=True)
                (metadata / "BUILD_FEATURES.json").write_text('{"ble":%s}' % str(ble).lower())
                package = root / "test.deb"
                subprocess.run([sys.executable, str(repository / "scripts/package_deb.py"),
                                "--stage", str(stage), "--version", "0.1.2+test1",
                                "--output", str(package)], check=True)
                deps = subprocess.check_output(["dpkg-deb", "--field", str(package), "Depends"], text=True)
                self.assertTrue("libssl3" in deps or "libssl3t64" in deps)
                self.assertEqual("bluez" in deps, ble)
                self.assertEqual("pkexec" in deps, ble)
                extracted = root / "extracted"
                subprocess.run(["dpkg-deb", "--extract", str(package), str(extracted)], check=True)
                self.assertTrue((extracted / "usr/bin/deskflow-otg").is_symlink())
                unit = extracted / "usr/lib/systemd/user/deskflow-otg-ble@.service"
                self.assertEqual(unit.exists(), ble)
                if ble:
                    self.assertIn("--ble-service --adapter %i", unit.read_text())
                    control = root / "control"
                    subprocess.run(["dpkg-deb", "--control", str(package), str(control)], check=True)
                    self.assertFalse((control / "postinst").exists())

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

    def test_optional_system_theme(self):
        repository = pathlib.Path(__file__).resolve().parent.parent
        for gtk in (True, False):
            with self.subTest(gtk=gtk), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                stage = root / "stage"
                (stage / "bin").mkdir(parents=True)
                for name in ("deskflow-otg-gui", "deskflow-otg"):
                    (stage / "bin" / name).write_text("mock binary\n")
                themes = stage / "plugins/platformthemes"
                themes.mkdir(parents=True)
                (themes / "libqxdgdesktopportal.so").write_bytes(b"mock portal plugin")
                if gtk:
                    (themes / "libqgtk3.so").write_bytes(b"mock gtk plugin")
                package = root / "test.deb"
                subprocess.run([sys.executable, str(repository / "scripts/package_deb.py"),
                                "--stage", str(stage), "--version", "0.1.2+test1",
                                "--output", str(package)], check=True)
                deps = subprocess.check_output(["dpkg-deb", "--field", str(package), "Depends"], text=True)
                self.assertNotIn("libgtk", deps)
                recommends = subprocess.check_output(
                    ["dpkg-deb", "--field", str(package), "Recommends"], text=True)
                self.assertEqual("libgtk-3-0t64 | libgtk-3-0" in recommends, gtk)
                extracted = root / "extracted"
                subprocess.run(["dpkg-deb", "--extract", str(package), str(extracted)], check=True)
                installed = extracted / "opt/deskflow-otg/plugins/platformthemes"
                self.assertTrue((installed / "libqxdgdesktopportal.so").is_file())
                self.assertEqual((installed / "libqgtk3.so").is_file(), gtk)
                desktop = (extracted / "usr/share/applications/deskflow-otg.desktop").read_text()
                self.assertNotIn("QT_QPA_PLATFORMTHEME", desktop)
                self.assertNotIn("QT_PLUGIN_PATH", desktop)
                self.assertNotIn("QT_STYLE_OVERRIDE", desktop)


if __name__ == "__main__":
    unittest.main()
