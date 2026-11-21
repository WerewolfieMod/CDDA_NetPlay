from __future__ import annotations

import logging
import sys

from launcher.config import APP_DIR, LOG_PATH
from launcher.ui import run


def configure_logging() -> None:
    try:
        APP_DIR.mkdir(parents=True, exist_ok=True)
        logging.basicConfig(
            filename=str(LOG_PATH),
            level=logging.INFO,
            format="%(asctime)s %(levelname)s %(name)s: %(message)s",
            encoding="utf-8",
        )
    except OSError:
        logging.basicConfig(level=logging.INFO)
    logging.getLogger("ddamp.launcher").info("launcher %s started", "1.0.0")


def _excepthook(exc_type, exc, tb) -> None:
    logging.getLogger("ddamp.launcher").error(
        "unhandled exception", exc_info=(exc_type, exc, tb)
    )
    try:
        sys.__excepthook__(exc_type, exc, tb)
    except Exception:
        pass


if __name__ == "__main__":
    configure_logging()
    sys.excepthook = _excepthook
    sys.exit(run())
