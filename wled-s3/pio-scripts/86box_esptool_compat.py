from pathlib import Path
import os
import shutil

Import("env")


def platformio_home():
    return Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")).expanduser()


def package_dir_from_platform():
    try:
        package_dir = env.PioPlatform().get_package_dir("tool-esptoolpy")
    except Exception:
        return None
    return Path(package_dir).expanduser() if package_dir else None


def is_esptool_dir(path):
    return path and (path / "esptool.py").is_file() and (path / "esptool").is_dir()


home = platformio_home()
expected = home / "packages" / "tool-esptoolpy"
candidates = [
    package_dir_from_platform(),
    home / "packages" / "tool-esptoolpy",
    home / "tools" / "tool-esptoolpy",
]

source = next((path for path in candidates if is_esptool_dir(path)), None)
if not source:
    print("86Box: tool-esptoolpy not found; PlatformIO may install it during this build")
elif not is_esptool_dir(expected):
    expected.parent.mkdir(parents=True, exist_ok=True)
    if expected.is_symlink():
        expected.unlink()
    if not expected.exists():
        try:
            expected.symlink_to(source, target_is_directory=True)
        except OSError:
            shutil.copytree(source, expected, dirs_exist_ok=True)
        print("86Box: linked tool-esptoolpy for Espressif builder compatibility")