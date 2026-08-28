#pragma once

// PlayerWindow — the application.
//
// Layout, drawing, hit-testing, and the transport controls. It includes NO OS
// header: every real-platform thing it needs arrives through app_shell's Host,
// which is what makes the Android build and a future desktop build the same
// program rather than two ports of one idea (CLAUDE.md rule 2).
//
// It owns a vp::Player (the state machine), a vp::VideoLayer (the picture),
// and whatever the Host handed it. It does not own a decoder: the Sink it
// constructs is platform code, made in create() and handed straight to
// Player::open().

#include <memory>
#include <string>

#include "app_view.hh"   // app_shell
#include "host.hh"       // app_shell
#include "core/player.h"
#include "video_layer.hh"

namespace vp {

class PlayerWindow : public AppView {
public:
    PlayerWindow();
    ~PlayerWindow() override;

    // Takes the Host the platform bootstrap built. False means the window,
    // Vulkan, or the decoder refused — the Host has already been told why in
    // whatever way that platform reports things (a logcat line on Android).
    bool create(std::unique_ptr<Host> host);
    void run();

    // ── AppView ───────────────────────────────────────────────────────────
    void onHostResized() override;
    void shutdown() override;
    void onHostLayoutInvalidated() override;
    void onKeyDownPortable(int keyCode) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vp
