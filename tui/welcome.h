// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_WELCOME_H
#define AMBER_TUI_WELCOME_H

#include <string>
#include <vector>

// The retro welcome mural shown in the persistent first window: an amber-CRT
// -headed robot holding a banner with the program name, version, release date,
// a short slash-command cheat sheet, and a tasteful author credit. Kept in its
// own module so the art can evolve without touching the Tui class.
namespace tui {
namespace welcome {

// Build the mural as a list of display lines (already amber-mono ASCII/box
// drawing). Version, build date, and author are pulled from agent::version.
std::vector<std::string> art();

} // namespace welcome
} // namespace tui

#endif // AMBER_TUI_WELCOME_H
