// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/debug_log.h"

#include <chrono>
#include <fstream>

namespace agent {

namespace {

// Resolve a "{ts}" placeholder in a debug-log path to a process-start stamp.
std::string resolve_debug_path(const std::string& path) {
    using namespace std::chrono;
    static const std::string stamp =
        std::to_string(duration_cast<seconds>(system_clock::now()
                                                   .time_since_epoch())
                           .count());
    std::string r = path;
    size_t pos = r.find("{ts}");
    if (pos != std::string::npos) r.replace(pos, 4, stamp);
    return r;
}

} // namespace

void debug_log(const std::string& path, const std::string& tag,
               const std::string& payload) {
    if (path.empty()) return;
    std::ofstream f(resolve_debug_path(path), std::ios::app | std::ios::binary);
    if (!f) return;
    using namespace std::chrono;
    long long ms =
        duration_cast<milliseconds>(system_clock::now().time_since_epoch())
            .count();
    f << "==== " << ms << ' ' << tag << " (" << payload.size() << "B) ====\n"
      << payload << "\n";
}

} // namespace agent
