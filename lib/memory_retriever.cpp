// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/experience.h"

#include <sstream>

namespace agent {

MemoryRetriever::MemoryRetriever(const MemoryStore& store)
    : store_(store) {}

std::string MemoryRetriever::build_system_prompt_suffix(
    const std::string& user_message,
    size_t max_tokens) const {
    (void)max_tokens;

    auto memories = store_.top_memories(8, user_message);
    auto skills = store_.top_skills(3, user_message);

    if (memories.empty() && skills.empty())
        return {};

    std::ostringstream out;
    out << "\n\n=== Learned Knowledge ===\n";

    if (!memories.empty()) {
        out << "\nMemories:\n";
        for (size_t i = 0; i < memories.size(); ++i) {
            out << "  " << (i + 1) << ". " << memories[i].content
                << " (confidence: " << memories[i].evidence_count << ")\n";
        }
    }

    if (!skills.empty()) {
        out << "\nSkills:\n";
        for (size_t i = 0; i < skills.size(); ++i) {
            out << "  " << (i + 1) << ". [" << skills[i].trigger_phrase << "] "
                << skills[i].content << "\n";
            for (const auto& step : skills[i].steps)
                out << "     - " << step << "\n";
        }
    }

    out << "\n=== End Learned Knowledge ===\n";
    return out.str();
}

} // namespace agent
