// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui.h"

namespace tui {

agent::Session Tui::snapshot(Window& w) {
    agent::Session s;
    s.id = w.session_id;
    s.model = cfg_.model;
    s.messages = w.agent ? w.agent->history() : std::vector<agent::Message>{};
    s.derive_title();
    if (w.title != "chat" && !w.title.empty()) s.title = w.title;
    return s;
}

void Tui::autosave() {
    Window& w = win();
    if (!w.dirty || !w.agent || w.agent->history().empty()) return;
    agent::Session s = snapshot(w);
    if (store_.save(s)) {
        w.session_id = s.id;
        if (w.title == "chat" && !s.title.empty()) w.title = s.title;
        w.dirty = false;
    }
}

void Tui::save_session() {
    Window& w = win();
    if (!w.agent || w.agent->history().empty()) {
        append_line(P_STATUS, "nothing to save (empty conversation)");
        return;
    }
    agent::Session s = snapshot(w);
    if (store_.save(s)) {
        w.session_id = s.id;
        w.dirty = false;
        append_line(P_STATUS, "saved session " + s.id + " (\"" + s.title + "\")");
    } else {
        append_line(P_STATUS, "save failed (could not write " + store_.dir() + ")");
    }
}

void Tui::load_session(const std::string& id) {
    agent::Session s;
    if (!store_.load(id, s)) {
        append_line(P_STATUS, "load failed: no session " + id);
        return;
    }
    Window& w = new_window(s.title.empty() ? "chat" : s.title);
    w.session_id = s.id;
    w.agent->set_history(s.messages);
    for (const auto& m : s.messages) {
        if (m.role == "user") append_line(P_USER, "> " + m.content);
        else if (m.role == "assistant" && !m.content.empty())
            append_line(P_ASSISTANT, m.content);
    }
    append_line(P_STATUS, "loaded session " + s.id);
    draw();
}

void Tui::pick_session() {
    auto metas = store_.list();
    if (metas.empty()) { append_line(P_STATUS, "no saved sessions"); return; }
    std::vector<std::string> labels;
    for (const auto& m : metas)
        labels.push_back(m.title + "  (" + std::to_string(m.message_count) +
                         " msgs)");
    int sel = drawer_menu("Load session", labels);
    if (sel >= 0 && sel < static_cast<int>(metas.size()))
        load_session(metas[sel].id);
}

void Tui::switch_to(size_t idx) {
    if (idx >= windows_.size() || idx == active_) return;
    active_ = idx;
    draw();
}

void Tui::close_window() {
    if (windows_.size() <= 1) {
        append_line(P_STATUS, "cannot close the last window");
        return;
    }
    autosave();
    windows_.erase(windows_.begin() + active_);
    if (active_ >= windows_.size()) active_ = windows_.size() - 1;
    draw();
}

void Tui::request_quit() { quit_ = true; }
void Tui::redraw_after_modal() { touchwin(stdscr); draw(); draw_input(""); }

} // namespace tui
