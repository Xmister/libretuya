"""Prepare a PlatformIO wrapper for the pinned BL602 Arduino SDK checkout.

The upstream checkout is not a PlatformIO package. This only populates a
versioned local build cache; no flash images are created or uploaded.
"""

import errno
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

SDK_REVISION = "2f1db433bc832b7aa8cc3d427527d48e66bcce29"
SDK_REPOSITORY = "https://github.com/Ishu1519/BL602-Arduino-Core.git"


def _is_valid_package(package):
    marker = package / ".sdk-revision"
    return (
        marker.is_file()
        and marker.read_text(encoding="utf-8").strip() == SDK_REVISION
        and (package / "package.json").is_file()
        and (package / "platform.txt").is_file()
        and (package / "system/lib").is_dir()
        and (package / "system/ld").is_dir()
    )


def _run(args, cwd=None):
    return subprocess.run(
        args,
        cwd=cwd,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    ).stdout.strip()


def prepare(core_dir, checkout=None):
    cache = Path(core_dir) / "bl602-sdk-cache"
    package = cache / SDK_REVISION
    marker = package / ".sdk-revision"
    if package.exists():
        if _is_valid_package(package):
            return package
        raise RuntimeError(f"Invalid existing BL602 SDK cache at {package}")
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".prepare-", dir=cache) as temporary:
        temporary = Path(temporary)
        if checkout:
            source = Path(checkout).resolve()
        else:
            source = temporary / "checkout"
            _run(["git", "init", "-q", str(source)])
            _run(
                ["git", "fetch", "-q", "--depth", "1", SDK_REPOSITORY, SDK_REVISION],
                source,
            )
            _run(["git", "checkout", "-q", "--detach", "FETCH_HEAD"], source)
        actual = _run(["git", "rev-parse", "HEAD"], source)
        if actual != SDK_REVISION:
            raise RuntimeError(
                f"BL602 SDK checkout is {actual}, expected {SDK_REVISION}"
            )
        _run(["git", "diff", "--quiet", "HEAD", "--", "platform.txt", "system"], source)
        if _run(
            [
                "git",
                "status",
                "--porcelain",
                "--untracked-files=all",
                "--",
                "platform.txt",
                "system",
            ],
            source,
        ):
            raise RuntimeError(
                "BL602 SDK checkout has untracked or modified SDK inputs"
            )
        staged = temporary / "package"
        staged.mkdir()
        for name in ("platform.txt", "LICENSE", "THIRD_PARTY_NOTICES.md"):
            shutil.copy2(source / name, staged / name)
        shutil.copytree(source / "system", staged / "system")
        (staged / "package.json").write_text(
            json.dumps(
                {
                    "name": "framework-bouffalo-bl602",
                    "version": "0.0.1",
                    "description": f"Pinned BL602 SDK {SDK_REVISION}",
                }
            )
            + "\n",
            encoding="utf-8",
        )
        (staged / ".sdk-revision").write_text(SDK_REVISION + "\n", encoding="utf-8")
        try:
            staged.rename(package)
        except OSError as err:
            # POSIX reports ENOTEMPTY when another builder publishes a
            # non-empty directory at the same time; Windows may use EEXIST.
            if err.errno not in (errno.EEXIST, errno.ENOTEMPTY):
                raise
            if not _is_valid_package(package):
                raise RuntimeError(f"Invalid existing BL602 SDK cache at {package}")
    return package
