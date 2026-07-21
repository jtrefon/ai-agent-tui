// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_COMPRESSOR_H
#define AGENT_COMPRESSOR_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agent/config.h"
#include "agent/llm.h"

namespace agent {

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------

// Tag applied to every turn by the TreeShaker.
enum class Classification : std::uint8_t {
    core,    // Keep verbatim — part of the active task
    context, // Summarise — useful but not critical
    prune    // Drop entirely — stale, superseded, or side-quest
};

// One entry in the compressed archive.
struct ArchiveEntry {
    std::string turn_range;  // e.g. "3-7"
    std::string summary;
};

// Structured compressed context that replaces pruned/summarised turns.
struct CompressedContext {
    struct Task {
        std::string name;
        std::string status;               // "in_progress" | "completed"
        std::string goal;
        std::vector<std::string> decisions;
        std::vector<std::string> done;
        std::vector<std::string> pending;
    };

    std::vector<Task> tasks;
    std::vector<ArchiveEntry> archive;
    json facts;
    int version = 1;
};

// Budget fractions for the compressed prompt.
struct CompressionBudget {
    double core     = 0.30;  // recent verbatim turns
    double archive  = 0.15;  // structured summary
    double headroom = 0.50;  // model output space
};

// Compression configuration.  Loaded from amber.conf [compression] section.
struct CompressionConfig {
    double threshold        = 0.75;   // trigger at 75% utilisation
    int    min_turns        = 10;     // don't compress short conversations
    int    cooldown_turns   = 20;     // turns between compressions
    size_t summary_max_tokens = 200;  // per-summary limit
    CompressionBudget budget;
};

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

// Decides whether compression should run before the next LLM call.
class CompressionGate {
public:
    virtual ~CompressionGate() = default;
    virtual bool should_compress(const std::vector<Message>& history,
                                  const Config& agent_cfg) const = 0;
};

// Transforms a full history into a compressed prompt.
class CompressionStrategy {
public:
    virtual ~CompressionStrategy() = default;
    virtual std::vector<Message> compress(
        const std::vector<Message>& history,
        const CompressionConfig& cfg) = 0;
};

// ---------------------------------------------------------------------------
// Factory functions
// ---------------------------------------------------------------------------

std::unique_ptr<CompressionStrategy> make_compressor(
    const CompressionConfig& cfg);

std::unique_ptr<CompressionGate> make_compression_gate(
    const CompressionConfig& cfg);

CompressionConfig load_compression_config(const Config& cfg);

} // namespace agent

#endif // AGENT_COMPRESSOR_H
