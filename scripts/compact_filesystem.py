"""Stage WROVER-E web assets without optional TFT backgrounds.

Used by both ESP-IDF and PlatformIO (including `pio run -t buildfs`).
Only the generated staging directory is written; data/ is never modified.
"""

from pathlib import Path
import shutil
import sys


def stage(source, destination):
    source, destination = Path(source), Path(destination)
    web = source / "www"
    if not (web / "index.html").is_file():
        raise ValueError(f"Missing web assets in {web}")
    if destination.resolve() == source.resolve():
        raise ValueError("Staging must not overwrite the source assets")
    destination.mkdir(parents=True, exist_ok=True)
    if (destination / "www").exists():
        shutil.rmtree(destination / "www")
    shutil.copytree(web, destination / "www", dirs_exist_ok=True)


if "Import" in globals():
    Import("env")  # noqa: F821 -- PlatformIO/SCons injects Import and env
    compact = env.subst("$BUILD_DIR/compact-data")  # noqa: F821
    stage(env.subst("$PROJECT_DIR/data"), compact)  # noqa: F821
    env.Replace(PROJECT_DATA_DIR=compact)  # noqa: F821
elif __name__ == "__main__":
    stage(*sys.argv[1:])
