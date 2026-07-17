// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/llm.h"
#include "agent/debug_log.h"
#include "http_transport.h"
#include "agent/model_probe.h"
#include "agent/request_builder.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace agent {

LLMClient::LLMClient(const Config& cfg) : cfg_(cfg) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

size_t LLMClient::write_cb(void* ptr, size_t size, size_t nmemb, void* user) {
    auto* buf = static_cast<std::string*>(user);
    buf->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

ServerInfo LLMClient::parse_models(const std::string& body) {
    return agent::parse_models(body);
}

ServerInfo LLMClient::probe_server() const {
    return agent::probe_server(cfg_);
}

Message LLMClient::chat(const std::vector<Message>& messages,
                        const std::vector<Tool*>& tools, Stats* stats) {
    json body = build_chat_body(cfg_, messages, tools, false);
    std::string payload = body.dump();
    debug_log(cfg_.debug_log, "request", payload);

    double ttfb = 0, total = 0;
    std::string response = post_completion(cfg_, payload, false, &ttfb, &total);
    debug_log(cfg_.debug_log, "response", response);

    Message out = message_from_completion(response);
    if (stats) {
        stats->valid = true;
        stats->latency_ms = ttfb * 1000.0;
        json resp = json::parse(response, nullptr, false);
        if (resp.contains("usage") && resp["usage"].is_object()) {
            const json& u = resp["usage"];
            if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number())
                stats->prompt_tokens = u["prompt_tokens"].get<long>();
            if (u.contains("completion_tokens") &&
                u["completion_tokens"].is_number())
                stats->completion_tokens = u["completion_tokens"].get<long>();
        }
        double gen = total - ttfb;
        if (stats->completion_tokens > 0 && gen > 0.0)
            stats->tps = stats->completion_tokens / gen;
    }
    return out;
}

Message LLMClient::chat_stream(const std::vector<Message>& messages,
                               const std::vector<Tool*>& tools,
                               const std::function<void(const StreamChunk&)>& on_chunk,
                               Stats* stats) {
    json body = build_chat_body(cfg_, messages, tools, true);
    std::string payload = body.dump();
    debug_log(cfg_.debug_log, "request-stream", payload);

    Message out;
    out.role = "assistant";
    StreamParser parser(out, on_chunk, cfg_.debug_log);

    long status = 0;
    stream_completion(cfg_, payload, parser, stats, status);
    debug_log(cfg_.debug_log, "response-stream",
              "http=" + std::to_string(status) +
                  " content=" + out.content +
                  "\n---reasoning---\n" + out.reasoning);
    return out;
}

} // namespace agent
