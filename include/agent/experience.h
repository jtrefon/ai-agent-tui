// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_EXPERIENCE_H
#define AGENT_EXPERIENCE_H

#include <memory>
#include <string>
#include <vector>

#include "agent/compressor.h"
#include "agent/llm.h"

namespace agent {

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------

// A single piece of knowledge — either informational (Memory) or
// procedural (Skill).
struct KnowledgeItem {
    std::string id;                     // hash of content for dedup
    std::string content;                // the fact or procedure description
    std::vector<std::string> tags;      // topical keywords for relevance matching
    int evidence_count = 0;             // number of compression-cycle confirmations
    int last_confirm_turn = 0;          // turn of last re-confirmation
    double score = 0.0;                 // computed rank score
};

// Declarative knowledge: "config is at path X", "project uses make".
struct Memory : KnowledgeItem {};

// Procedural knowledge: "to run tests do: make test".
struct Skill : KnowledgeItem {
    std::string trigger_phrase;         // injected only when user msg matches
    std::vector<std::string> steps;
    std::string expected_outcome;
};

// Experience extraction configuration.  Loaded from amber.conf [experience].
struct ExperienceConfig {
    bool   enabled                  = true;
    std::string store_path;         // ~/.amber/memories.json
    size_t max_memories             = 8;
    size_t max_skills               = 3;
    int    memory_promote_threshold = 3;
    int    skill_promote_threshold  = 5;
    double decay_rate               = 0.1;
    size_t max_prompt_tokens        = 500;
};

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------

// Persistent store for memories and skills.
class MemoryStore {
public:
    virtual ~MemoryStore() = default;

    virtual void upsert(const Memory& memory) = 0;
    virtual void upsert(const Skill& skill) = 0;

    // Return the top K memories/skills ranked by score, filtered for
    // relevance to user_message.
    virtual std::vector<Memory> top_memories(
        size_t k, const std::string& user_message) const = 0;
    virtual std::vector<Skill> top_skills(
        size_t k, const std::string& user_message) const = 0;

    // Apply evidence decay to all items.
    virtual void decay_all() = 0;

    // Load from / save to the backing file.
    virtual bool load(const std::string& path) = 0;
    virtual bool save(const std::string& path) const = 0;
};

// Notified when the compression pipeline has produced classification data.
// The ExperienceExtractor implements this to run asynchronously after
// the LLM responds.
class CompressionObserver {
public:
    virtual ~CompressionObserver() = default;
    virtual void on_compression_complete(
        const std::vector<Message>& history,
        const std::vector<Classification>& tags) = 0;
};

// Builds the system prompt suffix from top-ranking memories and skills.
// Runs synchronously before every LLM call.
class MemoryRetriever {
public:
    explicit MemoryRetriever(const MemoryStore& store);
    std::string build_system_prompt_suffix(
        const std::string& user_message,
        size_t max_tokens = 500) const;
private:
    const MemoryStore& store_;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<MemoryStore> make_memory_store(const ExperienceConfig& cfg);
ExperienceConfig load_experience_config(const Config& cfg);

} // namespace agent

#endif // AGENT_EXPERIENCE_H
