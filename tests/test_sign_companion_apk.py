# SPDX-License-Identifier: Apache-2.0
"""Offline signing-policy tests; never use a real release key or a phone."""

import base64
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))
import sign_companion_apk  # noqa: E402


class CompanionSigningTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.source = self.root / "app-release-unsigned.apk"
        self.source.write_bytes(b"unsigned release")
        self.output = self.root / "bundle/deskflow-otg-companion.apk"
        self.tools = self.root / "build-tools"
        self.tools.mkdir()
        for name in ("aapt", "zipalign", "apksigner"):
            (self.tools / name).touch()
        env = {"ANDROID_KEYSTORE_BASE64": base64.b64encode(b"private fake key").decode(),
               "ANDROID_KEYSTORE_PASSWORD": "test-store-password",
               "ANDROID_KEY_ALIAS": "release", "ANDROID_KEY_PASSWORD": "test-key-password"}
        patch = mock.patch.dict(os.environ, env)
        patch.start()
        self.addCleanup(patch.stop)
        self.badging = "package: name='com.github.gh6497.deskflowotg' versionCode='13' versionName='0.1.11'\n"
        self.commands = []

    def sdk(self, command, **kwargs):
        self.commands.append(command)
        self.assertTrue(kwargs["check"])
        self.assertTrue(kwargs["capture_output"])
        self.assertEqual(kwargs["stdin"], subprocess.DEVNULL)
        self.assertNotIn("ANDROID_KEYSTORE_BASE64", kwargs["env"])
        self.assertNotIn("test-store-password", " ".join(command))
        self.assertNotIn("test-key-password", " ".join(command))
        if command[1:3] == ["dump", "badging"]:
            return subprocess.CompletedProcess(command, 0, stdout=self.badging)
        if pathlib.Path(command[0]).name == "zipalign":
            pathlib.Path(command[-1]).write_bytes(b"aligned release")
        elif command[1] == "sign":
            key = pathlib.Path(command[command.index("--ks") + 1])
            self.assertEqual(key.read_bytes(), b"private fake key")
            if os.name != "nt":
                self.assertEqual(key.stat().st_mode & 0o777, 0o600)
            self.assertIn("env:ANDROID_KEYSTORE_PASSWORD", command)
            self.assertIn("env:ANDROID_KEY_PASSWORD", command)
            pathlib.Path(command[command.index("--out") + 1]).write_bytes(b"signed release")
        return subprocess.CompletedProcess(command, 0, stdout="")

    def sign(self):
        sign_companion_apk.sign_apk(self.source, self.output, self.tools)

    def test_signs_verifies_then_publishes_and_removes_key(self):
        with mock.patch.object(subprocess, "run", side_effect=self.sdk):
            self.sign()
        self.assertEqual([pathlib.Path(c[0]).name for c in self.commands],
                         ["aapt", "zipalign", "apksigner", "apksigner"])
        self.assertEqual(self.commands[-1][1], "verify")
        self.assertEqual(self.output.read_bytes(), b"signed release")
        self.assertEqual(list(self.output.parent.iterdir()), [self.output])
        self.assertEqual(self.source.read_bytes(), b"unsigned release")

    def test_failed_verification_preserves_old_bundle_and_cleans_up(self):
        self.output.parent.mkdir()
        self.output.write_bytes(b"old signed release")

        def fail_verify(command, **kwargs):
            if command[1] == "verify":
                raise subprocess.CalledProcessError(1, command)
            return self.sdk(command, **kwargs)

        with mock.patch.object(subprocess, "run", side_effect=fail_verify):
            with self.assertRaises(subprocess.CalledProcessError):
                self.sign()
        self.assertEqual(self.output.read_bytes(), b"old signed release")
        self.assertEqual(list(self.output.parent.iterdir()), [self.output])

    def test_rejects_debug_or_wrong_package_before_signing(self):
        for badging in (self.badging + "application-debuggable\n",
                        "package: name='another.application' versionCode='1'\n"):
            with self.subTest(badging=badging):
                self.badging = badging
                with mock.patch.object(subprocess, "run", side_effect=self.sdk):
                    with self.assertRaises(ValueError):
                        self.sign()
                self.assertFalse(self.output.exists())
        self.assertTrue(all(command[1] == "dump" for command in self.commands))

    def test_requires_all_secrets(self):
        for name in sign_companion_apk.SECRETS:
            with self.subTest(name=name), mock.patch.dict(os.environ, {name: ""}), \
                    mock.patch.object(subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, name):
                    self.sign()
                run.assert_not_called()

    def test_rejects_invalid_base64_without_leaking_value(self):
        with mock.patch.dict(os.environ, {"ANDROID_KEYSTORE_BASE64": "invalid-private-data!"}):
            with self.assertRaisesRegex(ValueError, "not valid base64") as error:
                self.sign()
            self.assertNotIn("invalid-private-data", str(error.exception))

    def test_rejects_missing_sdk_and_overwriting_input(self):
        (self.tools / "zipalign").unlink()
        with self.assertRaisesRegex(ValueError, "tool missing"):
            self.sign()
        with self.assertRaisesRegex(ValueError, "different from output"):
            sign_companion_apk.sign_apk(self.source, self.source, self.tools)


if __name__ == "__main__":
    unittest.main()
