#pragma once

// PlayerWindow — the application.
//
// It includes NO OS header: everything real-platform arrives through
// app_shell's Host, which is what would make an Android build and a desktop
// build the same program rather than two ports of one idea (CLAUDE.md rule 2).
// The one thing it does construct per-platform is the Sink handed to
// Player::open(); today Android is the only host, so that construction lives
// in create() behind the one #if in this file's .cc.
//
// Threads, because a video player has three jobs that must not wait on each
// other:
//
//   the UI thread     — pumps the host, draws, decides which frame is due
//   the feed thread   — pulls packets out of the Demuxer into both decoders
//   the decoder threads — owned by MediaCodecVideo and FlacOutput
//
// core/ has none of them. It is called from them.

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

    bool create(std::unique_ptr<Host> host);
    void run();

    // ── AppView ───────────────────────────────────────────────────────────
    void onHostResized() override;
    void shutdown() override;
    void onHostLayoutInvalidated() override;
    void onKeyDownPortable(int keyCode) override;
    void onLButtonUp(int x, int y) override;
    void onSurfaceLost() override;
    bool onSurfaceRecreated() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vp
