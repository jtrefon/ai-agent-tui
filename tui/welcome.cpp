// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "welcome.h"

#include "agent/version.h"

#include <string>
#include <vector>

namespace tui {
namespace welcome {

namespace {

// Inner width (display columns) of the held banner's content area.
constexpr int kInner = 44;
// Leading indent that aligns the banner under the robot's arms.
const char* kPad = "        ";

// Pad/clip `s` to `w` columns (ASCII content, so byte length == columns).
std::string pad_to(const std::string& s, int w) {
    int len = static_cast<int>(s.size());
    if (len >= w) return s.substr(0, w);
    return s + std::string(w - len, ' ');
}
std::string center(const std::string& s, int w) {
    int len = static_cast<int>(s.size());
    if (len >= w) return s.substr(0, w);
    int left = (w - len) / 2;
    return std::string(left, ' ') + s + std::string(w - len - left, ' ');
}

// Repeat a UTF-8 box-drawing glyph n times.
std::string repeat(const char* glyph, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += glyph;
    return out;
}

// Banner frame rows (top/divider/bottom) share one inner width so edges align.
std::string frame_top()    { return std::string(kPad) + "\u250f" + repeat("\u2501", kInner + 2) + "\u2513"; }
std::string frame_div()    { return std::string(kPad) + "\u2523" + repeat("\u2501", kInner + 2) + "\u252b"; }
std::string frame_bottom() { return std::string(kPad) + "\u2517" + repeat("\u2501", kInner + 2) + "\u251b"; }

// A centered banner content row (used for titles).
std::string banner_row(const std::string& text) {
    return std::string(kPad) + "\u2503 " + center(text, kInner) + " \u2503";
}
// A left-aligned banner content row (used for the command cheat sheet).
std::string banner_left(const std::string& text) {
    return std::string(kPad) + "\u2503 " + pad_to(text, kInner) + " \u2503";
}

} // namespace

std::vector<std::string> art() {
    const std::string ver = std::string("v") + agent::kVersion;
    const std::string date = agent::kBuildDate;

    std::vector<std::string> a;

    // --- the CRT-headed robot -------------------------------------------
    // Amber-mono, box drawing for the CRT head + scanlines, holding a banner.
    a.push_back("");
    a.push_back("        .-------------------.");
    a.push_back("        |  .-------------.  |        A M B E R");
    a.push_back("        |  | >_          |  |     amber-crt agent");
    a.push_back("        |  | \u2592\u2592\u2592\u2592\u2592\u2592\u2592\u2592\u2592\u2592  |  |");
    a.push_back("        |  | \u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591\u2591  |  |     " + ver + "  \u00b7  " + date);
    a.push_back("        |  | \u2592\u2592\u2592\u2592\u2592\u2592\u2592\u2592\u2592\u2592  |  |");
    a.push_back("        |  '-------------'  |");
    a.push_back("        |   o   [_____]   o |");
    a.push_back("        '--.-------------.--'");
    a.push_back("        __/   |     |     \\__");
    a.push_back("       /  \\   |     |    /   \\");
    a.push_back("      | () |==='     '==='| () |");
    a.push_back("       \\__/   .-------.   \\__/");
    a.push_back("        ||    |///////|    ||");

    // --- the held banner: name / help / credit --------------------------
    a.push_back(frame_top());
    a.push_back(banner_row("A M B E R  " + ver));
    a.push_back(banner_row("an AI agent for the amber-CRT age"));
    a.push_back(frame_div());
    a.push_back(banner_left("/help      show commands & getting started"));
    a.push_back(banner_left("/model     set provider URL, model, context"));
    a.push_back(banner_left("/new       open a chat window"));
    a.push_back(banner_left("/config    show current configuration"));
    a.push_back(banner_left("/close     close this window"));
    a.push_back(frame_div());
    a.push_back(banner_row("just start typing to chat"));
    a.push_back(frame_bottom());
    a.push_back("");
    // Faux boot-ROM credit line: reads as retro copyright, not ego.
    a.push_back("        transmission by " + std::string(agent::kAuthor) +
                "  \u00b7  (c) 2026 amber systems");
    a.push_back("");

    return a;
}

} // namespace welcome
} // namespace tui
