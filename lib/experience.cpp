// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/experience.h"

#include <algorithm>
#include <unordered_set>

namespace agent {

// =========================================================================
// MemoryExtractor
// =========================================================================

namespace {

// Does this tool result contain information worth remembering?
bool is_memory_worthy(const Message& msg) {
    if (msg.role != "tool") return false;
    // Only capture from factual/read tools, not from side-effect tools.
    static const std::unordered_set<std::string> informative_tools{
        "read", "grep", "search", "glob", "bash"
    };
    if (!informative_tools.count(msg.name)) return false;
    // Must have meaningful content
    return msg.content.size() > 50 && msg.content.size() < 5000;
}

// Does this message look like a runnable procedure?
bool is_skill_candidate(const Message& msg) {
    if (msg.role != "assistant") return false;
    // Heuristic: assistant messages describing how to do something
    static const std::unordered_set<std::string> cues{
        "to run", "to build", "to test", "you can", "try running",
        "the command", "use", "run:", "execute"
    };
    for (const auto& cue : cues) {
        if (msg.content.find(cue) != std::string::npos) return true;
    }
    return false;
}

// Crude extraction: grab the first substantive line.
std::string first_line(const std::string& s) {
    auto nl = s.find('\n');
    if (nl == std::string::npos) return s.substr(0, 200);
    return s.substr(0, nl);
}

} // namespace

// =========================================================================
// ExperienceExtractor
// =========================================================================

class ExperienceExtractor : public CompressionObserver {
public:
    explicit ExperienceExtractor(MemoryStore& store,
                                  const ExperienceConfig& cfg)
        : store_(store), cfg_(cfg) {}

    void on_compression_complete(
        const std::vector<Message>& history,
        const std::vector<Classification>& tags) override {
        for (size_t i = 0; i < history.size(); ++i) {
            const auto& msg = history[i];
            if (tags[i] == Classification::prune) continue;

            if (is_memory_worthy(msg)) {
                Memory mem;
                mem.content = first_line(msg.content);
                mem.tags = {msg.name};
                mem.evidence_count = 1;

                // Check if we already have this memory
                auto existing = store_.top_memories(100, msg.name);
                bool found = false;
                for (const auto& e : existing) {
                    if (e.content == mem.content) {
                        found = true;
                        Memory updated = e;
                        updated.evidence_count = std::min(
                            cfg_.memory_promote_threshold * 2,
                            e.evidence_count + 1);
                        store_.upsert(updated);
                        break;
                    }
                }
                if (!found && mem.content.size() > 20) {
                    store_.upsert(mem);
                }
            }

            if (is_skill_candidate(msg)) {
                Skill sk;
                sk.content = first_line(msg.content);
                sk.trigger_phrase = "";
                sk.evidence_count = 1;

                auto existing = store_.top_skills(100, "");
                bool found = false;
                for (const auto& e : existing) {
                    if (e.content == sk.content) {
                        found = true;
                        Skill updated = e;
                        updated.evidence_count = std::min(
                            cfg_.skill_promote_threshold * 2,
                            e.evidence_count + 1);
                        store_.upsert(updated);
                        break;
                    }
                }
                if (!found) {
                    store_.upsert(sk);
                }
            }
        }

        store_.decay_all();
        if (!cfg_.store_path.empty()) {
            store_.save(cfg_.store_path);
        }
    }

private:
    MemoryStore& store_;
    ExperienceConfig cfg_;
};

} // namespace agent
