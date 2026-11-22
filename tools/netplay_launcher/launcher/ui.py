"""PySide6 UI for the DDA NetPlay launcher."""
from __future__ import annotations

import logging
import os
import sys
from pathlib import Path

from PySide6.QtCore import QThread, QTimer, QUrl, Signal
from PySide6.QtGui import QColor, QDesktopServices, QFont, QPalette
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from . import __version__
from .config import APP_DIR, Config
from .game import (
    DEFAULT_PORT,
    is_game_running,
    is_installed,
    launch,
    local_ip,
    normalize_address,
)
from .i18n import Translator, detect_language
from .updater import Release, Updater, format_size, is_newer

REPO_URL = "https://github.com/WerewolfieMod/CDDA_NetPlay"
logger = logging.getLogger("ddamp.launcher.ui")


class ReleasesWorker(QThread):
    ok = Signal(object)
    fail = Signal(str)

    def __init__(self, updater: Updater, force: bool) -> None:
        super().__init__()
        self.updater = updater
        self.force = force

    def run(self) -> None:
        try:
            self.ok.emit(self.updater.releases(force=self.force))
        except Exception as error:  # noqa: BLE001 - shown to the player
            logger.exception("release check failed")
            self.fail.emit(str(error))


class InstallWorker(QThread):
    progress = Signal(int, str, object)
    ok = Signal(object)
    fail = Signal(str)

    def __init__(self, updater: Updater, release: Release,
                 install_dir: Path, keep_previous: bool) -> None:
        super().__init__()
        self.updater = updater
        self.release = release
        self.install_dir = install_dir
        self.keep_previous = keep_previous

    def run(self) -> None:
        temp: Path | None = None
        try:
            temp, sha256 = self.updater.download(self.release, self.progress.emit)
            self.updater.install(
                temp,
                self.install_dir,
                self.release,
                keep_previous=self.keep_previous,
                sha256=sha256,
                progress=self.progress.emit,
            )
            self.ok.emit(self.release)
        except Exception as error:  # noqa: BLE001 - shown to the player
            logger.exception("install failed")
            self.fail.emit(str(error))
        finally:
            if temp is not None:
                Path(temp).unlink(missing_ok=True)


class JoinDialog(QDialog):
    def __init__(self, parent: QWidget, tr: Translator, address: str) -> None:
        super().__init__(parent)
        self.tr = tr
        self.setWindowTitle(tr("join_title"))
        self.setModal(True)
        layout = QVBoxLayout(self)
        form = QFormLayout()
        self.address_edit = QLineEdit(address)
        self.address_edit.setPlaceholderText(tr("join_hint"))
        form.addRow(tr("join_label"), self.address_edit)
        layout.addLayout(form)
        hint = QLabel(tr("join_hint"))
        hint.setStyleSheet("color: #8a8f98;")
        layout.addWidget(hint)
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText(tr("join_ok"))
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText(tr("cancel"))
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self.address_edit.setFocus()

    def value(self) -> str:
        return self.address_edit.text().strip()


class MainWindow(QMainWindow):
    def __init__(self, config: Config) -> None:
        super().__init__()
        self.config = config
        self.updater = Updater(APP_DIR / "cache")
        self.tr = Translator(detect_language(str(config.get("language"))))
        self.latest: Release | None = None
        self.releases: list[Release] = []
        self.busy = False
        self.game_running = False
        self._releases_worker: ReleasesWorker | None = None
        self._install_worker: InstallWorker | None = None
        self._check_error = ""
        self._no_builds = False

        self._build_ui()
        self._retranslate()
        self._refresh()

        self._poll_timer = QTimer(self)
        self._poll_timer.timeout.connect(self._poll)
        self._poll_timer.start(3000)

        if bool(config.get("auto_check")):
            QTimer.singleShot(400, lambda: self.check_updates(force=False))

    # ---------------------------------------------------------------- layout

    def _build_ui(self) -> None:
        central = QWidget(self)
        layout = QVBoxLayout(central)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(10)

        self.title_label = QLabel()
        title_font = QFont()
        title_font.setPointSize(18)
        title_font.setBold(True)
        self.title_label.setFont(title_font)
        self.subtitle_label = QLabel()
        self.subtitle_label.setStyleSheet("color: #8a8f98;")
        layout.addWidget(self.title_label)
        layout.addWidget(self.subtitle_label)

        self.game_group = QGroupBox()
        game_layout = QVBoxLayout(self.game_group)
        path_row = QHBoxLayout()
        self.dir_label = QLabel()
        self.dir_edit = QLineEdit(str(self.config.get("install_dir")))
        self.dir_edit.editingFinished.connect(self._on_dir_changed)
        self.browse_btn = QPushButton()
        self.browse_btn.clicked.connect(self._browse)
        self.open_btn = QPushButton()
        self.open_btn.clicked.connect(self._open_folder)
        path_row.addWidget(self.dir_label)
        path_row.addWidget(self.dir_edit, 1)
        path_row.addWidget(self.browse_btn)
        path_row.addWidget(self.open_btn)
        game_layout.addLayout(path_row)
        self.installed_label = QLabel()
        self.installed_label.setStyleSheet("font-weight: 600;")
        self.latest_label = QLabel()
        game_layout.addWidget(self.installed_label)
        game_layout.addWidget(self.latest_label)
        layout.addWidget(self.game_group)

        self.status_label = QLabel()
        self.status_label.setWordWrap(True)
        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        self.progress.setVisible(False)
        layout.addWidget(self.status_label)
        layout.addWidget(self.progress)

        action_row = QHBoxLayout()
        self.check_btn = QPushButton()
        self.check_btn.clicked.connect(lambda: self.check_updates(force=True))
        self.install_btn = QPushButton()
        self.install_btn.clicked.connect(self.start_install)
        self.rollback_btn = QPushButton()
        self.rollback_btn.clicked.connect(self.rollback)
        action_row.addWidget(self.check_btn)
        action_row.addWidget(self.install_btn, 2)
        action_row.addWidget(self.rollback_btn)
        layout.addLayout(action_row)

        play_row = QHBoxLayout()
        self.play_btn = QPushButton()
        self.play_btn.clicked.connect(self.play)
        self.host_btn = QPushButton()
        self.host_btn.clicked.connect(self.host)
        self.join_btn = QPushButton()
        self.join_btn.clicked.connect(self.join)
        play_row.addWidget(self.play_btn, 2)
        play_row.addWidget(self.host_btn)
        play_row.addWidget(self.join_btn)
        layout.addLayout(play_row)

        layout.addStretch(1)

        bottom = QHBoxLayout()
        self.repo_btn = QPushButton()
        self.repo_btn.setFlat(True)
        self.repo_btn.clicked.connect(
            lambda: QDesktopServices.openUrl(QUrl(REPO_URL))
        )
        self.lang_label = QLabel()
        self.lang_combo = QComboBox()
        self.lang_combo.addItem("", "auto")
        self.lang_combo.addItem("English", "en")
        self.lang_combo.addItem("Русский", "ru")
        self._select_language()
        self.lang_combo.currentIndexChanged.connect(self._on_language_changed)
        self.about_btn = QPushButton()
        self.about_btn.clicked.connect(self.about)
        bottom.addWidget(self.repo_btn)
        bottom.addStretch(1)
        bottom.addWidget(self.lang_label)
        bottom.addWidget(self.lang_combo)
        bottom.addWidget(self.about_btn)
        layout.addLayout(bottom)

        self.setCentralWidget(central)
        self.resize(780, 520)

    def _select_language(self) -> None:
        configured = str(self.config.get("language") or "auto")
        index = self.lang_combo.findData(configured)
        self.lang_combo.blockSignals(True)
        self.lang_combo.setCurrentIndex(max(0, index))
        self.lang_combo.blockSignals(False)

    def _retranslate(self) -> None:
        self.setWindowTitle(self.tr("app_title"))
        self.title_label.setText("DDA NetPlay")
        self.subtitle_label.setText(self.tr("subtitle"))
        self.game_group.setTitle(self.tr("group_game"))
        self.dir_label.setText(self.tr("install_dir"))
        self.browse_btn.setText(self.tr("browse"))
        self.open_btn.setText(self.tr("open_folder"))
        self.check_btn.setText(self.tr("btn_check"))
        self.rollback_btn.setText(self.tr("btn_rollback"))
        self.play_btn.setText(self.tr("btn_play"))
        self.host_btn.setText(self.tr("btn_host"))
        self.join_btn.setText(self.tr("btn_join"))
        self.repo_btn.setText(self.tr("repo_link"))
        self.about_btn.setText(self.tr("menu_about"))
        self.lang_label.setText(self.tr("menu_language"))
        self.lang_combo.setItemText(0, self.tr("lang_auto"))
        self._refresh()

    # ----------------------------------------------------------------- state

    def _install_dir(self) -> Path:
        return Path(self.dir_edit.text().strip().strip('"'))

    def _refresh(self) -> None:
        install_dir = self._install_dir()
        local = Updater.local_version(install_dir)
        installed = is_installed(install_dir)
        local_tag = str((local or {}).get("tag") or "")

        if installed:
            version = local_tag or self.tr("unknown_build")
            self.installed_label.setText(self.tr("installed", version=version))
        else:
            self.installed_label.setText(self.tr("not_installed"))

        if self.latest is not None:
            self.latest_label.setText(self.tr("latest", version=self.latest.tag))
        else:
            self.latest_label.setText(self.tr("latest_unknown"))

        if self.busy:
            pass  # the progress handler owns the status line
        elif self.game_running:
            self.status_label.setText(self.tr("status_game_running"))
        elif self._check_error:
            self.status_label.setText(
                self.tr("status_check_failed", error=self._check_error)
            )
        elif self._no_builds:
            self.status_label.setText(self.tr("no_builds"))
        elif self.latest is None:
            self.status_label.setText(self.tr("status_not_checked"))
        elif not installed or is_newer(self.latest.tag, local_tag or None):
            self.status_label.setText(
                self.tr(
                    "status_update",
                    version=self.latest.tag,
                    size=format_size(self.latest.asset_size),
                )
            )
        else:
            self.status_label.setText(self.tr("status_up_to_date"))

        update_available = (
            self.latest is not None
            and (not installed or is_newer(self.latest.tag, local_tag or None))
        )
        self.check_btn.setEnabled(not self.busy)
        self.install_btn.setEnabled(
            update_available and not self.busy and not self.game_running
        )
        self.install_btn.setText(
            self.tr("btn_update") if installed else self.tr("btn_install")
        )
        self.rollback_btn.setEnabled(
            not self.busy
            and not self.game_running
            and Updater.has_previous(install_dir)
        )
        can_play = installed and not self.busy
        for button in (self.play_btn, self.host_btn, self.join_btn):
            button.setEnabled(can_play)
        self.progress.setVisible(self.busy)

    def _set_busy(self, busy: bool) -> None:
        self.busy = busy
        if busy:
            self.progress.setValue(0)
            self.status_label.setText(self.tr("status_working"))
        self._refresh()

    def _progress_text(self, stage: str, details: dict) -> str:
        details = details or {}
        if stage == "download":
            done = format_size(int(details.get("read") or 0))
            total = int(details.get("total") or 0)
            if total:
                return self.tr(
                    "progress_download", done=done, total=format_size(total)
                )
            return self.tr("progress_download_unknown", done=done)
        if stage == "extract":
            return self.tr(
                "progress_extract",
                done=details.get("done", 0),
                total=details.get("total", 0),
            )
        known = {"verify", "backup", "restore", "rollback", "done"}
        return self.tr(f"progress_{stage}" if stage in known else "progress_done")

    # -------------------------------------------------------------- updating

    def check_updates(self, force: bool) -> None:
        if self.busy:
            return
        if self._releases_worker is not None and self._releases_worker.isRunning():
            return
        self._check_error = ""
        self._no_builds = False
        self.status_label.setText(self.tr("status_checking"))
        self.check_btn.setEnabled(False)
        worker = ReleasesWorker(self.updater, force)
        self._releases_worker = worker
        worker.ok.connect(self._on_releases)
        worker.fail.connect(lambda error: self._on_releases_failed(error, force))
        # Clear the reference only after QThread has really finished; dropping
        # it in the ok/fail slot can destroy a still-running QThread (Qt aborts
        # with 0xc0000409 in Qt6Core).
        worker.finished.connect(self._releases_finished)
        worker.start()

    def _releases_finished(self) -> None:
        worker = self._releases_worker
        self._releases_worker = None
        if worker is not None:
            worker.deleteLater()

    def _on_releases(self, releases: object) -> None:
        self.releases = list(releases)  # type: ignore[arg-type]
        self.latest = self.releases[0] if self.releases else None
        self._check_error = ""
        self._no_builds = self.latest is None
        self._refresh()

    def _on_releases_failed(self, error: str, manual: bool) -> None:
        logger.warning("release check failed: %s", error)
        self._no_builds = False
        if manual:
            QMessageBox.warning(
                self, self.tr("app_title"), self.tr("check_failed", error=error)
            )
        else:
            self._check_error = error
        self._refresh()

    def start_install(self) -> None:
        if self.busy:
            return
        if self.game_running:
            QMessageBox.information(
                self, self.tr("update_title"), self.tr("status_game_running")
            )
            return
        if self.latest is None:
            self.check_updates(force=True)
            return
        worker = InstallWorker(
            self.updater,
            self.latest,
            self._install_dir(),
            bool(self.config.get("keep_previous")),
        )
        self._install_worker = worker
        self._set_busy(True)
        worker.progress.connect(self._on_progress)
        worker.ok.connect(self._on_install_ok)
        worker.fail.connect(self._on_install_failed)
        worker.finished.connect(self._install_finished)
        worker.start()

    def _install_finished(self) -> None:
        worker = self._install_worker
        self._install_worker = None
        if worker is not None:
            worker.deleteLater()

    def _on_progress(self, percent: int, stage: str, details: object) -> None:
        self.progress.setValue(max(0, min(100, int(percent))))
        self.status_label.setText(self._progress_text(stage, dict(details or {})))

    def _on_install_ok(self, release: object) -> None:
        self._set_busy(False)
        tag = getattr(release, "tag", "?")
        self.latest = release if isinstance(release, Release) else self.latest
        self._refresh()
        QMessageBox.information(
            self, self.tr("update_title"), self.tr("update_done", version=tag)
        )

    def _on_install_failed(self, error: str) -> None:
        self._set_busy(False)
        QMessageBox.critical(
            self, self.tr("update_title"), self.tr("install_failed", error=error)
        )

    def rollback(self) -> None:
        if self.busy or self.game_running:
            return
        install_dir = self._install_dir()
        if not Updater.has_previous(install_dir):
            QMessageBox.information(
                self, self.tr("rollback_title"), self.tr("rollback_missing")
            )
            return
        answer = QMessageBox.question(
            self,
            self.tr("rollback_title"),
            self.tr("rollback_confirm"),
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
        )
        if answer != QMessageBox.StandardButton.Yes:
            return
        try:
            self._set_busy(True)
            self.progress.setValue(10)
            self.status_label.setText(self.tr("progress_rollback"))
            Updater.rollback(install_dir)
        except Exception as error:  # noqa: BLE001
            logger.exception("rollback failed")
            self._set_busy(False)
            QMessageBox.critical(
                self,
                self.tr("rollback_title"),
                self.tr("rollback_failed", error=str(error)),
            )
            return
        self._set_busy(False)
        self._refresh()
        QMessageBox.information(
            self, self.tr("rollback_title"), self.tr("rollback_done")
        )

    # ------------------------------------------------------------------ play

    def play(self) -> None:
        self._launch_game()

    def host(self) -> None:
        if self.busy or not is_installed(self._install_dir()):
            QMessageBox.information(
                self, self.tr("app_title"), self.tr("need_install")
            )
            return
        # Show the hosting instructions BEFORE launching. A modal box opened
        # after the game starts ends up behind the fullscreen game window and
        # the launcher looks frozen.
        box = QMessageBox(self)
        box.setWindowTitle(self.tr("host_title"))
        box.setIcon(QMessageBox.Icon.Information)
        box.setText(self.tr("host_text", ip=local_ip(), port=DEFAULT_PORT))
        launch_button = box.addButton(
            self.tr("btn_launch"), QMessageBox.ButtonRole.AcceptRole
        )
        box.addButton(self.tr("cancel"), QMessageBox.ButtonRole.RejectRole)
        box.exec()
        if box.clickedButton() is launch_button:
            self._launch_game()

    def join(self) -> None:
        if self.busy or not is_installed(self._install_dir()):
            QMessageBox.information(
                self, self.tr("app_title"), self.tr("need_install")
            )
            return
        dialog = JoinDialog(
            self, self.tr, str(self.config.get("last_join") or "")
        )
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return
        address = dialog.value()
        if not address:
            QMessageBox.warning(
                self, self.tr("join_title"), self.tr("join_empty")
            )
            return
        try:
            address = normalize_address(address)
        except ValueError:
            QMessageBox.warning(
                self, self.tr("join_title"), self.tr("join_empty")
            )
            return
        self.config.set("last_join", address)
        self._launch_game(["--mpclient", address])

    def _launch_game(self, args: list[str] | None = None) -> bool:
        install_dir = self._install_dir()
        if self.busy or not is_installed(install_dir):
            QMessageBox.information(
                self, self.tr("app_title"), self.tr("need_install")
            )
            return False
        try:
            launch(install_dir, args or [])
        except Exception as error:  # noqa: BLE001
            logger.exception("launch failed")
            QMessageBox.critical(
                self, self.tr("app_title"), self.tr("err_launch", error=str(error))
            )
            return False
        QTimer.singleShot(1500, self._poll)
        return True

    # ---------------------------------------------------------------- extras

    def _browse(self) -> None:
        chosen = QFileDialog.getExistingDirectory(
            self, self.tr("install_dir"), str(self._install_dir())
        )
        if chosen:
            self.dir_edit.setText(chosen)
            self._on_dir_changed()

    def _open_folder(self) -> None:
        install_dir = self._install_dir()
        try:
            install_dir.mkdir(parents=True, exist_ok=True)
            os.startfile(str(install_dir))  # type: ignore[attr-defined]
        except OSError as error:
            QMessageBox.warning(
                self, self.tr("app_title"), self.tr("err_generic", error=str(error))
            )

    def _on_dir_changed(self) -> None:
        self.config.set("install_dir", str(self._install_dir()))

    def _on_language_changed(self) -> None:
        value = str(self.lang_combo.currentData() or "auto")
        self.config.set("language", value)
        self.tr = Translator(detect_language(value))
        self._retranslate()

    def _poll(self) -> None:
        running = is_game_running()
        if running != self.game_running:
            self.game_running = running
            self._refresh()

    def about(self) -> None:
        QMessageBox.about(
            self,
            self.tr("about_title"),
            self.tr("about_text", version=__version__, repo=REPO_URL),
        )

    def closeEvent(self, event) -> None:  # noqa: N802 - Qt API
        checking = (
            self._releases_worker is not None
            and self._releases_worker.isRunning()
        )
        if self.busy or checking:
            QMessageBox.information(
                self, self.tr("app_title"), self.tr("close_blocked")
            )
            event.ignore()
            return
        self._on_dir_changed()
        event.accept()


def _apply_dark_palette(app: QApplication) -> None:
    app.setStyle("Fusion")
    palette = QPalette()
    palette.setColor(QPalette.ColorRole.Window, QColor(31, 33, 38))
    palette.setColor(QPalette.ColorRole.WindowText, QColor(230, 232, 235))
    palette.setColor(QPalette.ColorRole.Base, QColor(24, 26, 30))
    palette.setColor(QPalette.ColorRole.AlternateBase, QColor(38, 41, 47))
    palette.setColor(QPalette.ColorRole.Text, QColor(230, 232, 235))
    palette.setColor(QPalette.ColorRole.Button, QColor(45, 48, 55))
    palette.setColor(QPalette.ColorRole.ButtonText, QColor(230, 232, 235))
    palette.setColor(QPalette.ColorRole.Highlight, QColor(74, 144, 226))
    palette.setColor(QPalette.ColorRole.HighlightedText, QColor(255, 255, 255))
    palette.setColor(QPalette.ColorRole.ToolTipBase, QColor(45, 48, 55))
    palette.setColor(QPalette.ColorRole.ToolTipText, QColor(230, 232, 235))
    app.setPalette(palette)


def run() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("DDA NetPlay Launcher")
    app.setApplicationVersion(__version__)
    _apply_dark_palette(app)
    window = MainWindow(Config())
    window.show()
    return app.exec()
