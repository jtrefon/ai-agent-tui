// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "http_transport.h"
#include "agent/debug_log.h"

#include <curl/curl.h>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace agent {

namespace {

// libcurl write callback shim: variadic_setopt cannot convert a lambda to a
// function pointer, so we need a named function. Forwards to StreamParser.
size_t stream_write_cb(void* ptr, size_t size, size_t nmemb, void* user) {
    return static_cast<StreamParser*>(user)->on_write(
        static_cast<char*>(ptr), size, nmemb);
}

long read_usage_token(const json& usage, const char* key) {
    auto it = usage.find(key);
    return (it != usage.end() && it->is_number()) ? it->get<long>() : -1;
}

void apply_tps(Stats& stats, double ttfb, double total) {
    double gen = total - ttfb;
    if (stats.completion_tokens > 0 && gen > 0.0)
        stats.tps = stats.completion_tokens / gen;
}

} // namespace

HeaderList::~HeaderList() {
    if (list) curl_slist_free_all(list);
}

void apply_auth(HeaderList& h, const Config& cfg) {
    if (!cfg.api_key.empty())
        h.add("Authorization: Bearer " + cfg.api_key);
}

// Parse a buffered /chat/completions JSON body into a Message, throwing on a
// malformed or error response so transport errors surface uniformly.
Message message_from_completion(const std::string& response) {
    json resp = json::parse(response, nullptr, false);
    if (resp.is_discarded() || !resp.contains("choices"))
        throw std::runtime_error("malformed LLM response: " + response);
    const json& msg = resp["choices"][0]["message"];
    Message out;
    out.role = "assistant";
    if (msg.contains("content") && !msg["content"].is_null())
        out.content = msg["content"].get<std::string>();
    for (const char* key : {"reasoning_content", "reasoning"}) {
        if (msg.contains(key) && msg[key].is_string())
            out.reasoning += msg[key].get<std::string>();
    }
    if (msg.contains("tool_calls") && !msg["tool_calls"].is_null())
        out.tool_calls = msg["tool_calls"];
    return out;
}

// Fill `stats` from a buffered response body and its transfer timings.
void fill_buffered_stats(Stats& stats, const std::string& response, double ttfb,
                         double total) {
    stats.valid = true;
    stats.latency_ms = ttfb * 1000.0;
    json resp = json::parse(response, nullptr, false);
    if (resp.contains("usage") && resp["usage"].is_object()) {
        const json& u = resp["usage"];
        stats.prompt_tokens = read_usage_token(u, "prompt_tokens");
        stats.completion_tokens = read_usage_token(u, "completion_tokens");
    }
    apply_tps(stats, ttfb, total);
}

// POST `payload` to the chat endpoint and return the raw response body, setting
// up auth/JSON headers and throwing on any transport error. `accept_sse` adds
// the text/event-stream Accept header for streaming requests. When non-null,
// `ttfb`/`total` receive transfer timings in seconds.
std::string post_completion(const Config& cfg, const std::string& payload,
                            bool accept_sse, double* ttfb, double* total) {
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("curl_easy_init failed");
    HeaderList headers;
    headers.add("Content-Type: application/json");
    if (accept_sse) headers.add("Accept: text/event-stream");
    apply_auth(headers, cfg);

    curl_easy_setopt(c, CURLOPT_URL, cfg.api_url().c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers.list);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
    std::string response;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, LLMClient::write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, accept_sse ? 300L : 120L);

    CURLcode rc = curl_easy_perform(c);
    if (ttfb) {
        double v = 0;
        curl_easy_getinfo(c, CURLINFO_STARTTRANSFER_TIME, &v);
        *ttfb = v;
    }
    if (total) {
        double v = 0;
        curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &v);
        *total = v;
    }
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        debug_log(cfg.debug_log, "error", std::string(curl_easy_strerror(rc)));
        throw std::runtime_error(std::string("curl error: ") +
                                 curl_easy_strerror(rc));
    }
    return response;
}

// Run a streaming completion: POST `payload`, feed SSE bytes to `parser`, and
// finalize. Fills `stats` (timings + token counts). Throws on transport error.
void stream_completion(const Config& cfg, const std::string& payload,
                       StreamParser& parser, Stats* stats, long& status_out) {
    CURL* c = curl_easy_init();
    if (!c) throw std::runtime_error("curl_easy_init failed");

    HeaderList headers;
    headers.add("Content-Type: application/json");
    headers.add("Accept: text/event-stream");
    apply_auth(headers, cfg);

    curl_easy_setopt(c, CURLOPT_URL, cfg.api_url().c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers.list);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &parser);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 1024L);  // surface deltas promptly

    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status_out);
    double ttfb = 0, total = 0;
    curl_easy_getinfo(c, CURLINFO_STARTTRANSFER_TIME, &ttfb);
    curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &total);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        debug_log(cfg.debug_log, "error-stream",
                  std::string(curl_easy_strerror(rc)));
        throw std::runtime_error(std::string("curl error: ") +
                                 curl_easy_strerror(rc));
    }
    parser.finalize();
    if (stats) {
        stats->valid = true;
        stats->latency_ms = ttfb * 1000.0;
        stats->prompt_tokens = parser.prompt_tokens();
        stats->completion_tokens = parser.completion_tokens();
        apply_tps(*stats, ttfb, total);
    }
}

} // namespace agent
