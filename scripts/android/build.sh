#!/usr/bin/env bash
# Build, install, launch on a file, and follow the log — one command, because
# the interesting part of an Android run is the logcat and opening it
# afterwards means missing the startup lines that say what refused.
#
#   scripts/android/build.sh /storage/emulated/0/Movies/some-hdr10.mkv
#
# The path is the DEVICE's, not this machine's. Push a file first with
#   adb push some.mkv /storage/emulated/0/Movies/
set -euo pipefail
cd "$(dirname "$0")/../../android"

VIDEO="${1:-}"
./gradlew assembleDebug

APK=$(find app/build/outputs/apk -name "*-arm64-v8a-*.apk" | head -1)
[ -n "$APK" ] || { echo "no arm64-v8a APK produced" >&2; exit 1; }

adb install -r "$APK"
adb logcat -c

# MANAGE_EXTERNAL_STORAGE is a "special" permission: `adb shell pm grant` does
# not work on it, and without it every absolute path fails to open. This is the
# one grant that can be done from a shell.
adb shell appops set io.nava.videoplayer MANAGE_EXTERNAL_STORAGE allow || true

if [ -n "$VIDEO" ]; then
    adb shell am start -n io.nava.videoplayer/io.nava.appshell.AppShellActivity \
        --es video_path "$VIDEO"
else
    adb shell am start -n io.nava.videoplayer/io.nava.appshell.AppShellActivity
fi

# What each tag is for:
#   VideoMain     phase lines — which startup stage refused, when it just closes
#   video_player  the app: swapchain HDR result, track summary
#   VideoCodec    the HEVC decoder and the external format it produced
#   VideoAudio    FLAC + the AAudio format actually opened
#   AppShell      HDR colour mode requested / not requested
#   vk_canvas     the resolved swapchain format+colourspace — the HDR proof
exec adb logcat -s VideoMain:V video_player:V VideoCodec:V VideoAudio:V \
                   AppShell:V AppShellActivity:V vk_canvas:V AndroidHost:V
