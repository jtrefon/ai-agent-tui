# Context Compression Architecture

- **Status:** Draft / proposed
- **Applies to:** `lib/` (core domain), `tools/` (adapter)
- **Depends on:** `LLMClient`, `Message`/`history_`, `Config`, `json`
- **Design patterns:** Strategy, Composite, Factory, Template Method, Value Object

---

## Problem

The agent loop appends every turn — user messages, assistant replies, tool calls,
tool results, thinking blocks — to an ever-growing `history_`.  Every iteration
sends the **entire** history to the LLM.  As the conversation grows:

- **Cost** increases linearly with total token count (OpenAI/Anthropic bill per
  token).
- **Latency** increases (longer prefill per LLM call).
- **Quality** degrades at scale: models exhibit "lost in the middle" — tokens in
  the middle of a long prompt are recalled less reliably.
- **Hard limits** are reached when the context window overflows, producing
  truncation errors or silent data loss.

A 128K-token context window fills up in 300–500 turns of a typical agent loop.
Tool-heavy conversations (large command output, file reads) consume tokens even
faster.

---

## Design Goals

**Maximum compaction, minimum disruption.**

1. **Maximum compaction** — free as much context space as possible. Every token
   that does not contribute to the current active task is a candidate for
   pruning or summarisation.
2. **Minimum disruption** — the LLM's ability to continue the current task must
   be unaffected after compression. Decisions, user preferences, task state,
   and file paths are preserved with sufficient fidelity.
3. **Lossless for the active task** — the current task's core (files being
   edited, last command–result pairs, current reasoning branch) is kept
   verbatim, never summarised.
4. **Transparent** — compression runs as middleware before the LLM call. The
   agent loop does not manage it.
5. **Recoverable** — only the LLM prompt is compressed; `history_` in the
   `Agent` object and the persisted session file retain the full conversation.
6. **Triggered, not continuous** — compression runs once when approaching the
   context limit, then coasts for N turns.

---

## Architecture Layers (Hexagonal)

```
┌─────────────────────────────────────────────────────────────────┐
│                     Adapters (tools/)                           │
│                                                                 │
│  CompressionTool                                                │
│  (optional: expose /compress to the LLM)                        │
└───────────────────────────┬─────────────────────────────────────┘
                            │ calls
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                    Domain Core (lib/ + include/agent/)           │
│                                                                 │
│  ┌──────────────┐   ┌──────────────┐   ┌────────────────────┐  │
│  │ Compression  │──▶│  Pipeline    │──▶│ CompressionStrategy │  │
│  │ Gate         │   │  (Composite) │   │  (Strategy port)   │  │
│  └──────────────┘   └──────┬───────┘   └────────────────────┘  │
│                            │                                    │
│              ┌─────────────┼─────────────┐                      │
│              ▼             ▼             ▼                      │
│        ┌──────────┐ ┌──────────┐ ┌──────────────┐              │
│        │ Tree     │ │Summarizer│ │ Structured   │              │
│        │ Shaker   │ │          │ │ Compressor   │              │
│        └──────────┘ └──────────┘ └──────────────┘              │
│                                                                 │
│  Value Objects: Classification, CompressedContext, ArchiveEntry │
│  Config:        CompressionConfig (struct)                      │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ▼
                    ┌───────────────┐
                    │   LLMClient   │
                    │ (core, exists)│
                    └───────────────┘
```

### Layer Rules

| Layer | Contents | Depends on |
|-------|----------|------------|
| **Ports** (`include/agent/`) | `CompressionGate` (abstract), `CompressionStrategy` (abstract), value types, `CompressionConfig` | `Message`, `json` |
| **Core** (`lib/`) | `TreeShaker`, `Summarizer`, `StructuredCompressor`, `CompressionPipeline`, factory functions | Ports, `LLMClient` |
| **Adapter** (`tools/`) | `CompressionTool` (optional, exposes `/compress` to the LLM) | Ports |
| **Tests** (`tests/`) | Unit tests for each class | Core, test utilities |

**Dependency rule:** Core depends on Ports. Adapters depend on Ports. Neither
depends on the other. The CLI/TUI wiring (`src/amber/main.cpp`,
`tui/tui_input.cpp`) creates the pipeline via factory and injects it into the
`Agent`.

---

## Design Patterns

| Pattern | Where | Why |
|---------|-------|-----|
| **Strategy** | `CompressionStrategy` interface | Swap strategies (tree-shake, noop, truncate) without changing the pipeline |
| **Composite** | `CompressionPipeline` | Run multiple strategies or stages in sequence |
| **Factory** | `make_compressor()`, `make_compression_gate()` | Construct a fully-wired pipeline from config |
| **Template Method** | `CompressionGate::should_compress()` | Fixed algorithm, overridable predicates per subclass |
| **Value Object** | `Classification`, `CompressedContext`, `ArchiveEntry` | Immutable, no identity, passed by const ref |
| **SRP** | Each class has exactly one reason to change (see class breakdown) | |

---

## Class Breakdown (SRP)

### `CompressionGate` (port, `include/agent/compressor.h`)

**Responsibility:** Decide whether compression should run before this LLM call.

```cpp
class CompressionGate {
public:
    virtual ~CompressionGate() = default;
    virtual bool should_compress(const std::vector<Message>& history,
                                  const Config& agent_cfg) const = 0;
};

// Default implementation
class DefaultCompressionGate : public CompressionGate {
public:
    explicit DefaultCompressionGate(const CompressionConfig& cfg);
    bool should_compress(const std::vector<Message>& history,
                          const Config& agent_cfg) const override;
private:
    CompressionConfig cfg_;
    // Predicates — Template Method overridable in subclasses
    virtual bool threshold_exceeded(const std::vector<Message>& history,
                                     const Config& agent_cfg) const;
    virtual bool cooldown_elapsed() const;
    virtual bool sufficient_turns(const std::vector<Message>& history) const;
    mutable size_t last_compress_turn_ = 0;  // for cooldown tracking
};
```

**One reason to change:** trigger conditions or cooldown policy.

---

### `CompressionStrategy` (port, `include/agent/compressor.h`)

**Responsibility:** Transform a full history into a compressed prompt.

```cpp
class CompressionStrategy {
public:
    virtual ~CompressionStrategy() = default;
    virtual std::vector<Message> compress(
        const std::vector<Message>& history,
        const CompressionConfig& cfg) = 0;
};
```

**One reason to change:** the compression algorithm itself.

---

### `TreeShaker` (core, `lib/compressor.cpp`)

**Responsibility:** Classify every turn in the conversation as `core`,
`context`, or `prune`.

```cpp
enum class Classification { core, context, prune };

class TreeShaker {
public:
    TreeShaker();

    // Entry point: classify the full history.
    std::vector<Classification> classify(
        const std::vector<Message>& history) const;

private:
    // Find the index of the last uncompleted user request.
    size_t find_active_root(const std::vector<Message>& history) const;

    // Classification predicates — each is a pure function of the turn + context
    bool is_active_task_turn(size_t idx, size_t active_root) const;
    bool is_decision_or_preference(const Message& msg) const;
    bool is_stale_tool_result(const Message& msg,
                              const std::vector<Message>& history,
                              size_t idx) const;
    bool is_superseded_attempt(const Message& msg,
                                const std::vector<Message>& history,
                                size_t idx) const;
    bool is_side_quest(const Message& msg,
                        const std::vector<Message>& history,
                        size_t idx, size_t active_root) const;
    bool is_diagnostic_output(const Message& msg) const;
};
```

**One reason to change:** classification heuristics / rules.

---

### `Summarizer` (core, `lib/compressor.cpp`)

**Responsibility:** Call the LLM to generate a short summary for a segment
of turns flagged as `context`.

```cpp
class Summarizer {
public:
    explicit Summarizer(LLMClient& client);

    // Summarise a contiguous segment of turns.  Returns 1-3 sentences.
    // If the LLM call fails, returns the original text verbatim (fail-safe).
    std::string summarize(const std::vector<Message>& segment) const;

private:
    LLMClient& client_;
    std::string build_prompt(const std::vector<Message>& segment) const;
};
```

**One reason to change:** summarisation prompt or model selection.

---

### `StructuredCompressor` (core, `lib/compressor.cpp`)

**Responsibility:** Assemble classified turns into a `CompressedContext` value
object, then format it into a replacement message for the LLM prompt.

```cpp
struct CompressedContext {
    struct Task {
        std::string name;
        std::string status;    // "in_progress" | "completed"
        std::string goal;
        std::vector<std::string> decisions;
        std::vector<std::string> done;
        std::vector<std::string> pending;
    };

    std::vector<Task> tasks;
    std::vector<ArchiveEntry> archive;
    json facts;                     // extracted preferences, constants
    int version = 1;
};

struct ArchiveEntry {
    std::string turn_range;         // e.g. "3-7"
    std::string summary;
};

class StructuredCompressor {
public:
    // Classify → summarise → assemble → serialise
    std::vector<Message> compress(
        const std::vector<Message>& history,
        const std::vector<Classification>& tags,
        const CompressionConfig& cfg,
        Summarizer& summarizer) const;

private:
    CompressedContext build_context(
        const std::vector<Message>& history,
        const std::vector<Classification>& tags,
        const CompressionConfig& cfg,
        Summarizer& summarizer) const;
    std::vector<ArchiveEntry> build_archive(
        const std::vector<Message>& history,
        const std::vector<Classification>& tags,
        const CompressionConfig& cfg,
        Summarizer& summarizer) const;
    json extract_facts(const std::vector<Message>& history,
                        const std::vector<Classification>& tags) const;
    // Serialize CompressedContext into a single assistant message.
    Message format_prompt(const CompressedContext& ctx,
                          const std::vector<Message>& core_turns) const;
    // Trim core turns + compressed context to fit within token budget.
    std::vector<Message> fit_budget(
        const std::vector<Message>& core_turns,
        const std::vector<Message>& compressed_msg,
        const CompressionConfig& cfg) const;
};
```

**One reason to change:** output format or token budget policy.

---

### `CompressionPipeline` (core, `lib/compressor.cpp`)

**Responsibility:** Sequence the three stages and handle errors.

```cpp
class CompressionPipeline : public CompressionStrategy {
public:
    CompressionPipeline(std::unique_ptr<TreeShaker> shaker,
                         std::unique_ptr<Summarizer> summarizer,
                         std::unique_ptr<StructuredCompressor> compressor);

    std::vector<Message> compress(
        const std::vector<Message>& history,
        const CompressionConfig& cfg) override;

private:
    std::unique_ptr<TreeShaker> shaker_;
    std::unique_ptr<Summarizer> summarizer_;
    std::unique_ptr<StructuredCompressor> compressor_;
};
```

**One reason to change:** the sequencing of stages (add/remove/reorder).

---

## Value Objects

### `Classification` (`include/agent/compressor.h`)

```cpp
enum class Classification : std::uint8_t { core, context, prune };
```

### `CompressionConfig` (`include/agent/compressor.h`)

```cpp
struct CompressionConfig {
    double threshold     = 0.75;    // trigger at 75% of context window
    int    min_turns     = 10;      // don't compress short conversations
    int    cooldown_turns = 20;     // wait N turns between compressions
    size_t summary_max_tokens = 200; // per-summary limit

    struct Budget {
        double core      = 0.30;   // recent verbatim turns
        double archive   = 0.15;   // structured summary
        double headroom  = 0.50;   // model output space
    } budget;
};
```

---

## Data Flow

```
Agent::chat_once(user_input):
                    │
                    ▼
   should_compress(history_)? ──no──▶ build_prompt(history_, user_input)
                    │
                   yes
                    ▼
   TreeShaker::classify(history_)
    → vector<Classification>
                    │
                    ▼
   StructuredCompressor::compress(history_, tags, cfg, summarizer)
    ├── build_context()          // assemble compressed context
    │   ├── build_archive()      // walk context-tagged turns
    │   │   └── Summarizer::summarize(segment)  // per-archive entry
    │   ├── extract_facts()      // scan decisions, preferences
    │   └── format_prompt()      // serialise to replacement message
    └── fit_budget()             // trim to token budget
                    │
                    ▼
   build_prompt(core_turns + compressed_msg, user_input)
                    │
                    ▼
               LLM call
                    │
                    ▼
          append response to history_
```

---

## Error Handling

| Failure | Behaviour | Rationale |
|---------|-----------|-----------|
| LLM summary call fails | Use segment verbatim (no summarisation) | Fail-safe: keeps all context, loses compaction only |
| Compressed context exceeds budget | Walk forward through archive, discard oldest entries until fit | Newer context is more relevant |
| Classification produces no `prune` | Skip compression entirely (return original history) | No savings, no disruption |
| Gate check throws | Log warning, skip compression | Never let middleware break the agent loop |
| `make_compressor()` config invalid | Return `NoopStrategy` (passthrough) | Graceful degradation |

---

## Factory Functions (`lib/compressor.cpp`)

```cpp
// Construct the default compression strategy from config.
// Returns NoopStrategy if compression is disabled in config.
std::unique_ptr<CompressionStrategy> make_compressor(
    LLMClient& client, const CompressionConfig& cfg);

// Construct the compression gate from config.
// Returns NeverCompressGate if compression is disabled.
std::unique_ptr<CompressionGate> make_compression_gate(
    const CompressionConfig& cfg);

// Load CompressionConfig from the agent Config (which reads amber.conf).
CompressionConfig load_compression_config(const Config& cfg);
```

---

## Integration Points

### Where compression enters the agent loop (`lib/agent.cpp`)

```cpp
// Agent is constructed with optional compression middleware:
Agent::Agent(const Config& cfg, ToolRegistry& registry,
             AgentHooks hooks,
             std::unique_ptr<CompressionStrategy> compressor,
             std::unique_ptr<CompressionGate> gate)
  : compressor_(std::move(compressor))
  , gate_(std::move(gate)) {}

// Inside chat_once(), before the LLM call:
Message Agent::chat_once(const std::vector<Tool*>& tools, bool display) {
    auto prompt_msgs = history_;
    if (gate_ && gate_->should_compress(history_, cfg_)) {
        prompt_msgs = compressor_->compress(history_, load_compression_config(cfg_));
    }
    // ... build request, call LLM, return ...
}
```

### Where wiring happens (`src/amber/main.cpp`, `tui/tui_main.cpp`)

```cpp
auto cfg = load_config("amber.conf");
auto comp_cfg = load_compression_config(cfg);
auto gate = make_compression_gate(comp_cfg);
auto compressor = make_compressor(client, comp_cfg);

Agent agent(cfg, registry, hooks,
            std::move(compressor), std::move(gate));
```

---

## Testing Strategy

| Test | What it covers | How |
|------|---------------|-----|
| `TreeShaker::classify` empty history | Edge case: no turns | Expect empty classification |
| `TreeShaker::classify` single turn | Edge case: trivial conversation | Expect `{core}` |
| `TreeShaker::classify` multi-task | Active task detection | Create history with two tasks; only `core` for active |
| `TreeShaker::classify` stale output | Prune detection | Tool output followed by superseding call → `prune` |
| `TreeShaker::classify` side quest | Branch discrimination | Unrelated tool calls after answer → `context` |
| `DefaultCompressionGate` below threshold | Trigger logic | History < 75% → false |
| `DefaultCompressionGate` above threshold | Trigger logic | History > 75% + cooldown met → true |
| `DefaultCompressionGate` cooldown active | Trigger logic | Second call within 20 turns → false |
| `Summarizer` LLM failure | Error handling | Mock client returns error → returns original text |
| `StructuredCompressor::fit_budget` over limit | Token budget | Archive trimmed to fit |
| `CompressionPipeline` end-to-end | Integration | Full pipeline returns valid message array |
| `make_compressor` disabled config | Factory | Returns NoopStrategy |

The LLM summarizer is mocked via `LLMClient::parse_models` /
`merge_server_info` (same pattern as existing tests).  No real network calls
in unit tests.

---

## File Map

| File | SRP |
|------|-----|
| `include/agent/compressor.h` | Ports (`CompressionGate`, `CompressionStrategy`), value types, `CompressionConfig` |
| `lib/compressor.cpp` | TreeShaker, Summarizer, StructuredCompressor, CompressionPipeline |
| `lib/compressor_factory.cpp` | Factory functions, config loading |
| `tools/compressor_tool.cpp` | Adapter: optional `/compress` tool (lets LLM request compression) |
| `tests/compressor_test.cpp` | Unit tests for all classes |

---

## Experience Extraction — Memories & Skills

### Why This Lives at the Compression Layer

The compression pipeline already does the expensive thing — a full O(n) scan
of the conversation history.  Adding "what did we learn here?" is the same
scan with one extra classification bucket.  The cost is already paid.

A second, subtler reason: **prompt cache invalidation.**  When a new memory
or skill enters the system prompt, the LLM API's prompt cache (Anthropic,
OpenAI) sees a new prefix and writes a fresh cache entry.  The old cache
slot is evicted.  This is *good* — the agent is re-armed with new knowledge
at the same cost it would have paid anyway to write the previous cache slot.
No extra API calls, no deliberate eviction logic.

| Aspect | During compression | Ad-hoc (mid-turn tool) |
|--------|-------------------|------------------------|
| Scan cost | Already paid | Extra LLM call + token cost |
| Context | Full history in view | Only what the model noticed |
| Distraction | Zero (async after LLM) | Interrupts workflow |
| Cache benefit | Natural eviction + refresh | Extra write for no reason |

### Memory vs Skill

| | Memory | Skill |
|---|---|---|
| Nature | Declarative / informational | Procedural / executable |
| Content | "Project config is at `include/agent/agent.h`" | "To run tests: `make test`, expect 87 passed" |
| Trigger | Always relevant (topical) | Only injected when user's message matches trigger phrase |
| Example | "Build output lands in repo root, not build/" | "Debug: compile with `make CXXFLAGS='-O0 -g'` then `gdb amber`" |
| Promotion threshold | 3 confirmations | 5 confirmations |
| Evidence decay rate | 0.1 per cycle without re-confirmation | 0.2 per cycle without re-confirmation |
| Max in prompt | 5-8 | 2-3 |

**Memory** answers "what/where."  **Skill** answers "how."

### Evidence Model: How Things Stick

Nothing is committed on first sight.  Every memory and skill goes through
a staged pipeline that mirrors Bayesian updating:

```
                   ┌────────────┐
                   │ Raw scan   │  compression cycle finds a candidate
                   └─────┬──────┘
                         │
                         ▼
                   ┌────────────┐
                   │ Candidate  │  evidence_count = 1  (not injected)
                   └─────┬──────┘
                         │
               ┌─────────┴─────────┐
               │                   │
               ▼                   ▼
     next cycle confirms    next cycle silent
               │                   │
               ▼                   ▼
     evidence_count++       evidence_count unchanged
               │                   │
               ▼                   ▼
     if ≥ threshold ──► PROMOTED   decay -= rate per cycle
     (injected into prompt)        if ≤ 0 ──► ARCHIVED
                                   (stored but not injected)
```

Concrete example with memory threshold=3:

| Cycle | Observation | Evidence | Status |
|-------|-------------|----------|--------|
| 5 | "Build uses make" | 1 | Candidate |
| 8 | (not mentioned) | 0.9 | Candidate (decayed) |
| 12 | "Run make clean && make" | 1.9 | Candidate |
| 15 | "make test runs 87 tests" | 2.9 | Candidate |
| 20 | "make lint runs clang-tidy" | 3.9 | **PROMOTED** — injected |
| ... | (stable, confirmed periodically) | 3.9+ | Injected |

After promotion, the evidence count continues to tick up on re-confirmation
(which keeps the memory competitive in slot ranking) and decays when absent.
A promoted memory never falls back to candidate — it stays in the store
indefinitely but can be displaced from the active prompt slot by higher-scored
items.

### How "Use" Is Tracked — Or Rather, Why It Isn't

We do **not** track whether the LLM read or acted on a memory.  That would
require instrumenting the model's internal state — a research problem, not
an engineering one.

Instead we track **re-confirmation at extraction time.**  A memory
accumulates evidence when the ExperienceExtractor observes the same fact
in *different compression cycles*.  Decay happens when cycles pass without
that fact reappearing in the conversation.

This means:
- **One-off facts** (evidence ≤2) never reach promotion — they can't pollute.
- **Robust patterns** (evidence ≥3) persist through repeated real-world
  confirmation by the user and the agent.
- **Stale facts** decay because the conversation has moved on — people stop
  talking about the old project structure once it's settled.
- **Revival** is possible: if the old fact becomes relevant again, the next
  compression cycle re-confirms it and its score jumps back up.

### Slot Budget: Preventing Context Trashing

The system prompt has a fixed memory/skill block.  Selection is competitive:

```
prompt_memories = sort_by_score(all_promoted_memories)
                   .filter(topical_match(user_message))  // keyword relevance
                   .take(K_MEMORIES)                      // K=5-8

prompt_skills   = sort_by_score(all_promoted_skills)
                   .filter(trigger_match(user_message))   // phrase match
                   .take(K_SKILLS)                        // K=2-3
```

**Score formula:**

```
score = evidence_count × weight_evidence
      + topical_relevance  × weight_topical
      + freshness          × weight_freshness
```

Where:
- `evidence_count` = number of confirmations (capped at 10)
- `topical_relevance` = keyword overlap with current user message (0.0–1.0)
- `freshness` = 1.0 if confirmed within last 20 turns, decays linearly to 0
- `weight_evidence` = 0.5, `weight_topical` = 0.3, `weight_freshness` = 0.2

When a new promoted memory enters the top-K, the lowest-scoring existing
item is demoted from the prompt.  It stays in storage and can compete again
next turn.

### Architecture Placement

Experience extraction is **not** in the synchronous compression pipeline.
It runs **asynchronously** after the LLM responds, triggered by an event:

```
LLM call N:
  ┌─────────────────────────────────────────────────────────────────────┐
  │ 1. [sync] MemoryRetriever: top-K memories/skills → system prompt   │
  │ 2. [sync] CompressionGate: should_compress?                        │
  │ 3. [sync] CompressionPipeline: TreeShaker → Compressor             │
  │ 4. [sync] Send prompt → LLM                                        │
  │ 5. [sync] LLM responds → append to history_                        │
  └─────────────────────────────────────────────────────────────────────┘
                                      │ fire event
                                      ▼
Between calls (background):
  ┌─────────────────────────────────────────────────────────────────────┐
  │ 6. [async] ExperienceExtractor: scan classified turns from step 3   │
  │ 7. [async] MemoryExtractor: extract new candidates, update evidence │
  │ 8. [async] SkillExtractor: extract new candidates, update evidence │
  │ 9. [async] Write to MemoryStore (file / DB)                        │
  └─────────────────────────────────────────────────────────────────────┘

LLM call N+1:
  ┌─────────────────────────────────────────────────────────────────────┐
  │ 10. [sync] MemoryRetriever: includes updates from steps 7-8        │
  └─────────────────────────────────────────────────────────────────────┘
```

The pipeline is:

```
CompressionPipeline (sync)          ExperienceExtractor (async)
                        │
  TreeShaker ──classify─┼──────────────────▶ ExperienceExtractor
         │              │                       │
         ▼              │                 ┌─────┴──────┐
  Compressor            │                 ▼            ▼
         │              │          Memory       Skill
         ▼              │          Extractor    Extractor
  LLM call              │                 │            │
                        │                 ▼            ▼
                        │              MemoryStore
                        │           (promotions + decay)
                        │                 │
                        ▼                 ▼
                   MemoryRetriever (sync, before next LLM call)
```

### Interfaces

```cpp
// === Value types (include/agent/experience.h) ===

struct KnowledgeItem {
    std::string id;               // hash of content for dedup
    std::string content;          // the fact or procedure
    std::vector<std::string> tags;  // topical keywords for matching
    int evidence_count = 0;        // how many compression cycles confirmed this
    int last_confirm_turn = 0;     // turn number of last confirmation
    double score = 0.0;            // computed: evidence × weight + relevance
};

struct Memory : KnowledgeItem {
    // Informational: "config is at path X"
};

struct Skill : KnowledgeItem {
    std::string trigger_phrase;   // only inject when user message matches
    std::vector<std::string> steps;
    std::string expected_outcome;
};


// === Ports (include/agent/experience.h) ===

class MemoryStore {
public:
    virtual ~MemoryStore() = default;
    virtual void upsert(const Memory& memory) = 0;
    virtual void upsert(const Skill& skill) = 0;
    virtual std::vector<Memory> top_memories(size_t k, const std::string& user_message) = 0;
    virtual std::vector<Skill> top_skills(size_t k, const std::string& user_message) = 0;
    virtual void decay_all() = 0;  // called each compression cycle
};

// Port: notifies when compression has produced classification data
class CompressionObserver {
public:
    virtual ~CompressionObserver() = default;
    virtual void on_compression_complete(
        const std::vector<Message>& history,
        const std::vector<Classification>& tags) = 0;
};


// === Core implementations (lib/experience.cpp) ===

class ExperienceExtractor : public CompressionObserver {
public:
    ExperienceExtractor(MemoryStore& store);
    void on_compression_complete(
        const std::vector<Message>& history,
        const std::vector<Classification>& tags) override;
private:
    void extract_memories(const std::vector<Message>& history,
                           const std::vector<Classification>& tags);
    void extract_skills(const std::vector<Message>& history,
                         const std::vector<Classification>& tags);
    bool is_new_information(const Message& msg) const;
    MemoryStore& store_;
};

class MemoryRetriever {
public:
    MemoryRetriever(const MemoryStore& store);
    // Called before every LLM call.
    // Returns formatted string to inject into system prompt.
    std::string build_system_prompt_suffix(
        const std::string& user_message,
        size_t max_tokens = 500) const;
private:
    const MemoryStore& store_;
};
```


### File Map (addition)

| File | SRP |
|------|-----|
| `include/agent/experience.h` | Ports (`MemoryStore`, `CompressionObserver`), value types |
| `lib/experience.cpp` | `ExperienceExtractor`, `MemoryExtractor`, `SkillExtractor` |
| `lib/memory_store.cpp` | Default `MemoryStore` adapter (JSON file–backed) |
| `lib/memory_retriever.cpp` | `MemoryRetriever` — builds system prompt suffix |
| `tests/experience_test.cpp` | Unit tests |

---

### Persistence Format (MemoryStore)

The default `MemoryStore` is backed by a JSON file at
`~/.amber/memories.json`.  Format:

```json
{
  "version": 1,
  "project_root": "/home/jack/Projects/cpp-agent",
  "memories": [
    {
      "id": "mem_2a1f8c",
      "content": "Build system is GNU make with ./configure step",
      "tags": ["build", "makefile", "configure"],
      "evidence": 4,
      "last_seen_turn": 152,
      "promoted_at": "2026-07-21T14:30:00Z"
    },
    {
      "id": "mem_3b7d09",
      "content": "Test suite is in tests/run_tests.cpp, 87 tests",
      "tags": ["tests", "testing"],
      "evidence": 3,
      "last_seen_turn": 148,
      "promoted_at": "2026-07-21T14:28:00Z"
    }
  ],
  "skills": [
    {
      "id": "skl_9c4e12",
      "content": "To run the full test suite",
      "trigger_phrase": "run tests",
      "steps": [
        "cd $AMBER_WORKSPACE",
        "make test"
      ],
      "expected_outcome": "87 passed, 0 failed",
      "tags": ["tests", "build"],
      "evidence": 6,
      "last_seen_turn": 155,
      "promoted_at": "2026-07-21T14:32:00Z"
    }
  ]
}
```

The file is read once at startup and written atomically (write to `.tmp`,
then `rename()`).  Concurrency is not an issue — only one agent process
runs per project.

---

### `amber.conf` Configuration

```ini
[compression]
# Trigger when context utilisation reaches this fraction of the model's
# context window (0.0 – 1.0).  Default: 0.75
threshold = 0.75

# Minimum conversation turns before compression is allowed.  Default: 10
min_turns = 10

# Turns to wait between compressions.  Default: 20
cooldown_turns = 20

# Token budget for the compressed archive block.  Default: 0.15 (15%)
budget_archive = 0.15

# Token budget for recent verbatim turns.  Default: 0.30 (30%)
budget_core = 0.30

# Headroom for model output.  Default: 0.50 (50%)
budget_headroom = 0.50


[experience]
# Enable memory and skill extraction.  Default: true
enabled = true

# Path to the memories JSON file.  Default: ~/.amber/memories.json
store_path = ~/.amber/memories.json

# Maximum memories injected into the system prompt.  Default: 8
max_memories = 8

# Maximum skills injected into the system prompt.  Default: 3
max_skills = 3

# Evidence threshold for promoting a memory to permanent.  Default: 3
memory_promote_threshold = 3

# Evidence threshold for promoting a skill to permanent.  Default: 5
skill_promote_threshold = 5

# Evidence decay per compression cycle without re-confirmation.  Default: 0.1
decay_rate = 0.1

# Maximum tokens the memory/skill block can occupy in the system prompt.
# Default: 500
max_prompt_tokens = 500
```

---

## Future Directions

- **Per-user profiles** — tolerate slider in `amber.conf`; aggressive prunes
  more, conservative summarises more.
- **Learned importance** — track which archived segments the LLM later asks
  about; adjust classification thresholds.
- **Incremental archive** — maintain compressed context incrementally (update
  per turn) rather than rebuilding on trigger. Reduces latency of the
  compression step itself.
- **Embedding-based retrieval** — replace keyword topical_relevance with a
  lightweight ONNX embedding model for better memory–query matching.
- **Multi-agent sharing** — inject compressed context from one session as
  `archive` in another, enabling cross-session memory without re-processing.
