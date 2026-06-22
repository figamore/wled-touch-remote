#!/usr/bin/env python3
"""Build the GitHub Pages web installer from a GitHub Release.

The installer is static, but ESP Web Tools works best when firmware is served
from the same origin as the page. This script downloads the latest release
firmware into the Pages artifact and writes the manifest consumed by the
installer button.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


DEFAULT_REPO = "figamore/wled-touch-remote"
DEFAULT_SITE_DIR = Path("web-installer")
FIRMWARE_DIR = "firmware"


class InstallerError(RuntimeError):
    pass


def request_json(url: str, token: str | None = None) -> dict[str, Any]:
    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "wled-touch-remote-installer-builder",
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"

    request = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raise InstallerError(f"GitHub API request failed: {url} returned {exc.code}") from exc
    except urllib.error.URLError as exc:
        raise InstallerError(f"GitHub API request failed: {url}: {exc.reason}") from exc


def release_url(repo: str, release_tag: str) -> str:
    base = f"https://api.github.com/repos/{repo}/releases"
    if release_tag == "latest":
        return f"{base}/latest"
    return f"{base}/tags/{release_tag}"


def download_asset(asset: dict[str, Any], dest: Path, token: str | None = None) -> None:
    headers = {"User-Agent": "wled-touch-remote-installer-builder"}
    if token:
        headers["Authorization"] = f"Bearer {token}"

    request = urllib.request.Request(asset["browser_download_url"], headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            with dest.open("wb") as output:
                shutil.copyfileobj(response, output)
    except urllib.error.HTTPError as exc:
        raise InstallerError(f"Download failed for {asset['name']}: HTTP {exc.code}") from exc
    except urllib.error.URLError as exc:
        raise InstallerError(f"Download failed for {asset['name']}: {exc.reason}") from exc


def asset_score(asset_name: str, variant: str, positive_terms: tuple[str, ...], negative_terms: tuple[str, ...] = ()) -> int:
    name = asset_name.lower()
    if not name.endswith(".bin"):
        return -1
    if any(term in name for term in negative_terms):
        return -1
    if "no-battery" in name and "no-battery" not in variant.lower():
        return -1
    if not all(term in name for term in positive_terms):
        return -1

    score = 10
    if variant.lower() in name:
        score += 20
    return score


def best_asset(assets: list[dict[str, Any]],
               variant: str,
               positive_terms: tuple[str, ...],
               negative_terms: tuple[str, ...] = ()) -> dict[str, Any] | None:
    candidates = []
    for asset in assets:
        score = asset_score(asset["name"], variant, positive_terms, negative_terms)
        if score >= 0:
            candidates.append((score, asset["name"], asset))
    if not candidates:
        return None
    candidates.sort(reverse=True)
    return candidates[0][2]


def find_firmware_assets(assets: list[dict[str, Any]], variant: str) -> tuple[list[tuple[dict[str, Any], int]], str]:
    merged = (
        best_asset(assets, variant, ("merged",)),
        best_asset(assets, variant, ("factory",)),
        best_asset(assets, variant, ("full",), ("bootloader", "partitions", "boot_app0")),
    )
    for asset in merged:
        if asset:
            return [(asset, 0)], "merged"

    bootloader = best_asset(assets, variant, ("bootloader",))
    partitions = best_asset(assets, variant, ("partitions",))
    boot_app0 = best_asset(assets, variant, ("boot_app0",))
    app = best_asset(assets, variant, ("firmware",), ("bootloader", "partitions", "boot_app0"))

    if bootloader and partitions and boot_app0 and app:
        return [
            (bootloader, 0x1000),
            (partitions, 0x8000),
            (boot_app0, 0xE000),
            (app, 0x10000),
        ], "split"

    names = ", ".join(asset["name"] for asset in assets) or "none"
    raise InstallerError(
        "Could not find web-installable ESP32 firmware assets. "
        "Attach either a merged/factory .bin, or bootloader.bin, partitions.bin, "
        f"boot_app0.bin, and firmware.bin to the release. Available assets: {names}"
    )


def write_manifest(site_dir: Path,
                   release: dict[str, Any],
                   downloaded: list[tuple[Path, int]]) -> None:
    manifest = {
        "name": "WLED Touch Remote",
        "version": release.get("tag_name", "release"),
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [
            {
                "chipFamily": "ESP32",
                "improv": False,
                "parts": [
                    {
                        "path": f"{FIRMWARE_DIR}/{path.name}",
                        "offset": offset,
                    }
                    for path, offset in downloaded
                ],
            }
        ],
    }
    (site_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def write_release_metadata(site_dir: Path,
                           release: dict[str, Any],
                           mode: str,
                           downloaded: list[tuple[Path, int]]) -> None:
    metadata = {
        "name": release.get("name") or release.get("tag_name", "Latest release"),
        "tag_name": release.get("tag_name"),
        "html_url": release.get("html_url"),
        "published_at": release.get("published_at"),
        "firmware_mode": mode,
        "firmware_files": [
            {
                "path": f"{FIRMWARE_DIR}/{path.name}",
                "offset": offset,
            }
            for path, offset in downloaded
        ],
    }
    (site_dir / "release.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")


def build_installer(repo: str, release_tag: str, site_dir: Path, variant: str, token: str | None) -> None:
    release = request_json(release_url(repo, release_tag), token)
    assets = release.get("assets", [])
    selected, mode = find_firmware_assets(assets, variant)

    firmware_dir = site_dir / FIRMWARE_DIR
    firmware_dir.mkdir(parents=True, exist_ok=True)
    for stale in firmware_dir.glob("*.bin"):
        stale.unlink()

    downloaded: list[tuple[Path, int]] = []
    for asset, offset in selected:
        dest = firmware_dir / asset["name"]
        print(f"Downloading {asset['name']} -> {dest}")
        download_asset(asset, dest, token)
        downloaded.append((dest, offset))

    write_manifest(site_dir, release, downloaded)
    write_release_metadata(site_dir, release, mode, downloaded)
    print(f"Built installer for {release.get('tag_name')} using {mode} firmware.")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=DEFAULT_REPO, help="GitHub repository, e.g. figamore/wled-touch-remote")
    parser.add_argument("--release-tag", default="latest", help='Release tag to use, or "latest"')
    parser.add_argument("--site-dir", type=Path, default=DEFAULT_SITE_DIR)
    parser.add_argument("--variant", default="esp32-cyd-capacitive")
    args = parser.parse_args()

    token = os.environ.get("GITHUB_TOKEN")
    try:
        build_installer(args.repo, args.release_tag, args.site_dir, args.variant, token)
    except InstallerError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
