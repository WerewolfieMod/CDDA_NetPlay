"""Configuration storage for the DDA NetPlay launcher.

Everything lives in %APPDATA%\\DDAMP Launcher so an install can be moved or
deleted without losing the launcher's own settings and logs.
"""
from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path
from typing import Any


APP_NAME = "DDA NetPlay Launcher"
APP_DIR = Path(os.environ.get("APPDATA") or Path.home()) / "DDAMP Launcher"
CONFIG_PATH = APP_DIR / "config.json"
LOG_PATH = APP_DIR / "launcher.log"

DEFAULTS: dict[str, Any] = {
    "install_dir": str(Path.home() / "Games" / "DDA NetPlay"),
    "last_join": "",
    "keep_previous": True,
    "auto_check": True,
    "language": "auto",
}


class Config:
    def __init__(self, path: Path = CONFIG_PATH) -> None:
        self.path = path
        self.data: dict[str, Any] = dict(DEFAULTS)
        self.load()

    def load(self) -> None:
        try:
            raw = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return
        if isinstance(raw, dict):
            for key in DEFAULTS:
                if key in raw:
                    self.data[key] = raw[key]

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(prefix="config-", suffix=".json", dir=str(self.path.parent))
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as fh:
                json.dump(self.data, fh, indent=2, ensure_ascii=False)
            os.replace(tmp, self.path)
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)

    def get(self, key: str, default: Any = None) -> Any:
        return self.data.get(key, DEFAULTS.get(key, default))

    def set(self, key: str, value: Any, save: bool = True) -> None:
        self.data[key] = value
        if save:
            self.save()
