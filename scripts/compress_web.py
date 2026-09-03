"""Deterministically compress OTA-embedded pages, leaving source assets intact."""
import gzip
from pathlib import Path
import sys


def compress(source, destination):
    source, destination = Path(source), Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    for name in ("index.html", "logs.html"):
        data = gzip.compress((source / name).read_bytes(), compresslevel=9, mtime=0)
        target = destination / (name + ".gz")
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)


if "Import" in globals():
    Import("env")  # noqa: F821 -- PlatformIO injects the build environment
    compress(env.subst("$PROJECT_DIR/data/www"), env.subst("$BUILD_DIR/embedded-web"))  # noqa: F821
elif __name__ == "__main__":
    compress(*sys.argv[1:])
