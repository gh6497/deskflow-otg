"""Verify the Windows installer's manifest without requiring NSIS or Windows."""

import pathlib
import tempfile
import unittest

from scripts.package_windows import make_script


class WindowsPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.stage = pathlib.Path(self.temp.name) / 'stage'
        for name in ('bin/deskflow-otg-gui.exe', 'bin/deskflow-otg.exe',
                     'bin/platform-tools/adb.exe', 'bin/platform-tools/AdbWinApi.dll',
                     'bin/plugins/platforms/qwindows.dll',
                     'share/deskflow-otg/LICENSE', 'share/deskflow-otg/README.md',
                     'share/deskflow-otg/THIRD_PARTY.md',
                     'share/deskflow-otg/licenses/libusb-LGPL-2.1.txt'):
            path = self.stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'test')

    def test_manifest_installs_and_uninstalls_every_file(self):
        script = make_script(self.stage, pathlib.Path(self.temp.name) / 'setup.exe', '0.1.0')
        for path in self.stage.rglob('*'):
            if path.is_file():
                relative = str(path.relative_to(self.stage)).replace('/', '\\')
                self.assertIn(f'  File "{path}"', script)
                self.assertIn(f'  Delete "$INSTDIR\\{relative}"', script)
        self.assertIn('RequestExecutionLevel user', script)
        self.assertIn('IfFileExists "$INSTDIR\\Uninstall.exe" 0 +4', script)
        self.assertIn('ExecWait', script)
        self.assertIn('Page instfiles', script)
        self.assertIn('deskflow-otg-gui.exe', script)
        self.assertNotIn('RMDir /r', script)

    def test_rejects_missing_bundle(self):
        (self.stage / 'bin/platform-tools/adb.exe').unlink()
        with self.assertRaisesRegex(ValueError, 'bundled ADB'):
            make_script(self.stage, pathlib.Path(self.temp.name) / 'setup.exe', '0.1.0')

    def test_rejects_unsafe_paths(self):
        (self.stage / 'bin/bad$name.dll').write_bytes(b'test')
        with self.assertRaisesRegex(ValueError, 'NSIS-unsafe'):
            make_script(self.stage, pathlib.Path(self.temp.name) / 'setup.exe', '0.1.0')


if __name__ == '__main__':
    unittest.main()
