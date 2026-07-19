#!/usr/bin/env python3
"""Reuse or provision the repository-local r-nacos test binary."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path
from typing import Any


REPOSITORY = "nacos-group/r-nacos"
API_ROOT = f"https://api.github.com/repos/{REPOSITORY}/releases"


class ProvisionError(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Reuse temp/rnacos, or download a matching official stable release when it is absent."
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path.cwd(),
        help="repository root containing temp/ (default: current directory)",
    )
    parser.add_argument(
        "--version",
        help="specific release version or tag, such as 0.8.2 or v0.8.2 (default: latest stable)",
    )
    return parser.parse_args()


def describe_binary(path: Path) -> str:
    try:
        result = subprocess.run(
            [path, "--version"],
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise ProvisionError(f"cannot execute cached binary {path}: {error}") from error

    version = (result.stdout or result.stderr).strip()
    if result.returncode != 0:
        raise ProvisionError(
            f"cached binary {path} failed --version with exit code {result.returncode}: {version}"
        )
    return version or "version unknown"


def fetch_json(url: str) -> dict[str, Any]:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "fiber-gateway-cpp-rnacos-provisioner",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    except (urllib.error.URLError, json.JSONDecodeError) as error:
        raise ProvisionError(f"failed to query {url}: {error}") from error


def release_url(version: str | None) -> str:
    if not version:
        return f"{API_ROOT}/latest"
    tag = version if version.startswith("v") else f"v{version}"
    return f"{API_ROOT}/tags/{urllib.parse.quote(tag, safe='')}"


def normalized_machine() -> str:
    machine = platform.machine().lower()
    aliases = {
        "amd64": "x86_64",
        "arm64": "aarch64",
        "i386": "i686",
        "i686": "i686",
    }
    return aliases.get(machine, machine)


def platform_marker() -> tuple[str, str]:
    system = platform.system().lower()
    machine = normalized_machine()

    if system == "linux":
        libc_name = platform.libc_ver()[0].lower()
        abi = "musl" if "musl" in libc_name else "gnu"
        return f"rnacos-{machine}-unknown-linux-{abi}-", ".tar.gz"
    if system == "darwin":
        return f"rnacos-{machine}-apple-darwin-", ".tar.gz"
    if system == "windows":
        return f"rnacos-{machine}-pc-windows-msvc-", ".zip"
    raise ProvisionError(f"unsupported platform: {platform.system()} {platform.machine()}")


def select_asset(release: dict[str, Any]) -> dict[str, Any]:
    marker, suffix = platform_marker()
    matches = [
        asset
        for asset in release.get("assets", [])
        if asset.get("name", "").startswith(marker) and asset.get("name", "").endswith(suffix)
        and "-mimalloc-" not in asset.get("name", "")
    ]
    if len(matches) != 1:
        available = ", ".join(asset.get("name", "<unnamed>") for asset in release.get("assets", []))
        raise ProvisionError(
            f"expected one asset matching {marker}*{suffix}, found {len(matches)}; available: {available}"
        )
    return matches[0]


def download_asset(asset: dict[str, Any], destination: Path) -> None:
    digest = asset.get("digest")
    if not isinstance(digest, str) or not digest.startswith("sha256:"):
        raise ProvisionError(f"release asset {asset.get('name')} has no published SHA-256 digest")

    request = urllib.request.Request(
        asset["browser_download_url"],
        headers={"User-Agent": "fiber-gateway-cpp-rnacos-provisioner"},
    )
    hasher = hashlib.sha256()
    try:
        with urllib.request.urlopen(request, timeout=60) as response, destination.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                output.write(chunk)
                hasher.update(chunk)
    except (OSError, urllib.error.URLError) as error:
        raise ProvisionError(f"failed to download {asset.get('name')}: {error}") from error

    actual = hasher.hexdigest()
    expected = digest.removeprefix("sha256:").lower()
    if actual != expected:
        raise ProvisionError(
            f"SHA-256 mismatch for {asset.get('name')}: expected {expected}, received {actual}"
        )


def extract_binary(archive: Path, destination: Path) -> None:
    binary_name = "rnacos.exe" if platform.system().lower() == "windows" else "rnacos"

    try:
        if archive.name.endswith(".tar.gz"):
            with tarfile.open(archive, "r:gz") as bundle:
                members = [
                    member
                    for member in bundle.getmembers()
                    if member.isfile() and Path(member.name).name == binary_name
                ]
                if len(members) != 1:
                    raise ProvisionError(
                        f"expected one {binary_name} file in {archive.name}, found {len(members)}"
                    )
                source = bundle.extractfile(members[0])
                if source is None:
                    raise ProvisionError(f"could not read {members[0].name} from {archive.name}")
                with source, destination.open("wb") as output:
                    shutil.copyfileobj(source, output)
            return

        if archive.name.endswith(".zip"):
            with zipfile.ZipFile(archive) as bundle:
                members = [
                    name
                    for name in bundle.namelist()
                    if Path(name).name == binary_name and not name.endswith("/")
                ]
                if len(members) != 1:
                    raise ProvisionError(
                        f"expected one {binary_name} file in {archive.name}, found {len(members)}"
                    )
                with bundle.open(members[0]) as source, destination.open("wb") as output:
                    shutil.copyfileobj(source, output)
            return

        raise ProvisionError(f"unsupported archive format: {archive.name}")
    except (OSError, tarfile.TarError, zipfile.BadZipFile) as error:
        raise ProvisionError(f"invalid release archive {archive.name}: {error}") from error


def provision(repo_root: Path, version: str | None) -> Path:
    root = repo_root.resolve()
    target = root / "temp" / ("rnacos.exe" if platform.system().lower() == "windows" else "rnacos")

    if target.exists():
        if not target.is_file():
            raise ProvisionError(f"cached path is not a regular file: {target}")
        if not os.access(target, os.X_OK):
            raise ProvisionError(f"cached binary is not executable: {target}")
        print(f"Using cached {target} ({describe_binary(target)})")
        return target

    release = fetch_json(release_url(version))
    if release.get("draft"):
        raise ProvisionError(f"refusing draft release {release.get('tag_name')}")
    if release.get("prerelease") and version is None:
        raise ProvisionError(f"latest release unexpectedly resolves to prerelease {release.get('tag_name')}")

    asset = select_asset(release)
    target.parent.mkdir(parents=True, exist_ok=True)
    archive_suffix = ".zip" if asset["name"].endswith(".zip") else ".tar.gz"
    archive_fd, archive_text = tempfile.mkstemp(prefix=".rnacos-download-", suffix=archive_suffix, dir=target.parent)
    os.close(archive_fd)
    binary_fd, binary_text = tempfile.mkstemp(prefix=".rnacos-binary-", dir=target.parent)
    os.close(binary_fd)
    archive = Path(archive_text)
    candidate = Path(binary_text)

    try:
        print(f"Downloading {asset['name']} from official release {release.get('tag_name')}...")
        download_asset(asset, archive)
        extract_binary(archive, candidate)
        candidate.chmod(candidate.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        version_text = describe_binary(candidate)

        if target.exists():
            if not target.is_file() or not os.access(target, os.X_OK):
                raise ProvisionError(f"cached path appeared during download but is unusable: {target}")
            print(f"Using concurrently installed {target} ({describe_binary(target)})")
            return target

        os.replace(candidate, target)
        print(f"Installed {target} ({version_text})")
        return target
    finally:
        archive.unlink(missing_ok=True)
        candidate.unlink(missing_ok=True)


def main() -> int:
    args = parse_args()
    try:
        provision(args.repo_root, args.version)
    except ProvisionError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
