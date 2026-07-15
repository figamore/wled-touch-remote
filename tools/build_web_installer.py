#!/usr/bin/env python3
"""Assemble the GitHub Pages installer for stable and preview firmware.

The stable remote is downloaded from a GitHub release. The bidirectional
remote and its matching WLED build are produced by the Pages workflow and
passed in as local binaries. Keeping every artifact in the Pages bundle avoids
cross-origin Web Serial issues and makes the preview reproducible.
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
INSTALLER_SCREENSHOTS = (
    ("wled-touch-remote-power.png", "wled-touch-remote-power.png"),
    ("wled-touch-remote-fx.png", "wled-touch-remote-fx.png"),
    ("cyd-full-espnow/cyd-full-palettes.png", "wled-touch-remote-palettes.png"),
    ("wled-presets-color-palette.png", "wled-presets-color-palette.png"),
)


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


def asset_score(
    asset_name: str,
    variant: str,
    positive_terms: tuple[str, ...],
    negative_terms: tuple[str, ...] = (),
) -> int:
    name = asset_name.lower()
    if not name.endswith(".bin") or any(term in name for term in negative_terms):
        return -1
    if "no-battery" in name and "no-battery" not in variant.lower():
        return -1
    if not all(term in name for term in positive_terms):
        return -1

    score = 10
    if variant.lower() in name:
        score += 20
    return score


def best_asset(
    assets: list[dict[str, Any]],
    variant: str,
    positive_terms: tuple[str, ...],
    negative_terms: tuple[str, ...] = (),
) -> dict[str, Any] | None:
    candidates = []
    for asset in assets:
        score = asset_score(asset["name"], variant, positive_terms, negative_terms)
        if score >= 0:
            candidates.append((score, asset["name"], asset))
    if not candidates:
        return None
    candidates.sort(reverse=True)
    return candidates[0][2]


def find_firmware_assets(
    assets: list[dict[str, Any]], variant: str
) -> tuple[list[tuple[dict[str, Any], int]], str]:
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


def write_manifest(
    site_dir: Path,
    filename: str,
    name: str,
    version: str,
    parts: list[tuple[str, int]],
) -> None:
    manifest = {
        "name": name,
        "version": version,
        "new_install_prompt_erase": False,
        "new_install_improv_wait_time": 0,
        "builds": [
            {
                "chipFamily": "ESP32",
                "improv": False,
                "parts": [{"path": path, "offset": offset} for path, offset in parts],
            }
        ],
    }
    (site_dir / filename).write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def copy_local_binary(source: Path, destination: Path, description: str) -> None:
    if not source.is_file():
        raise InstallerError(f"{description} not found: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def copy_installer_screenshots(source_dir: Path, site_dir: Path) -> None:
    destination_dir = site_dir / "screenshots"
    if destination_dir.exists():
        shutil.rmtree(destination_dir)
    destination_dir.mkdir(parents=True)
    for source_name, destination_name in INSTALLER_SCREENSHOTS:
        source = source_dir / source_name
        if not source.is_file():
            raise InstallerError(f"Installer screenshot not found: {source}")
        shutil.copy2(source, destination_dir / destination_name)


def build_installer(
    repo: str,
    release_tag: str,
    site_dir: Path,
    variant: str,
    advanced_firmware: Path | None,
    advanced_version: str,
    advanced_source_url: str,
    wled_firmware: Path | None,
    wled_version: str,
    wled_source_url: str,
    assets_dir: Path,
    token: str | None,
) -> None:
    release = request_json(release_url(repo, release_tag), token)
    selected, stable_mode = find_firmware_assets(release.get("assets", []), variant)

    firmware_dir = site_dir / FIRMWARE_DIR
    if firmware_dir.exists():
        shutil.rmtree(firmware_dir)
    (firmware_dir / "stable").mkdir(parents=True)

    stable_parts: list[tuple[str, int]] = []
    stable_files: list[dict[str, Any]] = []
    for asset, offset in selected:
        dest = firmware_dir / "stable" / asset["name"]
        print(f"Downloading {asset['name']} -> {dest}")
        download_asset(asset, dest, token)
        relative = dest.relative_to(site_dir).as_posix()
        stable_parts.append((relative, offset))
        stable_files.append({"path": relative, "offset": offset})

    stable_version = release.get("tag_name", "release")
    write_manifest(
        site_dir,
        "manifest-stable.json",
        "WLED Touch Remote — Standard WLED",
        stable_version,
        stable_parts,
    )
    # Preserve old links/bookmarks: the unqualified manifest remains the stable track.
    shutil.copy2(site_dir / "manifest-stable.json", site_dir / "manifest.json")

    metadata: dict[str, Any] = {
        "stable": {
            "name": release.get("name") or stable_version,
            "version": stable_version,
            "html_url": release.get("html_url"),
            "published_at": release.get("published_at"),
            "firmware_mode": stable_mode,
            "firmware_files": stable_files,
        }
    }

    if advanced_firmware is not None:
        advanced_dest = firmware_dir / "advanced" / "wled-touch-remote-bidirectional.bin"
        copy_local_binary(advanced_firmware, advanced_dest, "Advanced remote firmware")
        advanced_path = advanced_dest.relative_to(site_dir).as_posix()
        write_manifest(
            site_dir,
            "manifest-advanced.json",
            "WLED Touch Remote — Bidirectional preview",
            advanced_version,
            [(advanced_path, 0)],
        )
        metadata["advanced"] = {
            "name": "Bidirectional ESP-NOW preview",
            "version": advanced_version,
            "source_url": advanced_source_url,
            "firmware_file": advanced_path,
        }

    if wled_firmware is not None:
        if advanced_firmware is None:
            raise InstallerError("--wled-firmware requires --advanced-firmware")
        wled_dest = firmware_dir / "wled" / "WLED_ESP32_bidirectional_OTA.bin"
        copy_local_binary(wled_firmware, wled_dest, "Bidirectional WLED firmware")
        metadata["wled"] = {
            "name": "WLED ESP32 bidirectional ESP-NOW preview",
            "version": wled_version,
            "source_url": wled_source_url,
            "firmware_file": wled_dest.relative_to(site_dir).as_posix(),
            "format": "generic ESP32 OTA/application binary",
        }

    (site_dir / "installer.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    # Older installer JavaScript reads release.json.
    (site_dir / "release.json").write_text(json.dumps(metadata["stable"], indent=2) + "\n", encoding="utf-8")
    copy_installer_screenshots(assets_dir, site_dir)
    print(f"Built stable installer for {stable_version} using {stable_mode} firmware.")
    if "advanced" in metadata:
        print(f"Built advanced installer for {advanced_version} with WLED {wled_version}.")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=DEFAULT_REPO, help="GitHub repository, e.g. figamore/wled-touch-remote")
    parser.add_argument("--release-tag", default="latest", help='Stable release tag to use, or "latest"')
    parser.add_argument("--site-dir", type=Path, default=DEFAULT_SITE_DIR)
    parser.add_argument("--assets-dir", type=Path, default=Path("screenshots"))
    parser.add_argument("--variant", default="esp32-cyd")
    parser.add_argument("--advanced-firmware", type=Path, required=True)
    parser.add_argument("--advanced-version", default="feature preview")
    parser.add_argument(
        "--advanced-source-url",
        default="https://github.com/figamore/wled-touch-remote/tree/feature/bidirectional-api",
    )
    parser.add_argument("--wled-firmware", type=Path, required=True)
    parser.add_argument("--wled-version", default="feature/bidirectional-espnow")
    parser.add_argument(
        "--wled-source-url",
        default="https://github.com/figamore/WLED/tree/feature/bidirectional-espnow",
    )
    args = parser.parse_args()

    try:
        build_installer(
            args.repo,
            args.release_tag,
            args.site_dir,
            args.variant,
            args.advanced_firmware,
            args.advanced_version,
            args.advanced_source_url,
            args.wled_firmware,
            args.wled_version,
            args.wled_source_url,
            args.assets_dir,
            os.environ.get("GITHUB_TOKEN"),
        )
    except InstallerError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
