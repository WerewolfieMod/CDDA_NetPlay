"""Tiny two-language UI catalog (English / Russian)."""
from __future__ import annotations

STRINGS: dict[str, dict[str, str]] = {
    "en": {
        "app_title": "DDA NetPlay Launcher",
        "subtitle": "Co-op launcher for Cataclysm: DDA",
        "group_game": "Game",
        "install_dir": "Install folder",
        "browse": "Browse...",
        "open_folder": "Open",
        "installed": "Installed: {version}",
        "not_installed": "Not installed",
        "unknown_build": "Unknown build",
        "latest": "Latest: {version}",
        "latest_unknown": "Latest: not checked",
        "status_ready": "Ready",
        "status_checking": "Checking GitHub releases...",
        "status_up_to_date": "You have the latest build.",
        "status_update": "New build available: {version} ({size})",
        "status_not_checked": "Press \"Check for updates\".",
        "status_game_running": "The game is running.",
        "btn_check": "Check for updates",
        "btn_install": "Install",
        "btn_update": "Update",
        "btn_play": "Play",
        "btn_host": "Host co-op",
        "btn_join": "Join server...",
        "btn_rollback": "Restore previous build",
        "menu_language": "Language",
        "lang_auto": "Auto",
        "menu_about": "About",
        "about_title": "About",
        "about_text": "DDA NetPlay Launcher {version}\n\n"
                      "Downloads and updates DDA NetPlay builds from GitHub Releases.\n"
                      "Game builds: {repo}\n\n"
                      "Saves, configuration and mods are kept on update; the previous build can be restored.",
        "join_title": "Join a network world",
        "join_label": "Host address:",
        "join_hint": "Example: 192.168.1.10 or 192.168.1.10:24224",
        "join_ok": "Join",
        "cancel": "Cancel",
        "join_empty": "Enter the host address.",
        "host_title": "Host a co-op world",
        "host_text": "The game will start.\n\n"
                     "In game press ESC and choose \"Network: open world for co-op\" (port {port}).\n\n"
                     "Your LAN address: {ip}:{port}\n\n"
                     "Allow port {port} in the Windows firewall if the other player cannot connect.",
        "need_install": "Install the game first.",
        "busy": "Wait for the current operation to finish.",
        "close_blocked": "An update is in progress. Wait for it to finish.",
        "err_launch": "Cannot start the game:\n{error}",
        "err_generic": "Error:\n{error}",
        "update_title": "Update",
        "update_done": "Installed {version}.",
        "rollback_title": "Restore previous build",
        "rollback_confirm": "Replace the current build with the previous one?\n"
                            "Saves, config and mods are kept.",
        "rollback_done": "Previous build restored.",
        "rollback_missing": "There is no previous build.",
        "rollback_failed": "Rollback failed:\n{error}",
        "check_failed": "Cannot check for updates:\n{error}",
        "install_failed": "Installation failed:\n{error}",
        "need_release": "Check for updates first.",
        "repo_link": "GitHub repository",
        "no_builds": "No builds with a Windows ZIP asset were found in the repository yet.",
        "status_check_failed": "Update check failed: {error}",
        "status_working": "Working...",
        "progress_download": "Downloading {done} / {total}",
        "progress_download_unknown": "Downloading {done}",
        "progress_verify": "Verifying the archive...",
        "progress_extract": "Extracting {done}/{total}",
        "progress_backup": "Backing up the current build",
        "progress_restore": "Restoring saves and configuration",
        "progress_rollback": "Restoring the previous build",
        "progress_done": "Done",
    },
    "ru": {
        "app_title": "DDA NetPlay Launcher",
        "subtitle": "Лаунчер совместной игры для Cataclysm: DDA",
        "group_game": "Игра",
        "install_dir": "Папка установки",
        "browse": "Выбрать...",
        "open_folder": "Открыть",
        "installed": "Установлено: {version}",
        "not_installed": "Не установлено",
        "unknown_build": "Неизвестная сборка",
        "latest": "Последняя: {version}",
        "latest_unknown": "Последняя: не проверялась",
        "status_ready": "Готово",
        "status_checking": "Проверяю релизы на GitHub...",
        "status_up_to_date": "У вас последняя сборка.",
        "status_update": "Доступна новая сборка: {version} ({size})",
        "status_not_checked": "Нажмите «Проверить обновления».",
        "status_game_running": "Игра запущена.",
        "btn_check": "Проверить обновления",
        "btn_install": "Установить",
        "btn_update": "Обновить",
        "btn_play": "Играть",
        "btn_host": "Хост co-op",
        "btn_join": "Подключиться...",
        "btn_rollback": "Вернуть предыдущую сборку",
        "menu_language": "Язык",
        "lang_auto": "Авто",
        "menu_about": "О программе",
        "about_title": "О программе",
        "about_text": "DDA NetPlay Launcher {version}\n\n"
                      "Скачивает и обновляет сборки DDA NetPlay с GitHub Releases.\n"
                      "Сборки игры: {repo}\n\n"
                      "При обновлении сохраняются сейвы, настройки и моды; предыдущую сборку можно вернуть.",
        "join_title": "Подключение к сетевому миру",
        "join_label": "Адрес хоста:",
        "join_hint": "Например: 192.168.1.10 или 192.168.1.10:24224",
        "join_ok": "Подключиться",
        "cancel": "Отмена",
        "join_empty": "Введите адрес хоста.",
        "host_title": "Хостинг совместного мира",
        "host_text": "Игра запустится.\n\n"
                     "В игре нажмите ESC и выберите «Сеть: открыть мир для совместной игры» (порт {port}).\n\n"
                     "Ваш адрес в локальной сети: {ip}:{port}\n\n"
                     "Если другой игрок не подключается, разрешите порт {port} в брандмауэре Windows.",
        "need_install": "Сначала установите игру.",
        "busy": "Дождитесь завершения текущей операции.",
        "close_blocked": "Идёт обновление. Дождитесь его завершения.",
        "err_launch": "Не удалось запустить игру:\n{error}",
        "err_generic": "Ошибка:\n{error}",
        "update_title": "Обновление",
        "update_done": "Установлена сборка {version}.",
        "rollback_title": "Возврат предыдущей сборки",
        "rollback_confirm": "Заменить текущую сборку предыдущей?\n"
                            "Сейвы, настройки и моды сохранятся.",
        "rollback_done": "Предыдущая сборка восстановлена.",
        "rollback_missing": "Предыдущей сборки нет.",
        "rollback_failed": "Не удалось вернуть предыдущую сборку:\n{error}",
        "check_failed": "Не удалось проверить обновления:\n{error}",
        "install_failed": "Не удалось установить сборку:\n{error}",
        "need_release": "Сначала проверьте обновления.",
        "repo_link": "Репозиторий на GitHub",
        "no_builds": "В репозитории пока нет сборок с Windows ZIP.",
        "status_check_failed": "Не удалось проверить обновления: {error}",
        "status_working": "Выполняю...",
        "progress_download": "Скачивание: {done} / {total}",
        "progress_download_unknown": "Скачивание: {done}",
        "progress_verify": "Проверяю архив...",
        "progress_extract": "Распаковка: {done}/{total}",
        "progress_backup": "Создаю резервную копию текущей сборки",
        "progress_restore": "Возвращаю сейвы и настройки",
        "progress_rollback": "Возвращаю предыдущую сборку",
        "progress_done": "Готово",
    },
}


def detect_language(configured: str) -> str:
    if configured in STRINGS:
        return configured
    try:
        from PySide6.QtCore import QLocale
        if QLocale.system().language() == QLocale.Language.Russian:
            return "ru"
        return "en"
    except Exception:
        import locale
        try:
            name = (locale.getlocale()[0] or "").lower()
        except Exception:
            name = ""
        return "ru" if name.startswith("ru") else "en"


class Translator:
    def __init__(self, language: str) -> None:
        self.language = language

    def __call__(self, key: str, **kwargs: object) -> str:
        text = STRINGS.get(self.language, {}).get(key) or STRINGS["en"].get(key) or key
        if kwargs:
            try:
                return text.format(**kwargs)
            except (KeyError, IndexError, ValueError):
                return text
        return text
