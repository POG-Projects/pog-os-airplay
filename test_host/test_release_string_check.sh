#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILDER="$ROOT/scripts/package_release_all.sh"
VERSION=0.1.51
FIXTURE=$(mktemp "${TMPDIR:-/tmp}/pog-airplay-release-string.XXXXXX")
trap 'rm -f "$FIXTURE"' EXIT

if grep -Fq 'strings "$APP" | grep -Fxq "$VERSION"' "$BUILDER"; then
  echo "release version check must not stop strings early under pipefail" >&2
  exit 1
fi
grep -Fq 'strings "$APP" | grep -Fx "$VERSION" >/dev/null' "$BUILDER"

# Keep enough data after the early match to reproduce strings receiving
# SIGPIPE when grep uses -q under `set -o pipefail`.
awk -v version="$VERSION" 'BEGIN {
  print version
  for (i = 0; i < 500000; i++) print "firmware-symbol-" i
}' > "$FIXTURE"

strings "$FIXTURE" | grep -Fx "$VERSION" >/dev/null
