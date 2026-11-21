"""Game process helpers (launch, running state, LAN address)."""
from __future__ import annotations

import os
import socket
import subprocess
from pathlib import Path
from typing import Sequence

EXE_NAME = "cataclysm-tiles.exe"
DEFAULT_PORT = 25565
_CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0


def game_exe(install_dir: Path) -> Path:
    return Path(install_dir) / EXE_NAME


def is_installed(install_dir: Path) -> bool:
    return game_exe(install_dir).is_file()


def is_game_running() -> bool:
    """Best-effort check for a running cataclysm-tiles.exe.

    Matches by image name, so a build started from another folder also blocks
    an update. Updating under a running game would fail on locked files anyway.
    """
    if os.name != "nt":
        try:
            result = subprocess.run(
                ["pgrep", "-f", EXE_NAME], capture_output=True, text=True, timeout=10
            )
        except (OSError, subprocess.SubprocessError):
            return False
        return bool(result.stdout.strip())
    try:
        result = subprocess.run(
            ["tasklist", "/FI", f"IMAGENAME eq {EXE_NAME}", "/FO", "CSV", "/NH"],
            capture_output=True, text=True, timeout=10,
            creationflags=_CREATE_NO_WINDOW,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return EXE_NAME.lower() in result.stdout.lower()


def launch(install_dir: Path, args: Sequence[str] = ()) -> subprocess.Popen:
    exe = game_exe(install_dir)
    if not exe.is_file():
        raise FileNotFoundError(exe)
    return subprocess.Popen(
        [str(exe), *args],
        cwd=str(install_dir),
        close_fds=True,
    )


def local_ip() -> str:
    """The address other players on the LAN should use."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            return sock.getsockname()[0]
    except OSError:
        try:
            return socket.gethostbyname(socket.gethostname())
        except OSError:
            return "127.0.0.1"


def normalize_address(address: str) -> str:
    address = address.strip()
    if not address:
        raise ValueError("empty address")
    if address.count(":") == 0:
        return f"{address}:{DEFAULT_PORT}"
    return address
