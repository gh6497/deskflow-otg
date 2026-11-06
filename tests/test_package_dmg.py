"""Host-independent checks for the macOS DMG staging layout."""

import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))
import package_dmg  # noqa: E402


class PackageDmgTest(unittest.TestCase):
    def test_contents_and_applications_link(self):
        self.check_contents_and_applications_link(existing_resources=False)

    def test_merges_existing_app_resources(self):
        self.check_contents_and_applications_link(existing_resources=True)

    def check_contents_and_applications_link(self, existing_resources):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            stage = root / "stage"
            app = stage / "deskflow-otg-gui.app"
            for relative in ("Contents/MacOS/deskflow-otg-gui",
                             "Contents/MacOS/deskflow-otg",
                             "Contents/MacOS/platform-tools/adb"):
                file = app / relative
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text("binary")
            docs = stage / "share/deskflow-otg"
            (docs / "licenses").mkdir(parents=True)
            (docs / "LICENSE").write_text("project license")
            (docs / "licenses/libusb-LGPL-2.1.txt").write_text("libusb license")
            resources = app / "Contents/Resources/deskflow-otg"
            if existing_resources:
                (resources / "licenses").mkdir(parents=True)
                (resources / "scrcpy-server").write_bytes(b"scrcpy runtime")
                (resources / "SCRCPY_VERSION.txt").write_text("5.0.1\n")
                (resources / "LICENSE").write_text("old project license")
                (resources / "licenses/libusb-LGPL-2.1.txt").write_text("old libusb license")
            output = root / "dist/output.dmg"

            def check_image(command, check):
                self.assertTrue(check)
                self.assertEqual(command[:2], ["hdiutil", "create"])
                image = pathlib.Path(command[command.index("-srcfolder") + 1])
                self.assertEqual((image / "Applications").readlink(), pathlib.Path("/Applications"))
                installed = image / app.name
                self.assertTrue((installed / "Contents/MacOS/deskflow-otg").is_file())
                bundled_resources = installed / "Contents/Resources/deskflow-otg"
                self.assertEqual((bundled_resources / "LICENSE").read_text(), "project license")
                self.assertEqual((bundled_resources / "licenses/libusb-LGPL-2.1.txt").read_text(),
                                 "libusb license")
                if existing_resources:
                    self.assertEqual((bundled_resources / "scrcpy-server").read_bytes(),
                                     b"scrcpy runtime")
                    self.assertEqual((bundled_resources / "SCRCPY_VERSION.txt").read_text(),
                                     "5.0.1\n")
                output.write_text("mock image")
                return subprocess.CompletedProcess(command, 0)

            with mock.patch.object(package_dmg.sys, "platform", "darwin"), \
                    mock.patch.object(package_dmg.subprocess, "run", side_effect=check_image), \
                    mock.patch.object(sys, "argv", ["package_dmg.py", "--stage", str(stage),
                                                    "--output", str(output)]):
                package_dmg.main()
            self.assertTrue(output.is_file())
            if existing_resources:
                self.assertEqual((resources / "LICENSE").read_text(), "old project license")
                self.assertEqual((resources / "licenses/libusb-LGPL-2.1.txt").read_text(),
                                 "old libusb license")
            else:
                self.assertFalse(resources.exists())

    def test_rejects_missing_app(self):
        with tempfile.TemporaryDirectory() as temp:
            with mock.patch.object(package_dmg.sys, "platform", "darwin"), \
                    mock.patch.object(sys, "argv", ["package_dmg.py", "--stage", temp,
                                                    "--output", str(pathlib.Path(temp) / "out.dmg")]):
                with self.assertRaises(SystemExit) as error:
                    package_dmg.main()
                self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
