#include "player_view.hh"

namespace vp {

// NOT YET IMPLEMENTED — build step 1 lands create()/run() over app_shell's
// Host and a vk_canvas Renderer constructed with OutputTarget::Hdr10PQ; the
// controls follow in later steps.
//
// What this file must NEVER acquire: an OS header. Its only platform coupling
// is Host (app_shell's), and the Sink it hands to Player::open() — which it
// receives, rather than constructs, on any platform that has more than one.
struct PlayerWindow::Impl {
    std::unique_ptr<Host> host;
    Player     player;
    VideoLayer video;
};

PlayerWindow::PlayerWindow() : impl_(std::make_unique<Impl>()) {}
PlayerWindow::~PlayerWindow() = default;

bool PlayerWindow::create(std::unique_ptr<Host> host) {
    impl_->host = std::move(host);
    return false;
}
void PlayerWindow::run() {}
void PlayerWindow::shutdown() {}
void PlayerWindow::onHostResized() {}
void PlayerWindow::onHostLayoutInvalidated() {}
void PlayerWindow::onKeyDownPortable(int) {}

}  // namespace vp
