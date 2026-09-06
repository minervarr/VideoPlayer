#pragma once

// A stand-in for the header app_shell's build normally GENERATES.
//
// app_shell's ui_metrics.hh includes ui_min_text_size.gen.h unconditionally.
// Normally a host tool derives its one constant from the faces an app ships:
// the smallest size at which the thinnest stroke in the thinnest font still
// covers half a pixel. Matrix Player runs that over four New Computer Modern
// faces and gets 18.29 px.
//
// This project draws no text — the UI so far is the picture and nothing else.
// Copying Matrix Player's number would be a claim about typefaces this player
// does not load, and the generator measures faces an app actually opens. So the
// constant is stated, with its reason. (assets/fonts now mounts the shared
// `fonts` submodule, so the faces are on hand the moment they are wanted; what
// is missing is a call site that opens one, not the fonts.)
//
// The moment this player draws its first string, this file is deleted and
// app_shell_generate_min_text_size() is called over the faces actually bundled
// — see framework/app_shell/cmake/AppShellMinTextSize.cmake.
constexpr float kMinReadableTextSizePx = 18.0f;
