// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/tool.h"
#include "agent/tools.h"
#include "agent/workspace.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace agent {

// bash: run a shell command inside the workspace root and return its combined
// stdout+stderr and exit status. Args:
//   command  (string, required) the command line, run via `sh -c`
//   timeout  (int, optional)    seconds before the command is killed (default 60)
//
// Safety: this tool executes arbitrary commands, so it declares
// requires_approval() == true. The agent loop will not run it unless the host
// grants approval (a TUI confirmation dialog or a CLI opt-in). It runs with the
// working directory set to the confined workspace root, in its own process
// group so a timeout can reliably kill the whole subtree.
class BashTool : public Tool {
public:
    std::string name() const override { return "bash"; }

    std::string description() const override {
        return "Run a shell command (via sh -c) in the workspace directory and "
               "return its combined stdout/stderr and exit code. Use for "
               "building, running tests, and inspecting the environment. "
               "Requires user approval before it runs.";
    }

    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"command", {{"type", "string"},
                             {"description", "Shell command to run via sh -c"}}},
                {"timeout", {{"type", "integer"},
                             {"description",
                              "Seconds before the command is killed (default 60)"}}}
            }},
            {"required", {"command"}}
        };
    }

    bool requires_approval() const override { return true; }

    std::string summarize(const json& a) const override {
        std::string cmd = (a.contains("command") && a["command"].is_string())
                              ? a["command"].get<std::string>() : "";
        if (cmd.size() > 200) cmd = cmd.substr(0, 197) + "...";
        return "run: " + cmd;
    }

    ToolResult execute(const json& a) const override {
        ToolResult r;
        if (!a.contains("command") || !a["command"].is_string() ||
            a["command"].get<std::string>().empty()) {
            r.ok = false; r.error = "missing 'command'"; return r;
        }
        std::string command = a["command"].get<std::string>();
        int timeout = static_cast<int>(a.value("timeout", 60));
        if (timeout < 1) timeout = 1;
        if (timeout > kMaxTimeout) timeout = kMaxTimeout;

        std::string cwd = Workspace::root();

        int pipefd[2];
        if (pipe(pipefd) != 0) {
            r.ok = false; r.error = "pipe failed"; return r;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(pipefd[0]); close(pipefd[1]);
            r.ok = false; r.error = "fork failed"; return r;
        }

        if (pid == 0) {
            // Child: new process group, redirect stdout+stderr to the pipe,
            // chdir into the workspace, then exec the shell.
            setpgid(0, 0);
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
            if (!cwd.empty()) { if (chdir(cwd.c_str()) != 0) _exit(127); }
            execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
            _exit(127);  // exec failed
        }

        // Parent: read output with a wall-clock deadline; kill the child's
        // process group if it overruns.
        close(pipefd[1]);
        setpgid(pid, pid);  // race-free with the child also setting it

        std::string output;
        bool timed_out = false;
        const long deadline_ms = timeout * 1000L;
        long elapsed_ms = 0;
        const int poll_ms = 50;

        int flags = fcntl(pipefd[0], F_GETFL, 0);
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

        std::array<char, 4096> buf{};
        bool child_done = false;
        while (true) {
            ssize_t n = read(pipefd[0], buf.data(), buf.size());
            if (n > 0) {
                if (output.size() < kMaxOutput)
                    output.append(buf.data(), static_cast<size_t>(n));
                continue;  // drain fast while data is flowing
            }
            if (n == 0) break;  // EOF: child closed the pipe

            // n < 0
            if (errno != EAGAIN && errno != EWOULDBLOCK) break;

            int status = 0;
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) { child_done = true; break; }

            if (elapsed_ms >= deadline_ms) {
                timed_out = true;
                kill(-pid, SIGKILL);
                break;
            }
            usleep(poll_ms * 1000);
            elapsed_ms += poll_ms;
        }

        // Reap the child (and drain any final bytes on clean exit).
        int status = 0;
        if (!child_done) {
            // Grab any last output then wait.
            ssize_t n;
            while ((n = read(pipefd[0], buf.data(), buf.size())) > 0) {
                if (output.size() < kMaxOutput)
                    output.append(buf.data(), static_cast<size_t>(n));
            }
            waitpid(pid, &status, 0);
        }
        close(pipefd[0]);

        bool truncated = output.size() >= kMaxOutput;
        if (truncated) output.resize(kMaxOutput);

        std::ostringstream out;
        out << output;
        if (!output.empty() && output.back() != '\n') out << '\n';
        if (truncated)
            out << "[output truncated at " << kMaxOutput << " bytes]\n";

        if (timed_out) {
            out << "[command timed out after " << timeout << "s and was killed]";
            r.ok = false;
            r.output = out.str();
            r.error = "timed out after " + std::to_string(timeout) + "s";
            return r;
        }

        int code = WIFEXITED(status) ? WEXITSTATUS(status)
                 : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
        out << "[exit " << code << "]";
        r.output = out.str();
        r.ok = (code == 0);
        if (!r.ok) r.error = "command exited with status " + std::to_string(code);
        return r;
    }

private:
    static constexpr int kMaxTimeout = 3600;              // 1 hour ceiling
    static constexpr size_t kMaxOutput = 64 * 1024;       // 64 KiB cap
};

std::unique_ptr<Tool> make_bash_tool() {
    return std::make_unique<BashTool>();
}

} // namespace agent
