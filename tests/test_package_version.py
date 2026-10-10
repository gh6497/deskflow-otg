# SPDX-License-Identifier: Apache-2.0
import pathlib
import tempfile
import unittest
from scripts.package_version import package_version


class PackageVersionTest(unittest.TestCase):
    def test_uses_staged_build(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = pathlib.Path(directory)
            path = stage / "share/deskflow-otg/BUILD_VERSION.txt"
            path.parent.mkdir(parents=True)
            path.write_text("0.1.2+git.123abc.dirty\n")
            self.assertEqual(package_version(stage), "0.1.2+git.123abc.dirty")
            self.assertEqual(package_version(stage, "0.1.2+git.123abc.dirty"), package_version(stage))
            with self.assertRaisesRegex(ValueError, "does not match"):
                package_version(stage, "0.1.3")

    def test_legacy_and_invalid_version(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = pathlib.Path(directory)
            self.assertEqual(package_version(stage, "0.1.2"), "0.1.2")
            for version in (None, "", '1.0.0"', "v1.0.0", "1.0.0\n", "arbitrary"):
                with self.assertRaises(ValueError):
                    package_version(stage, version)


if __name__ == "__main__":
    unittest.main()
