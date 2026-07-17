// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/request_builder.h"

namespace agent {

namespace {

auto read_int = [](const json& o, const char* k) -> int {
    auto it = o.find(k);
    return (it != o.end() && it->is_number_integer()) ? it->get<int>() : 0;
};

} // namespace

json build_chat_body(const Config& cfg, const std::vector<Message>& messages,
                     const std::vector<Tool*>& tools, bool stream) {
    json body = {
        {"model", cfg.model},
        {"temperature", cfg.temperature},
        {"max_tokens", cfg.max_tokens},
        {"stream", stream},
        {"messages", json::array()}};

    // Ask the server to emit a final usage chunk during streaming so we can show
    // context usage and token counts (Qwen/llama.cpp/vLLM honour this).
    if (stream) body["stream_options"] = {{"include_usage", true}};

    // Qwen-style thinking control for servers using the model's native jinja
    // chat template (llama.cpp --jinja). The template reads enable_thinking (and
    // an optional thinking_budget) from chat_template_kwargs.
    //   "auto" -> send nothing, defer to the template default.
    if (cfg.thinking == "on" || cfg.thinking == "off") {
        bool enable = (cfg.thinking == "on");
        body["chat_template_kwargs"]["enable_thinking"] = enable;
        if (enable && cfg.thinking_budget > 0)
            body["chat_template_kwargs"]["thinking_budget"] =
                cfg.thinking_budget;
    }

    // Compatibility fallback for OpenAI o-series / vLLM style reasoning servers
    // that use the reasoning_effort field instead of a jinja kwarg.
    if (!cfg.reasoning_effort.empty() && cfg.reasoning_effort != "off")
        body["reasoning_effort"] = cfg.reasoning_effort;

    for (const auto& m : messages) {
        json jm = {{"role", m.role}};
        if (m.role == "assistant" && !m.tool_calls.is_null())
            jm["tool_calls"] = m.tool_calls;
        else if (!m.content.empty())
            jm["content"] = m.content;
        if (m.role == "tool") {
            jm["tool_call_id"] = m.tool_call_id;
            jm["name"] = m.name;
        }
        body["messages"].push_back(jm);
    }

    if (!tools.empty()) {
        json tarr = json::array();
        for (auto* t : tools) {
            tarr.push_back({{"type", "function"},
                            {"function",
                             {{"name", t->name()},
                              {"description", t->description()},
                              {"parameters", t->parameters_schema()}}}});
        }
        body["tools"] = tarr;
        body["tool_choice"] = "auto";
    }
    return body;
}

} // namespace agent
