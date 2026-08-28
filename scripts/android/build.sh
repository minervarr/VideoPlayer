#!/usr/bin/env bash
# Build, install and follow the log. One command, because the interesting part
# of an Android run is the logcat and forgetting to open it wastes the run.
set -euo pipefail
cd "$(dirname "$0")/../../android"

VARIANT="${1:-Debug}"
./gradlew "assemble${VARIANT}"

APK=$(find app/build/outputs/apk -name "*-arm64-v8a-*.apk" | head -1)
[ -n "$APK" ] || { echo "no arm64-v8a APK produced" >&2; exit 1; }

adb install -r "$APK"
adb logcat -c
adb shell am start -n io.nava.videoplayer/android.app.NativeActivity

# VideoMain's phase lines say which stage refused when the activity just
# closes; the vk_canvas tag carries the resolved swapchain format, which is
# how you confirm HDR10 actually happened.
exec adb logcat -s VideoMain:V AndroidHost:V video_player:V vk_canvas:V
