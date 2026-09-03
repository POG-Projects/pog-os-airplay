"""Track .incbin inputs in PlatformIO's SCons build as well as CMake.

PlatformIO does not carry CMake's OBJECT_DEPENDS into its assembly builder.
Without these edges an HTML-only edit leaves the old control panel in the app.
"""

Import("env")  # noqa: F821 -- PlatformIO injects the build environment

env.Depends(  # noqa: F821
    "$BUILD_DIR/main/network/web_index.S.o",
    ["$PROJECT_DIR/data/www/index.html", "$PROJECT_DIR/data/www/logs.html",
     "$BUILD_DIR/embedded-web/index.html.gz", "$BUILD_DIR/embedded-web/logs.html.gz"],
)
env.Depends(  # noqa: F821
    "$BUILD_DIR/components/pogwake/wake_models.S.o",
    ["$PROJECT_DIR/components/pogwake/models/" + name + ".tflite"
     for name in ("hey_jarvis", "okay_nabu", "alexa")] +
    ["$BUILD_DIR/embedded-wake/" + name + ".z"
     for name in ("hey_jarvis", "okay_nabu", "alexa")],
)
