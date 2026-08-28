#include <android_native_app_glue.h>
#include <android/log.h>

#include <exception>
#include <memory>

#include "android_host.hh"  // app_shell: the Android Host
#include "player_view.hh"

#define LOG_TAG "VideoMain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// The Android entry point, and the sibling of gui/src/gui_main.cc: construct
// the app, hand it a Host, run it. Everything below create() is the same
// PlayerWindow a desktop host would run — there is no Android build of the UI,
// because player_view.cc includes no OS header and reaches the platform only
// through app_shell's Host.
//
// ── Why this function is so loud ─────────────────────────────────────────────
//
// When android_main() RETURNS, the activity is finished — so every failure
// here looks identical from the outside: the app opens and closes. There is no
// window left to put an error in and no console to print one to. The phase
// lines below are the only way to tell "create() refused" from "something
// threw" from "it ran and quit", and they cost one logcat line each at
// startup. Filter with:
//
//     adb logcat -s VideoMain:V AndroidHost:V video_player:V
void android_main(android_app* state) {
    LOGI("phase 1/5: entry -- constructing PlayerWindow");
    try {
        vp::PlayerWindow win;

        LOGI("phase 2/5: create() -- host init, Vulkan, decoder");
        // "video_path" is the intent extra this app is launched with; the
        // fallback is where a phone keeps video. app_shell knows neither — it
        // reads whatever key it is handed.
        if (!win.create(std::make_unique<AndroidHost>(
                state, "video_path", "/storage/emulated/0/Movies"))) {
            LOGE("phase 2/5 FAILED: create() returned false -- the activity "
                 "will now finish. The line above this one is the reason.");
            return;
        }

        LOGI("phase 3/5: create() OK -- entering run()");
        win.run();

        LOGI("phase 4/5: run() returned -- shutting down");
        win.shutdown();
        LOGI("phase 5/5: clean exit");
    } catch (const std::exception& e) {
        LOGE("FATAL: unhandled exception: %s", e.what());
    } catch (...) {
        LOGE("FATAL: unhandled non-standard exception");
    }
}
