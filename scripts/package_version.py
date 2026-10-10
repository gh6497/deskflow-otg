# SPDX-License-Identifier: Apache-2.0
"""Read the build version from installed artifacts, never from the current checkout."""
import re


def package_version(stage, requested=None):
    path = stage / "share/deskflow-otg/BUILD_VERSION.txt"
    built = path.read_text(encoding="utf-8").strip() if path.is_file() else None
    if built and requested and built != requested:
        raise ValueError(f"requested version {requested} does not match staged build {built}")
    version = built or requested
    if not version or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[+~][A-Za-z0-9.+~\-]+)?", version):
        raise ValueError("stage must contain BUILD_VERSION.txt, or supply a valid --version for older builds")
    return version
