#include <memory>
#include <string>

#include "app_main.hh"    // app_shell declares app_shell_main; we define it
#include "player_view.hh"

// app_shell's desktop bootstrap owns main(); it builds the Host for whichever
// windowing system this is (os/wayland_host.cc on Linux) and calls us.
std::unique_ptr<Host> make_host();

// The portable entry point, and the sibling of android/src/main.cc.
//
// It returned 1 unconditionally for a long time, because no desktop Host was
// built. One is now: app_shell has had a complete Wayland Host the whole time,
// and what was actually missing was a gui/ that could compile without Android
// headers in it. That is fixed, so this is real.
//
// The file comes from the command line here rather than from
// Host::launchArgument(): a desktop app is handed its argument by a shell, and
// an Intent extra is Android's answer to the same question. openWhateverWeWere
// LaunchedWith() covers the Android side.
int app_shell_main(int argc, char** argv) {
    vp::PlayerWindow win;
    if (!win.create(make_host())) return 1;

    if (argc > 1 && argv[1]) win.openPath(argv[1]);

    win.run();
    win.shutdown();
    return 0;
}
