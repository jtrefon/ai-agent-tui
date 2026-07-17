# Tools

The following tools are available to you. Invoke them by name with a JSON
object of arguments matching each schema. Results are returned as text and
fed back into the conversation automatically.

When deciding which tool to use, consider:

- `search` — find things first (cheap, broad).
- `read` — look at specific content (paginated).
- `write` — change files with minimal, targeted edits.
- `bash` — run shell commands (build, test, inspect) in the workspace.

`bash` executes real commands and therefore requires explicit user approval
before each run (unless the user has granted approval for the session). Prefer
the dedicated file tools over shelling out; reach for `bash` when you need to
build, run tests, or inspect the environment. Keep commands focused and expect
that they run with the working directory set to the workspace root.
