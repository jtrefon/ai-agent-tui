// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "palette.h"

namespace tui::palette {

std::string token(const std::string& input) {
    if (input.empty() || input[0] != '/') return "";
    std::string rest = input.substr(1);
    size_t sp = rest.find(' ');
    return sp == std::string::npos ? rest : rest.substr(0, sp);
}

bool has_arg(const std::string& input) {
    return input.find(' ') != std::string::npos;
}

bool wants_open(const std::string& input) {
    return !input.empty() && input[0] == '/';
}

std::vector<const Command*> filter(const std::vector<Command>& commands,
                                   const std::string& tok) {
    std::vector<const Command*> out;
    for (const auto& c : commands) {
        bool hit = tok.empty() || c.name.rfind(tok, 0) == 0;
        if (!hit)
            for (const auto& a : c.aliases)
                if (a.rfind(tok, 0) == 0) { hit = true; break; }
        if (hit) out.push_back(&c);
    }
    return out;
}

const Command* find(const std::vector<Command>& commands,
                    const std::string& name) {
    for (const auto& c : commands) {
        if (c.name == name) return &c;
        for (const auto& a : c.aliases)
            if (a == name) return &c;
    }
    return nullptr;
}

std::string common_prefix(const std::vector<std::string>& names) {
    if (names.empty()) return "";
    std::string p = names.front();
    for (const auto& n : names) {
        size_t i = 0;
        while (i < p.size() && i < n.size() && p[i] == n[i]) ++i;
        p.resize(i);
    }
    return p;
}

std::string usage(const Command& c) {
    std::string u = "/" + c.name;
    if (!c.args.empty()) u += " " + c.args;
    return u;
}

std::string complete(const std::vector<Command>& commands,
                     const std::string& input, int sel) {
    std::string tok = token(input);
    auto matches = filter(commands, tok);
    if (matches.empty()) return input;

    // If a row is selected, complete straight to it. Otherwise complete to the
    // longest common prefix of the matches' primary names.
    if (sel >= 0 && sel < static_cast<int>(matches.size()))
        return "/" + matches[sel]->name + " ";

    std::vector<std::string> names;
    for (auto* c : matches) names.push_back(c->name);
    std::string cp = common_prefix(names);
    if (cp.size() > tok.size())
        return "/" + cp;                 // extend to shared prefix
    if (matches.size() == 1)
        return "/" + matches.front()->name + " ";
    return input;                        // ambiguous, nothing to add
}

// Tab pressed in argument context: complete the partially-typed argument
// against the command's candidate list. Returns the (possibly rewritten) full
// input line. When the result is ambiguous (multiple candidates sharing the
// resulting prefix), `out_choices` is filled with the candidates so the caller
// can pop up a selection list (zsh-style).
std::string complete_arg(const std::vector<Command>& commands,
                         const std::string& input,
                         std::vector<std::string>& out_choices) {
    out_choices.clear();
    if (input.empty() || input[0] != '/') return input;
    std::string rest = input.substr(1);
    size_t sp = rest.find(' ');
    if (sp == std::string::npos) return input;        // still typing command
    std::string name = rest.substr(0, sp);
    const Command* c = find(commands, name);
    if (!c || !c->complete_arg) return input;
    std::string partial = rest.substr(sp + 1);
    auto cand = c->complete_arg(partial);
    if (cand.empty()) return input;

    std::string cp = common_prefix(cand);
    if (cp.size() > partial.size())
        return "/" + name + " " + cp;                // extend to shared prefix
    if (cand.size() == 1)
        return "/" + name + " " + cand.front() + " ";
    out_choices = std::move(cand);                    // let caller show a list
    return input;                                     // leave line unchanged
}

}  // namespace tui::palette
