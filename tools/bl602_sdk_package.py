"""Prepare a PlatformIO wrapper for the pinned BL602 Arduino SDK checkout.

The upstream checkout is not a PlatformIO package. This only populates a
versioned local build cache; no flash images are created or uploaded.
"""

import errno
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path
from urllib.request import Request, urlopen

SDK_REVISION = "2f1db433bc832b7aa8cc3d427527d48e66bcce29"
SDK_REPOSITORY = "https://github.com/Ishu1519/BL602-Arduino-Core.git"
LWIP_MDNS_REVISION = "159e31b689577dbf69cf0683bbaffbd71fa5ee10"
LWIP_MDNS_SHA256 = "31ae5b30abd30ad70e1cfcae121d76756e45f12559f6ecf14f9f6cab55f7d426"
LWIP_MDNS_URL = (
    "https://raw.githubusercontent.com/lwip-tcpip/lwip/"
    f"{LWIP_MDNS_REVISION}/src/apps/mdns/mdns.c"
)
LWIP_MDNS_PATH = Path("system/upstream/lwip/src/apps/mdns/mdns.c")
PACKAGE_REVISION = LWIP_MDNS_REVISION[:12]


def _sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(64 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _download_lwip_mdns():
    request = Request(LWIP_MDNS_URL, headers={"User-Agent": "LibreTiny-BL602"})
    with urlopen(request, timeout=60) as response:
        source = response.read()
    actual = hashlib.sha256(source).hexdigest()
    if actual != LWIP_MDNS_SHA256:
        raise RuntimeError(
            f"Upstream lwIP mDNS source is {actual}, expected {LWIP_MDNS_SHA256}"
        )
    return source


def _is_valid_package(package):
    marker = package / ".sdk-revision"
    lwip_marker = package / ".lwip-mdns-revision"
    lwip_source = package / LWIP_MDNS_PATH
    return (
        marker.is_file()
        and marker.read_text(encoding="utf-8").strip() == SDK_REVISION
        and lwip_marker.is_file()
        and lwip_marker.read_text(encoding="utf-8").strip() == LWIP_MDNS_REVISION
        and lwip_source.is_file()
        and _sha256(lwip_source) == LWIP_MDNS_SHA256
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
    package = cache / f"{SDK_REVISION}-lwip-{PACKAGE_REVISION}"
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
        lwip_source = staged / LWIP_MDNS_PATH
        lwip_source.parent.mkdir(parents=True)
        lwip_source.write_bytes(_download_lwip_mdns())
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
        (staged / ".lwip-mdns-revision").write_text(
            LWIP_MDNS_REVISION + "\n", encoding="utf-8"
        )
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
