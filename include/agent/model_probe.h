// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_MODEL_PROBE_H
#define AGENT_MODEL_PROBE_H

#include "agent/config.h"
#include "agent/llm.h"

namespace agent {

// Query the server's GET /v1/models endpoint and report the first model's id
// and context window (n_ctx). Never throws: on any transport/parse failure it
// returns a ServerInfo with ok == false.
ServerInfo probe_server(const Config& cfg);

// Parse a /v1/models JSON body into ServerInfo. Pure and network-free so the
// extraction logic can be unit-tested without a live server.
ServerInfo parse_models(const std::string& body);

} // namespace agent

#endif // AGENT_MODEL_PROBE_H
