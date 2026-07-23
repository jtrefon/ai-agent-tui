# Tools

## Result envelope

Every tool result uses the exact same form:

```
[tool=<name> args=<json> status=<status> meta=<json>]
<content>
[end]
```

| Field | Meaning |
|-------|---------|
| `name` | The tool that was called |
| `args` | Your original call arguments echoed back (compact JSON) |
| `status` | One of: `ok`, `error`, `denied`, `timeout` |
| `meta` | Structured metadata (lines, hits, exit code, etc.) |
| `<content>` | The result payload |
| `[end]` | End marker — no more output from this call |

The envelope never changes shape. Only the values differ. Parse the
header line to determine what happened; read the content for details.

## Tool categories

| Category | Tools | Expected output |
|----------|-------|-----------------|
| **Query** | `search`, `read` | Return data. Full content in envelope. |
| **Command** | `write`, `bash`, `process_*` | Return status. Summary in content. |

---

## search

Query filesystem. Use first to find symbols, definitions, and patterns.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| pattern | string | yes | Short regex or query (max 256 chars) |
| path | string | no | Directory to search (default: workspace root) |
| glob | string | no | File filter, e.g. `"*.cpp"` |
| mode | string | no | `"grep"` (default) or `"semantic"` |
| max | integer | no | Max matches (default 200) |

**Output**: matching lines with file paths and line numbers.
**When**: finding symbols, usages, definitions before reading files.

## read

Read a file with pagination.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| path | string | yes | File to read (confined to workspace) |
| offset | integer | no | Starting line, 1-based (default 1) |
| limit | integer | no | Max lines to return (default 2000) |

**Output**: lines prefixed with number. Footer indicates more available.
**When**: inspecting file contents after search identified relevant files.

## write

Edit a file with targeted `old`/`new` blocks. Use `old=""` to create.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| path | string | yes | File to edit (confined to workspace) |
| edits | array | yes | `[{old, new}, ...]` blocks to apply in order |

**Output**: count of edits applied.
**When**: after reading and planning — make the actual change.

## bash

Run a shell command. Requires interactive approval.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| command | string | yes | Shell command via `/bin/sh -c` |
| timeout | integer | no | Seconds of no output before kill (default 60) |

**Output**: combined stdout+stderr, exit code, truncation notice.
**When**: building, testing, linting, analysing, git operations.

## process_start

Start a background job. Returns immediately with a `job_id`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| command | string | yes | Shell command to run |
| timeout | integer | no | Hard lifetime in seconds (default 600) |
| idle_timeout | integer | no | Seconds idle before kill (default 30) |
| cwd | string | no | Working directory (default: workspace) |

**Output**: bare `job_id` string (pass to process_read/process_stop).
**When**: long-running commands — dev servers, watchers, builds.

## process_read

Fetch new output from a background job since the last read.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| id | string | yes | Job id from process_start |
| all | boolean | no | Return full output instead of delta |

**Output**: status line + delta output.
**When**: checking progress of a background job.

## process_stop

Terminate a background job and return its captured output.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| id | string | yes | Job id from process_start |

**Output**: "stopped" notice + captured output.
**When**: killing a job that is no longer needed.
