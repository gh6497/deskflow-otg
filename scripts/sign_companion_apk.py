#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Sign the release companion APK on a Linux CI runner using environment secrets."""

import argparse
import base64
import binascii
import os
import pathlib
import re
import subprocess
import tempfile


SECRETS = ("ANDROID_KEYSTORE_BASE64", "ANDROID_KEYSTORE_PASSWORD",
           "ANDROID_KEY_ALIAS", "ANDROID_KEY_PASSWORD")


def sign_apk(source, output, tools):
    missing = [name for name in SECRETS if not os.environ.get(name)]
    if missing:
        raise ValueError("Missing signing secrets: " + ", ".join(missing))
    try:
        keystore = base64.b64decode("".join(os.environ[SECRETS[0]].split()), validate=True)
    except (ValueError, binascii.Error):
        raise ValueError("ANDROID_KEYSTORE_BASE64 is not valid base64") from None
    if not keystore:
        raise ValueError("ANDROID_KEYSTORE_BASE64 is empty")
    source, output, tools = source.resolve(), output.resolve(), tools.resolve()
    if not source.is_file() or source == output:
        raise ValueError("Input must be an existing APK different from output")
    for name in ("aapt", "zipalign", "apksigner"):
        if not (tools / name).is_file():
            raise ValueError(f"Android SDK tool missing: {tools / name}")

    # Do not forward the encoded private key to SDK subprocesses. Passwords are
    # referenced by environment variable name, never included in command lines.
    env = dict(os.environ)
    env.pop("ANDROID_KEYSTORE_BASE64", None)

    def run(*args):
        return subprocess.run([str(arg) for arg in args], env=env, check=True,
                              stdin=subprocess.DEVNULL, capture_output=True, text=True).stdout

    badging = run(tools / "aapt", "dump", "badging", source)
    if not re.search(r"^package: name='com\.github\.gh6497\.deskflowotg' ", badging, re.MULTILINE):
        raise ValueError("Input is not the deskflow-otg companion APK")
    if re.search(r"^application-debuggable(?:\s|$)", badging, re.MULTILINE):
        raise ValueError("Refusing to bundle a debuggable APK")

    output.parent.mkdir(parents=True, exist_ok=True)
    # TemporaryDirectory is private (0700); all signing material is removed on
    # success and failure. Keep it on the output filesystem for atomic replace.
    with tempfile.TemporaryDirectory(prefix=".companion-sign-", dir=output.parent) as temp:
        root = pathlib.Path(temp)
        key = root / "release.keystore"
        key.write_bytes(keystore)
        key.chmod(0o600)
        aligned = root / "aligned.apk"
        signed = root / "signed.apk"
        run(tools / "zipalign", "-f", "4", source, aligned)
        run(tools / "apksigner", "sign", "--ks", key,
            "--ks-key-alias", os.environ["ANDROID_KEY_ALIAS"],
            "--ks-pass", "env:ANDROID_KEYSTORE_PASSWORD",
            "--key-pass", "env:ANDROID_KEY_PASSWORD", "--out", signed, aligned)
        run(tools / "apksigner", "verify", "--verbose", signed)
        signed.replace(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    parser.add_argument("--build-tools", type=pathlib.Path,
                        default=pathlib.Path(sdk) / "build-tools/36.0.0" if sdk else None)
    args = parser.parse_args()
    if args.build_tools is None:
        parser.error("Set ANDROID_HOME/ANDROID_SDK_ROOT or --build-tools")
    try:
        sign_apk(args.input, args.output, args.build_tools)
    except ValueError as error:
        parser.error(str(error))
    except subprocess.CalledProcessError:
        # SDK diagnostics could contain sensitive keystore details. Never print
        # captured output or command arguments from a failed signing operation.
        parser.exit(1, "Companion APK validation/signing failed; check SDK tools and signing secrets.\n")
    print(args.output)


if __name__ == "__main__":
    main()
