#!/usr/bin/env bash
set -euo pipefail

VERSION=${1:?semantic version is required}
SOURCE_COMMIT=${2:?source commit is required}

printf '%s\n' "$VERSION" | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
printf '%s\n' "$SOURCE_COMMIT" | grep -Eq '^[0-9a-f]{40}$'
test "$(tr -d '[:space:]' < version.txt)" = "$VERSION"

rm -rf dist build sdkconfig
mkdir -p dist

while IFS='|' read -r NAME ENV TARGET FLASH LABEL SDKCONFIG; do
  rm -rf build sdkconfig
  idf.py set-target "$TARGET"
  idf.py -DSDKCONFIG_DEFAULTS="$SDKCONFIG" build
  (
    cd build
    esptool.py --chip "$TARGET" merge_bin --format raw -o merged.bin $(cat flash_args)
  )

  APP="dist/firmware-$NAME.bin"
  MERGED="dist/merged-$NAME.bin"
  cp build/airplay2-receiver.bin "$APP"
  cp build/merged.bin "$MERGED"
  strings "$APP" | grep -Fxq "$VERSION"

  APP_SHA=$(sha256sum "$APP" | cut -d' ' -f1)
  MERGED_SHA=$(sha256sum "$MERGED" | cut -d' ' -f1)
  MERGED_SIZE=$(stat -c%s "$MERGED")
  python3 - "$NAME" "$ENV" "$TARGET" "$FLASH" "$LABEL" \
    "$APP_SHA" "$MERGED_SHA" "$MERGED_SIZE" <<'PY'
import json
import sys

name, env, chip, flash, label, app_sha, merged_sha, merged_size = sys.argv[1:]
fragment = {
    "env": env,
    "label": label,
    "chip": chip,
    "flashSize": flash,
    "app": f"firmware-{name}.bin",
    "appSha256": app_sha,
    "merged": f"merged-{name}.bin",
    "mergedSha256": merged_sha,
    "mergedSize": int(merged_size),
}
with open(f"dist/manifest-{name}.json", "w", encoding="utf-8") as stream:
    json.dump(fragment, stream, ensure_ascii=False, indent=2)
PY
done <<'BOARDS'
esp32s3|esp32s3|esp32s3|16MB|ESP32-S3 DevKitC-1 (16 Mo)|sdkconfig.defaults;sdkconfig.defaults.esp32s3
pog-s3|pog-s3|esp32s3|16MB|POG ESP32-S3 N16R8 (MAX98357A sur 4/5/7)|sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.pog-s3
pog-s3-voice|pog-s3-voice|esp32s3|16MB|POG ESP32-S3 N16R8 satellite vocal|sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.pog-s3;sdkconfig.defaults.pog-s3-voice
xiao-s3|xiao-s3|esp32s3|8MB|Seeed XIAO ESP32-S3 (8 Mo)|sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.xiao
n16r8|n16r8|esp32s3|16MB|POG AirPlay N16R8 (MAX98357A, 16 Mo)|sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.n16r8
squeezeamp-bt|squeezeamp|esp32|8MB|SqueezeAMP (ESP32, BT, 8 Mo)|sdkconfig.defaults;sdkconfig.defaults.squeezeamp;sdkconfig.defaults.bt
wrover-e|wrover-e|esp32|4MB|ESP32-WROVER-E (4 Mo, PSRAM)|sdkconfig.defaults;sdkconfig.defaults.wrover-e
wrover-e-voice|wrover-e-voice|esp32|4MB|ESP32-WROVER-E vocal (I2S 32/33/25/35, 4 Mo)|sdkconfig.defaults;sdkconfig.defaults.wrover-e;sdkconfig.defaults.wrover-e-voice
esparagus-audio-brick-bt|esparagus-audio-brick|esp32|8MB|Esparagus Audio Brick (ESP32, BT)|sdkconfig.defaults;sdkconfig.defaults.esparagus-audio-brick;sdkconfig.defaults.bt
BOARDS

python3 - "$VERSION" "$SOURCE_COMMIT" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

version, source_commit = sys.argv[1:]
fragments = sorted(Path("dist").glob("manifest-*.json"))
boards = [json.loads(path.read_text()) for path in fragments]
if len(boards) != 9:
    raise SystemExit(f"expected 9 boards, found {len(boards)}")
if len({board["env"] for board in boards}) != len(boards):
    raise SystemExit("duplicate board environment")
for board in boards:
    for asset_key, hash_key in (("app", "appSha256"), ("merged", "mergedSha256")):
        path = Path("dist", board[asset_key])
        if hashlib.sha256(path.read_bytes()).hexdigest() != board[hash_key]:
            raise SystemExit(f"{path}: digest mismatch")
    if Path("dist", board["merged"]).stat().st_size != board["mergedSize"]:
        raise SystemExit(f"{board['env']}: merged size mismatch")
manifest = {"version": version, "sourceCommit": source_commit, "boards": boards}
Path("dist/manifest.json").write_text(
    json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
    encoding="utf-8",
)
for path in fragments:
    path.unlink()
PY

(
  cd dist
  sha256sum *.bin manifest.json > SHA256SUMS
  cat manifest.json
  cat SHA256SUMS
)
