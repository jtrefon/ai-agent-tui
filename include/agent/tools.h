// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_TOOLS_H
#define AGENT_TOOLS_H

#include <memory>
#include <vector>
#include "agent/tool.h"

namespace agent {

// Request cancellation of any running tool.  Thread-safe atomic flag;
// the TUI sets it, the tool's execution loop polls it.
void request_tool_cancel();
bool is_tool_cancel_requested();
void clear_tool_cancel();

class JobService;  // process_* tools bind to the host-owned job service

// Built-in tool factories. Definitions live in tools/*.cpp, compiled and linked
// into libagent. Kept as factories so the registry owns unique instances.
std::unique_ptr<Tool> make_read_tool();
std::unique_ptr<Tool> make_write_tool();
std::unique_ptr<Tool> make_search_tool();
std::unique_ptr<Tool> make_bash_tool(JobService* jobs = nullptr);

// Process (background job) tool factories. They all share the caller-owned
// JobService so model-started jobs are visible and killable from the host.
std::vector<std::unique_ptr<Tool>> make_process_tools(JobService& jobs);

} // namespace agent

#endif // AGENT_TOOLS_H
