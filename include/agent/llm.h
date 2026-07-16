#ifndef AGENT_LLM_H
#define AGENT_LLM_H

#include <string>
#include <vector>
#include <functional>
#include "agent/config.h"
#include "agent/tool.h"

namespace agent {

// A single message in the conversation. role is one of:
//   "system", "user", "assistant", "tool"
struct Message {
    std::string role;
    std::string content;
    std::string reasoning;               // assistant thinking (not sent back)
    std::string tool_call_id;            // for role == "tool"
    std::string name;                    // tool name for tool messages
    json tool_calls;                     // assistant messages may carry calls
};

// One parsed Server-Sent-Event delta from a streaming response.
struct StreamChunk {
    bool done = false;                   // terminal [DONE] marker seen
    std::string delta;                   // incremental answer text (if any)
    std::string reasoning;               // incremental thinking/reasoning text
    json tool_calls;                     // incremental tool_call fragments (if any)
};

// Thin client over an OpenAI-compatible /chat/completions endpoint using
// libcurl. The library does not own the conversation state; callers pass the
// full message list each turn. Supports both buffered (chat) and streamed
// (chat_stream) modes.
class LLMClient {
public:
    explicit LLMClient(const Config& cfg);

    // Buffered request: returns the full assistant message (may include
    // tool_calls). Throws std::runtime_error on transport/API failure.
    Message chat(const std::vector<Message>& messages,
                 const std::vector<Tool*>& tools);

    // Streaming request: invokes `on_chunk` for every parsed SSE event and
    // returns the assembled assistant message. The callback receives partial
    // text as it arrives, enabling live TUI rendering.
    Message chat_stream(const std::vector<Message>& messages,
                        const std::vector<Tool*>& tools,
                        const std::function<void(const StreamChunk&)>& on_chunk);

private:
    Config cfg_;

    json build_body(const std::vector<Message>& messages,
                    const std::vector<Tool*>& tools, bool stream) const;

    static size_t write_cb(void* ptr, size_t size, size_t nmemb, void* user);
};

} // namespace agent

#endif // AGENT_LLM_H
