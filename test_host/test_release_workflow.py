"""Static guardrails for the quota-independent release workflow."""

import re
from pathlib import Path


root = Path(__file__).parents[1]
text = (root / ".github/workflows/ci-release.yml").read_text()
builder = (root / "scripts/package_release_all.sh").read_text()

if "actions/upload-artifact@" in text or "actions/download-artifact@" in text:
    raise SystemExit("release workflow must not use Actions artifacts")
if "      group: POG Release trusted" not in text:
    raise SystemExit("release job must use the trusted runner group")
if "      labels: pog-release-linux-x64" not in text:
    raise SystemExit("release job must require the release runner label")
if "gh workflow run ci-release.yml" not in text or "--ref main" not in text:
    raise SystemExit("validated tags must dispatch the workflow from main")

release = re.search(r"(?ms)^  release:\n(?P<body>.*)\Z", text)
if release is None:
    raise SystemExit("release job is missing")
for required in (
    "github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/main'",
    "Validate immutable release source",
    'scripts/package_release_all.sh "$AIRPLAY_BUILD_VERSION" "$SOURCE_COMMIT"',
    "Publish one immutable GitHub release",
    "--verify-tag",
):
    if required not in release.group("body"):
        raise SystemExit(f"release guardrail missing: {required}")

if builder.count("idf.py -DSDKCONFIG_DEFAULTS=") != 1:
    raise SystemExit("release builder must compile from source")
if builder.count("|sdkconfig.defaults;") != 9:
    raise SystemExit("release builder must enumerate all nine supported boards")
