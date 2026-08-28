#include <memory>

#include "app_main.hh"    // app_shell declares app_shell_main; we define it
#include "player_view.hh"

// The portable entry point, and the sibling of android/src/main.cc.
//
// Nothing calls this yet: no desktop Host is built (see the note at the top of
// the repo's CMakeLists.txt). It exists so that adding one is adding a Host
// and a link line, not inventing an entry point — and so the asymmetry stays
// visible rather than becoming a surprise later.
int app_shell_main(int /*argc*/, char** /*argv*/) {
    return 1;  // no desktop Host yet
}
