// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/agent.h"
#include "agent/prompt.h"
#include <stdexcept>
#include <chrono>
#include <string>

namespace agent {

namespace {
// Parse one OpenAI tool_call delta into its id/name/args triple. The arguments
// field may arrive as a JSON string (streaming fragments) and is parsed here.
void parse_tool_call(const json& call, std::string& id, std::string& fn,
                     json& args) {
    auto str_or = [](const json& j, const char* k,
                     const std::string& d) -> std::string {
        auto it = j.find(k);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : d;
    };
    json fnobj = call.contains("function") && call["function"].is_object()
                     ? call["function"] : json::object();
    id = str_or(call, "id", "");
    fn = str_or(fnobj, "name", "");
    args = fnobj.contains("arguments") && !fnobj["arguments"].is_null()
               ? fnobj["arguments"] : json::object();
    if (args.is_string()) {
        try { args = json::parse(args.get<std::string>()); }
        catch (...) { args = json::object(); }
    }
}
} // namespace

Agent::Agent(const Config& cfg, ToolRegistry& registry, AgentHooks hooks)
    : cfg_(cfg), registry_(registry), client_(cfg), hooks_(std::move(hooks)) {}

void Agent::ensure_system_prompt() {
    if (!history_.empty() && history_.front().role == "system") return;

    std::string system = load_prompt(cfg_.system_prompt_path);
    if (system.empty())
        system = "You are amber, a helpful coding assistant running on a "
                 "Linux server. Use the provided tools when they help.";
    if (!cfg_.tools_prompt_path.empty())
        system += "\n\n" + load_prompt(cfg_.tools_prompt_path);
    else if (!registry_.empty())
        system += "\n\n" + render_tools_markdown(registry_);

    Message sys_msg;
    sys_msg.role = "system";
    sys_msg.content = system;
    history_.insert(history_.begin(), sys_msg);
}

void Agent::set_history(std::vector<Message> messages) {
    history_ = std::move(messages);
}

bool Agent::approve_call(const Tool& tool, const json& args) {
    // Fail safe: with no approval handler, deny gated tools outright.
    if (!hooks_.on_approval) return false;
    std::string summary = tool.summarize(args);
    Approval d = hooks_.on_approval(tool.name(), args, summary);
    if (d == Approval::AllowSession) {
        session_approved_.insert(tool.name());
        return true;
    }
    return d == Approval::AllowOnce;
}

// Execute every requested tool call, recording results back into the
// conversation history so the model can consume them on the next iteration.
void Agent::dispatch_tool_calls(const json& calls, std::vector<Tool*>&) {
    for (const auto& call : calls) {
        std::string id, fn;
        json args;
        parse_tool_call(call, id, fn, args);
        if (hooks_.on_tool_call) hooks_.on_tool_call(fn, args);
        log_.event("tool_call", {{"name", fn}, {"id", id}, {"args", args}});

        Tool* tool = registry_.find(fn);
        ToolResult res;
        if (!tool) {
            res.ok = false;
            res.error = "unknown tool: " + fn;
        } else if (tool->requires_approval() && !session_approved_.count(fn) &&
                   !approve_call(*tool, args)) {
            // Denied (or no approval handler installed). Report back so the
            // model can adapt instead of silently failing.
            res.ok = false;
            res.error = "denied by user: the " + fn + " tool was not approved";
            log_.event("tool_denied", {{"name", fn}, {"id", id}, {"args", args}});
        } else {
            try { res = tool->execute(args); }
            catch (const std::exception& e) {
                res.ok = false;
                res.error = std::string("tool threw: ") + e.what();
            }
        }
        if (hooks_.on_tool_result) hooks_.on_tool_result(fn, res);
        log_.event("tool_result", {{"name", fn}, {"id", id},
                                    {"ok", res.ok},
                                    {"output", res.ok ? res.output : res.error}});

        Message tool_msg;
        tool_msg.role = "tool";
        tool_msg.tool_call_id = id;
        tool_msg.name = fn;
        tool_msg.content = res.ok ? res.output : ("ERROR: " + res.error);
        history_.push_back(tool_msg);
    }
}

Message Agent::chat_once(std::vector<Tool*>& tools) {
    Message reply;
    Stats stats;
    if (hooks_.on_state) hooks_.on_state(RunState::Waiting);
    if (cfg_.stream) {
        reply = client_.chat_stream(history_, tools,
            [this](const StreamChunk& ch) {
                if (ch.done) return;
                if (!ch.reasoning.empty()) {
                    if (hooks_.on_state) hooks_.on_state(RunState::Thinking);
                    if (hooks_.on_reasoning) hooks_.on_reasoning(ch.reasoning);
                }
                if (!ch.delta.empty()) {
                    if (hooks_.on_state) hooks_.on_state(RunState::Streaming);
                    if (hooks_.on_token) hooks_.on_token(ch.delta);
                }
            }, &stats);
    } else {
        reply = client_.chat(history_, tools, &stats);
    }
    if (stats.valid && hooks_.on_stats) hooks_.on_stats(stats);
    return reply;
}

void Agent::reset() {
    history_.clear();
    session_approved_.clear();
}

std::string Agent::run(const std::string& user_prompt) {
    // Retain prior turns (stateful). Seed the system prompt only if this is a
    // brand-new conversation.
    ensure_system_prompt();
    if (!log_.enabled()) log_.open(cfg_.log_path);
    log_.event("user", {{"content", user_prompt}, {"model", cfg_.model}});

    Message user_msg;
    user_msg.role = "user";
    user_msg.content = user_prompt;
    history_.push_back(user_msg);

    std::string final_reply;
    auto set_state = [this](RunState s) { if (hooks_.on_state) hooks_.on_state(s); };

    std::vector<Tool*> tools;
    for (const auto& t : registry_.tools()) tools.push_back(t.get());

    for (int iter = 0; iter < cfg_.max_tool_iterations; ++iter) {
        Message reply = chat_once(tools);
        // Persist the assistant turn (including any tool_calls).
        history_.push_back(reply);
        if (!reply.reasoning.empty())
            log_.event("reasoning", {{"content", reply.reasoning}});

        if (reply.content.empty() && !reply.tool_calls.is_null()) {
            set_state(RunState::Tooling);
            if (hooks_.on_status) hooks_.on_status("assistant requested tools");
            dispatch_tool_calls(reply.tool_calls, tools);
            continue;  // loop back to let the model consume results
        }

        // Plain-text reply: we are done.
        final_reply = reply.content;
        if (hooks_.on_assistant) hooks_.on_assistant(final_reply);
        log_.event("assistant", {{"content", final_reply}});
        break;
    }


    if (final_reply.empty()) {
        final_reply = "[agent stopped: maximum tool iterations reached]";
        log_.event("error", {{"reason", "max_tool_iterations"}});
    }
    log_.event("turn_end", {{"content", final_reply}});
    set_state(RunState::Idle);
    return final_reply;
}

} // namespace agent
