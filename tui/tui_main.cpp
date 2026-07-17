// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include <agent.h>

#include "tui.h"

#include <cstdio>
#include <fstream>

int main(int argc, char** argv) {
    agent::Config cfg;
    std::string config_file;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config_file = argv[++i];
        else if (a == "--api-base" && i + 1 < argc) cfg.api_base = argv[++i];
        else if (a == "--api-key" && i + 1 < argc) cfg.api_key = argv[++i];
        else if (a == "--model" && i + 1 < argc) { cfg.model = argv[++i]; cfg.model_explicit = true; }
        else if (a == "--system" && i + 1 < argc) cfg.system_prompt_path = argv[++i];
        else if (a == "--tools" && i + 1 < argc) cfg.tools_prompt_path = argv[++i];
        else if (a == "--no-stream") cfg.stream = false;
    }
    if (!config_file.empty()) cfg.load(config_file);
    {
        std::ifstream sf("amber.conf");
        if (sf) cfg.load("amber.conf");
    }
    cfg.apply_environment();

    agent::apply_server_autodetect(cfg);

    if (auto errs = cfg.validate(); !errs.empty()) {
        std::fprintf(stderr, "error: invalid configuration:\n");
        for (const auto& e : errs)
            std::fprintf(stderr, "  - %s\n", e.c_str());
        return 2;
    }

    if (cfg.system_prompt_path.empty()) cfg.system_prompt_path = "prompts/system.md";
    if (cfg.tools_prompt_path.empty()) cfg.tools_prompt_path = "prompts/tools.md";

    agent::ToolRegistry registry;
    agent::register_default_tools(registry);

    tui::Tui tui(cfg, registry);
    tui.run();
    return 0;
}
