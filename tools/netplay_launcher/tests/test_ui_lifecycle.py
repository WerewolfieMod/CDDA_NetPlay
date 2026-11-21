from __future__ import annotations

import json
import os
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QApplication  # noqa: E402

from launcher.config import Config  # noqa: E402
from launcher.ui import MainWindow  # noqa: E402
from launcher.updater import Release  # noqa: E402


class UiLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication([])

    def _window(self) -> MainWindow:
        root = Path(tempfile.mkdtemp(prefix="ddamp-ui-"))
        config_path = root / "config.json"
        config_path.write_text(
            json.dumps(
                {
                    "install_dir": str(root / "game"),
                    "auto_check": False,
                    "language": "en",
                }
            ),
            encoding="utf-8",
        )
        return MainWindow(Config(config_path))

    def _pump(self, window: MainWindow, deadline_seconds: float = 5.0) -> None:
        deadline = time.time() + deadline_seconds
        while time.time() < deadline:
            self.app.processEvents()
            if window._install_worker is None and window._releases_worker is None:
                return
            time.sleep(0.01)

    def test_install_worker_is_not_destroyed_while_running(self) -> None:
        window = self._window()
        window.latest = Release(
            tag="ddamp-v4.0a-999",
            name="test",
            published_at="",
            asset_name="game-win64.zip",
            asset_url="https://example.invalid/game.zip",
        )

        def fake_download(_release, _progress):
            handle, name = tempfile.mkstemp(suffix=".zip")
            os.close(handle)
            Path(name).write_bytes(b"PK")
            return Path(name), "0" * 64

        def fake_install(*_args, **_kwargs):
            time.sleep(0.15)

        window.updater.download = fake_download  # type: ignore[assignment]
        window.updater.install = fake_install  # type: ignore[assignment]

        with mock.patch("launcher.ui.QMessageBox.information"), \
                mock.patch("launcher.ui.QMessageBox.critical"):
            window.start_install()
            self.assertTrue(window.busy)
            self._pump(window)

        self.assertFalse(window.busy)
        self.assertIsNone(window._install_worker)
        window.close()
        self.app.processEvents()

    def test_release_worker_is_not_destroyed_while_running(self) -> None:
        window = self._window()
        window.updater.releases = lambda force=False: []  # type: ignore[assignment]
        window.check_updates(force=True)
        self.assertIsNotNone(window._releases_worker)
        self._pump(window)
        self.assertIsNone(window._releases_worker)
        window.close()
        self.app.processEvents()


if __name__ == "__main__":
    unittest.main()
