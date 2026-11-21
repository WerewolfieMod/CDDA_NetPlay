from __future__ import annotations

import json
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

from launcher.updater import (
    LauncherError,
    Release,
    Updater,
    is_newer,
    parse_tag,
    tag_sort_key,
)


def make_release(tag: str, asset: str = "Cataclysm-DDA-NetPlay-win64.zip") -> Release:
    return Release(
        tag=tag,
        name=tag,
        published_at="2026-11-21T00:00:00Z",
        asset_name=asset,
        asset_url=f"https://example.invalid/{asset}",
        asset_size=1234,
        asset_digest="sha256:" + "0" * 64,
    )


def make_zip(path: Path, marker: str) -> None:
    source = Path(tempfile.mkdtemp(prefix="ddamp-src-"))
    (source / "cataclysm-tiles.exe").write_bytes(b"MZ" + marker.encode("utf-8"))
    (source / "data").mkdir()
    (source / "data" / "core.json").write_text(marker, encoding="utf-8")
    with zipfile.ZipFile(path, "w") as archive:
        for file in source.rglob("*"):
            if file.is_file():
                archive.write(file, file.relative_to(source))


class TagTests(unittest.TestCase):
    def test_parse_tag(self) -> None:
        self.assertEqual(parse_tag("ddamp-v4.0a-223"), (4, 0, "a", 223))
        self.assertEqual(parse_tag("ddamp-v4.0-7"), (4, 0, "", 7))
        self.assertIsNone(parse_tag("experimental-2026"))
        self.assertIsNone(parse_tag("ddamp-v4.0"))

    def test_ordering(self) -> None:
        self.assertGreater(tag_sort_key("ddamp-v4.0a-223"), tag_sort_key("ddamp-v4.0a-99"))
        self.assertGreater(tag_sort_key("ddamp-v4.1a-1"), tag_sort_key("ddamp-v4.0a-999"))
        self.assertTrue(is_newer("ddamp-v4.0a-2", "ddamp-v4.0a-1"))
        self.assertFalse(is_newer("ddamp-v4.0a-1", "ddamp-v4.0a-1"))
        self.assertTrue(is_newer("ddamp-v4.0a-1", None))


class ReleaseParsingTests(unittest.TestCase):
    def test_picks_windows_asset(self) -> None:
        release = Release.from_json(
            {
                "tag_name": "ddamp-v4.0a-10",
                "name": "Build 10",
                "draft": False,
                "assets": [
                    {"name": "source.zip", "browser_download_url": "https://x/source.zip"},
                    {
                        "name": "Cataclysm-DDA-NetPlay-v4.0a-10-win64.zip",
                        "browser_download_url": "https://x/game.zip",
                        "size": 42,
                        "digest": "sha256:abc",
                    },
                ],
            }
        )
        self.assertIsNotNone(release)
        assert release is not None
        self.assertEqual(release.asset_url, "https://x/game.zip")
        self.assertEqual(release.asset_size, 42)
        self.assertEqual(release.asset_digest, "sha256:abc")

    def test_ignores_foreign_or_draft_releases(self) -> None:
        base = {
            "tag_name": "ddamp-v4.0a-10",
            "assets": [
                {"name": "game-win64.zip", "browser_download_url": "https://x/game.zip"}
            ],
        }
        self.assertIsNone(Release.from_json({**base, "tag_name": "foo"}))
        self.assertIsNone(Release.from_json({**base, "draft": True}))
        self.assertIsNone(Release.from_json({**base, "assets": []}))

    def test_release_list_is_sorted(self) -> None:
        updater = Updater(Path(tempfile.mkdtemp(prefix="ddamp-cache-")))
        payload = [
            {"tag_name": "ddamp-v4.0a-2", "assets": [{"name": "g-win64.zip", "browser_download_url": "u"}]},
            {"tag_name": "ddamp-v4.0a-10", "assets": [{"name": "g-win64.zip", "browser_download_url": "u"}]},
        ]
        with mock.patch.object(updater, "_releases_payload", return_value=payload):
            tags = [release.tag for release in updater.releases(force=True)]
        self.assertEqual(tags, ["ddamp-v4.0a-10", "ddamp-v4.0a-2"])


class InstallTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="ddamp-install-"))
        self.install = self.root / "game"
        self.updater = Updater(self.root / "cache")

    def test_install_update_and_rollback(self) -> None:
        first = self.root / "first.zip"
        second = self.root / "second.zip"
        make_zip(first, "first")
        make_zip(second, "second")

        self.updater.install(first, self.install, make_release("ddamp-v4.0a-1"), sha256="a" * 64)
        self.assertTrue((self.install / "cataclysm-tiles.exe").is_file())
        version = json.loads((self.install / "version.json").read_text(encoding="utf-8"))
        self.assertEqual(version["tag"], "ddamp-v4.0a-1")

        (self.install / "save").mkdir()
        (self.install / "save" / "character.gsav").write_text("survivor", encoding="utf-8")
        (self.install / "config").mkdir()
        (self.install / "config" / "options.json").write_text("{}", encoding="utf-8")

        self.updater.install(second, self.install, make_release("ddamp-v4.0a-2"), sha256="b" * 64)
        version = json.loads((self.install / "version.json").read_text(encoding="utf-8"))
        self.assertEqual(version["tag"], "ddamp-v4.0a-2")
        self.assertTrue((self.install / "save" / "character.gsav").is_file())
        self.assertTrue((self.install / "config" / "options.json").is_file())
        previous = self.root / "game.previous"
        self.assertTrue(previous.is_dir())
        self.assertEqual(
            json.loads((previous / "version.json").read_text(encoding="utf-8"))["tag"],
            "ddamp-v4.0a-1",
        )

        self.updater.rollback(self.install)
        version = json.loads((self.install / "version.json").read_text(encoding="utf-8"))
        self.assertEqual(version["tag"], "ddamp-v4.0a-1")
        self.assertFalse(previous.exists())
        self.assertTrue((self.install / "save" / "character.gsav").is_file())

    def test_refuses_foreign_directory(self) -> None:
        self.install.mkdir(parents=True)
        (self.install / "notes.txt").write_text("not a game", encoding="utf-8")
        archive = self.root / "build.zip"
        make_zip(archive, "first")
        with self.assertRaises(LauncherError):
            self.updater.install(archive, self.install, make_release("ddamp-v4.0a-1"))

    def test_refuses_archive_without_executable(self) -> None:
        archive = self.root / "bad.zip"
        with zipfile.ZipFile(archive, "w") as zf:
            zf.writestr("data/core.json", "{}")
        with self.assertRaises(LauncherError):
            self.updater.install(archive, self.install, make_release("ddamp-v4.0a-1"))


class CacheTests(unittest.TestCase):
    class FakeResponse:
        def __init__(self, payload: list) -> None:
            self.payload = payload

        def __enter__(self):
            return self

        def __exit__(self, *args) -> bool:
            return False

        def read(self) -> bytes:
            return json.dumps(self.payload).encode("utf-8")

    def test_empty_cache_is_not_trusted(self) -> None:
        root = Path(tempfile.mkdtemp(prefix="ddamp-cache-"))
        updater = Updater(root)
        updater.cache_path.write_text(
            json.dumps({"fetched_at": __import__("time").time(), "releases": []}),
            encoding="utf-8",
        )
        fresh = [{"tag_name": "ddamp-v4.0a-223", "assets": []}]
        with mock.patch(
            "launcher.updater.urllib.request.urlopen",
            return_value=self.FakeResponse(fresh),
        ):
            self.assertEqual(updater._releases_payload(False), fresh)

    def test_fresh_cache_avoids_network(self) -> None:
        root = Path(tempfile.mkdtemp(prefix="ddamp-cache-"))
        updater = Updater(root)
        cached = [{"tag_name": "ddamp-v4.0a-222", "assets": []}]
        updater.cache_path.write_text(
            json.dumps({"fetched_at": __import__("time").time(), "releases": cached}),
            encoding="utf-8",
        )
        with mock.patch(
            "launcher.updater.urllib.request.urlopen",
            side_effect=AssertionError("network must not be used"),
        ):
            self.assertEqual(updater._releases_payload(False), cached)


if __name__ == "__main__":
    unittest.main()
