"""GitHub Releases based updater for DDA NetPlay.

The launcher never builds the game: it downloads the Windows ZIP attached to a
`ddamp-v*` GitHub release, verifies it and swaps the install directory while
keeping the player's saves, config and mods.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Optional

REPO = "WerewolfieMod/CDDA_NetPlay"
DEFAULT_API = f"https://api.github.com/repos/{REPO}"
API_BASE = os.environ.get("DDAMP_LAUNCHER_API", DEFAULT_API).rstrip("/")
USER_AGENT = "DDAMP-Launcher/1.0 (+https://github.com/WerewolfieMod/CDDA_NetPlay)"
CACHE_SECONDS = 600
EXE_NAME = "cataclysm-tiles.exe"
VERSION_FILE = "version.json"
# Directories that belong to the player, not to the build.
USER_DATA_DIRS = ("save", "config", "mods")

TAG_RE = re.compile(r"^ddamp-v(\d+)\.(\d+)([a-z]?)-(\d+)$")

Progress = Callable[[int, str, dict[str, Any]], None]


class LauncherError(RuntimeError):
    """A problem worth showing to the player as-is."""


def noop_progress(_percent: int, _stage: str, _details: dict[str, Any]) -> None:
    pass


def parse_tag(tag: str) -> Optional[tuple[int, int, str, int]]:
    match = TAG_RE.match(tag or "")
    if not match:
        return None
    major, minor, letter, build = match.groups()
    return int(major), int(minor), letter, int(build)


def tag_sort_key(tag: str) -> tuple[int, int, str, int]:
    parsed = parse_tag(tag)
    return parsed if parsed is not None else (-1, -1, "", -1)


def is_newer(remote_tag: str, local_tag: str | None) -> bool:
    if not local_tag:
        return True
    return tag_sort_key(remote_tag) > tag_sort_key(local_tag)


def format_size(size: int) -> str:
    value = float(size or 0)
    for unit in ("B", "KB", "MB", "GB"):
        if value < 1024 or unit == "GB":
            return f"{value:.1f} {unit}"
        value /= 1024
    return f"{value:.1f} GB"


def _pick_asset(assets: list[dict[str, Any]]) -> Optional[dict[str, Any]]:
    zips = [a for a in assets if str(a.get("name", "")).lower().endswith(".zip")]
    windows = [a for a in zips
               if "win64" in a.get("name", "").lower()
               or "windows" in a.get("name", "").lower()]
    chosen = windows or zips
    return chosen[0] if chosen else None


@dataclass
class Release:
    tag: str
    name: str
    published_at: str
    asset_name: str
    asset_url: str
    asset_size: int = 0
    asset_digest: str = ""
    body: str = ""
    html_url: str = ""

    @property
    def sort_key(self) -> tuple[int, int, str, int]:
        return tag_sort_key(self.tag)

    def version_doc(self, sha256: str = "") -> dict[str, Any]:
        return {
            "tag": self.tag,
            "name": self.name,
            "published_at": self.published_at,
            "asset": self.asset_name,
            "asset_url": self.asset_url,
            "sha256": sha256 or self.asset_digest.split(":")[-1],
            "source": f"https://github.com/{REPO}/releases/tag/{self.tag}",
            "installed_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }

    @classmethod
    def from_json(cls, data: dict[str, Any]) -> Optional["Release"]:
        if not isinstance(data, dict) or data.get("draft"):
            return None
        tag = str(data.get("tag_name") or "")
        if parse_tag(tag) is None:
            return None
        asset = _pick_asset(data.get("assets") or [])
        if asset is None or not asset.get("browser_download_url"):
            return None
        return cls(
            tag=tag,
            name=str(data.get("name") or tag),
            published_at=str(data.get("published_at") or data.get("created_at") or ""),
            asset_name=str(asset.get("name") or ""),
            asset_url=str(asset.get("browser_download_url") or ""),
            asset_size=int(asset.get("size") or 0),
            asset_digest=str(asset.get("digest") or ""),
            body=str(data.get("body") or ""),
            html_url=str(data.get("html_url") or ""),
        )


class Updater:
    def __init__(self, cache_dir: Path, api_base: str = API_BASE) -> None:
        self.cache_dir = Path(cache_dir)
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        self.api_base = api_base.rstrip("/")
        self.cache_path = self.cache_dir / "releases.json"

    # ---------------------------------------------------------------- network

    def releases(self, force: bool = False) -> list[Release]:
        payload = self._releases_payload(force)
        result: list[Release] = []
        for item in payload:
            release = Release.from_json(item)
            if release is not None:
                result.append(release)
        result.sort(key=lambda r: r.sort_key, reverse=True)
        return result

    def latest(self, force: bool = False) -> Release:
        releases = self.releases(force)
        if not releases:
            raise LauncherError(
                "No game builds were found. Publish a release with a Windows ZIP asset first."
            )
        return releases[0]

    def _releases_payload(self, force: bool) -> list[dict[str, Any]]:
        cache = self._read_cache()
        if not force and cache and time.time() - float(cache.get("fetched_at") or 0) < CACHE_SECONDS:
            return cache.get("releases") or []

        url = f"{self.api_base}/releases?per_page=30"
        request = urllib.request.Request(
            url,
            headers={"User-Agent": USER_AGENT, "Accept": "application/vnd.github+json"},
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                payload = json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as error:
            if cache and cache.get("releases"):
                return cache["releases"]
            if error.code == 403:
                raise LauncherError(
                    "GitHub API rate limit reached. Try again in a few minutes."
                ) from error
            raise LauncherError(f"GitHub returned HTTP {error.code}.") from error
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            if cache and cache.get("releases"):
                return cache["releases"]
            raise LauncherError(f"Cannot reach GitHub: {error}") from error

        if not isinstance(payload, list):
            raise LauncherError("Unexpected answer from GitHub.")
        self._write_cache(payload)
        return payload

    def _read_cache(self) -> Optional[dict[str, Any]]:
        try:
            data = json.loads(self.cache_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
        return data if isinstance(data, dict) else None

    def _write_cache(self, releases: list[dict[str, Any]]) -> None:
        tmp = self.cache_path.with_suffix(".tmp")
        try:
            tmp.write_text(
                json.dumps({"fetched_at": time.time(), "releases": releases}),
                encoding="utf-8",
            )
            os.replace(tmp, self.cache_path)
        except OSError:
            pass

    # --------------------------------------------------------------- download

    def download(self, release: Release, progress: Progress = noop_progress) -> tuple[Path, str]:
        if not release.asset_url:
            raise LauncherError("This build has no downloadable ZIP asset.")

        fd, name = tempfile.mkstemp(prefix="ddamp-", suffix=".zip")
        os.close(fd)
        target = Path(name)
        request = urllib.request.Request(
            release.asset_url,
            headers={"User-Agent": USER_AGENT, "Accept": "application/octet-stream"},
        )
        digest = hashlib.sha256()
        read_total = 0
        last_percent = -1
        last_report = 0
        try:
            with urllib.request.urlopen(request, timeout=60) as response, \
                    open(target, "wb") as file_obj:
                total = int(response.headers.get("Content-Length") or release.asset_size or 0)
                while True:
                    chunk = response.read(64 * 1024)
                    if not chunk:
                        break
                    file_obj.write(chunk)
                    digest.update(chunk)
                    read_total += len(chunk)
                    if total:
                        percent = int(read_total * 100 / total)
                        # One signal per percent is plenty for a progress bar;
                        # a 120 MB build would otherwise queue ~1900 events.
                        if percent != last_percent or read_total >= total:
                            last_percent = percent
                            progress(
                                percent,
                                "download",
                                {"read": read_total, "total": total},
                            )
                    elif read_total - last_report >= 1024 * 1024:
                        last_report = read_total
                        progress(0, "download", {"read": read_total, "total": 0})
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            target.unlink(missing_ok=True)
            raise LauncherError(f"Download failed: {error}") from error

        actual = digest.hexdigest()
        expected = release.asset_digest.split(":")[-1].lower() if release.asset_digest else ""
        if expected and actual != expected:
            target.unlink(missing_ok=True)
            raise LauncherError("The downloaded archive is corrupted (SHA256 mismatch).")
        progress(100, "verify", {})
        return target, actual

    # ---------------------------------------------------------------- install

    def install(
        self,
        zip_path: Path,
        install_dir: Path,
        release: Release,
        keep_previous: bool = True,
        sha256: str = "",
        progress: Progress = noop_progress,
    ) -> None:
        install_dir = Path(install_dir)
        parent = install_dir.parent
        parent.mkdir(parents=True, exist_ok=True)

        staging = Path(tempfile.mkdtemp(prefix=".ddamp-staging-", dir=str(parent)))
        try:
            progress(0, "extract", {})
            with zipfile.ZipFile(zip_path) as archive:
                self._safe_extract(archive, staging, progress)

            if not (staging / EXE_NAME).is_file():
                raise LauncherError(
                    f"The archive does not contain {EXE_NAME}; it does not look like a DDA NetPlay build."
                )

            (staging / VERSION_FILE).write_text(
                json.dumps(release.version_doc(sha256), indent=2, ensure_ascii=False),
                encoding="utf-8",
            )

            previous = parent / f"{install_dir.name}.previous"
            has_install = install_dir.is_dir() and any(install_dir.iterdir())

            if has_install and not (install_dir / EXE_NAME).is_file() \
                    and not (install_dir / VERSION_FILE).is_file():
                raise LauncherError(
                    "The install folder is not empty and does not look like a DDA NetPlay build. "
                    "Choose an empty folder or an existing DDA NetPlay install."
                )

            if has_install:
                if previous.exists():
                    shutil.rmtree(previous)
                progress(72, "backup", {})
                install_dir.rename(previous)
                try:
                    staging.rename(install_dir)
                except OSError:
                    previous.rename(install_dir)
                    raise

                progress(80, "restore", {})
                self._restore_user_data(previous, install_dir)
                if not keep_previous:
                    shutil.rmtree(previous, ignore_errors=True)
            else:
                if install_dir.exists():
                    install_dir.rmdir()
                staging.rename(install_dir)

            progress(100, "done", {})
        finally:
            if staging.exists():
                shutil.rmtree(staging, ignore_errors=True)

    @staticmethod
    def _safe_extract(archive: zipfile.ZipFile, destination: Path, progress: Progress) -> None:
        root = destination.resolve()
        infos = archive.infolist()
        total = max(1, len(infos))
        for index, info in enumerate(infos, 1):
            target = (destination / info.filename).resolve()
            if target != root and root not in target.parents:
                raise LauncherError(f"Archive entry escapes the install directory: {info.filename}")
            archive.extract(info, destination)
            if index % 50 == 0 or index == total:
                progress(int(index * 70 / total), "extract", {"done": index, "total": total})

    @staticmethod
    def _restore_user_data(previous: Path, install_dir: Path) -> None:
        for name in USER_DATA_DIRS:
            source = previous / name
            if not source.is_dir():
                continue
            target = install_dir / name
            shutil.copytree(source, target, dirs_exist_ok=True)

    # ------------------------------------------------------------------ local

    @staticmethod
    def local_version(install_dir: Path) -> Optional[dict[str, Any]]:
        path = Path(install_dir) / VERSION_FILE
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None
        return data if isinstance(data, dict) else None

    @staticmethod
    def has_previous(install_dir: Path) -> bool:
        install_dir = Path(install_dir)
        return (install_dir.parent / f"{install_dir.name}.previous").is_dir()

    @staticmethod
    def rollback(install_dir: Path, progress: Progress = noop_progress) -> None:
        install_dir = Path(install_dir)
        previous = install_dir.parent / f"{install_dir.name}.previous"
        if not previous.is_dir():
            raise LauncherError("There is no previous build to restore.")
        failed = install_dir.parent / f"{install_dir.name}.failed"
        if failed.exists():
            shutil.rmtree(failed)
        progress(10, "rollback", {})
        if install_dir.exists():
            install_dir.rename(failed)
        try:
            previous.rename(install_dir)
        except OSError:
            if failed.exists():
                failed.rename(install_dir)
            raise
        shutil.rmtree(failed, ignore_errors=True)
        progress(100, "done", {})
