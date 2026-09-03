"""Pack bundled models for ESP32 ROM miniz; no model download at build/runtime."""
from pathlib import Path
import struct
import sys
import zlib


def pack(source, destination):
    source, destination = Path(source), Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    for name in ("hey_jarvis", "okay_nabu", "alexa"):
        raw = (source / (name + ".tflite")).read_bytes()
        if not 8 <= len(raw) <= 65536 or raw[4:8] != b"TFL3":
            raise ValueError("Invalid bundled model: " + name)
        data = struct.pack("<I", len(raw)) + zlib.compress(raw, 9)
        target = destination / (name + ".z")
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)


if "Import" in globals():
    Import("env")  # noqa: F821
    pack(env.subst("$PROJECT_DIR/components/pogwake/models"), env.subst("$BUILD_DIR/embedded-wake"))  # noqa: F821
elif __name__ == "__main__":
    pack(*sys.argv[1:])
