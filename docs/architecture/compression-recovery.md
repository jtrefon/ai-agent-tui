# Compression Recovery — Architecture Review

## The Core Problem

Compression shrinks `history_` from 250K → 15K tokens.  Session
persistence writes `history_` to disk.  After compression, saving the
session writes the compressed (lossy) version.  Restoring loads the
compressed version — tool calls, detailed results, reasoning chains
are gone forever.

We need BOTH:
- **Compact persistence**: save the compressed version, restore fast
- **Full recovery**: ability to get the original conversation back

---

## Stakeholders & Concerns

| Concern | Who cares | Why |
|---------|-----------|-----|
| Session fidelity | User | Must restore exactly where we left off |
| Memory evidence | System | Evidence counts must not inflate/deflate incorrectly |
| Skill extraction | System | Skills extracted from tool sequences must survive |
| On-disk size | User | Session files should stay small |
| Restore speed | User | Restoring a compressed session should be instant |
| Decompress speed | User | Getting the full history back should be possible |

---

## Approach Evaluation

### Approach A: Archive in session file (previous proposal)

Store the original messages in a JSON `archive` field within the session.

```
session.json:
  messages: [compressed...]     ← active history (15K)
  archive:
    messages: [original...]     ← full backup (250K)
    compression: {stats}
```

**Problems found during review:**

| # | Issue | Severity |
|---|-------|----------|
| A1 | **Double compress**: second compress overwrites the first archive. Original full conversation lost. | HIGH |
| A2 | **Memory decay during compressed sessions**: ExperienceExtractor scans `history_`. If it's compressed, pruned tool results aren't visible. Existing memories sourced from those results lose evidence. | HIGH |
| A3 | **On-disk size**: archive doubles the file (250K + 15K = 265K). Net savings: zero. | MEDIUM |
| A4 | **Evidence inflation on decompress**: After decompress, the next compression cycle sees the full conversation again and re-confirms old memories. Evidence counts ratchet up without new information. | MEDIUM |

### Approach B: Sidecar archive file

Store the archive in a separate file alongside the session.

```
session_<id>.json            ← compressed messages (15K)
session_<id>.archive.json    ← original messages (250K)
```

**Problems:**

| # | Issue | Severity |
|---|-------|----------|
| B1 | Same as A1–A4 (archiving is the same, just in a different file) | HIGH |
| B2 | Two files to manage per session. Cleanup complexity (delete session = delete two files). | LOW |

### Approach C: No archive — memories replace raw history

Compression is destructive.  The original conversation is lost.
Important facts are captured as memories and skills before compression
runs.  `/decompress` loads the last autosave before compression.

```
Before compress:  ExperienceExtractor runs → facts extracted as memories
Compress:         history_ = compressed (original discarded)
Save:             writes compressed history_
Decompress:       loads most recent pre-compress autosave (if available)
```

**Problems:**

| # | Issue | Severity |
|---|-------|----------|
| C1 | **Irrecoverable detail**: if memory extraction missed something, the information is permanently lost. | HIGH |
| C2 | **Pre-compress autosave**: requires keeping an extra backup copy of the session before compression. Doubles storage. | MEDIUM |

### Approach D: Wrap — compression as metadata overlay

Don't modify `history_` at all (my read-only fix).  Instead, store the
compressed context as a **metadata overlay** that the LLM sees, while
the full `history_` stays intact for persistence.

```
history_ (always full):    [msg1, msg2, ..., msg250K]
                             ↓
LLM prompt (compressed):   [msg1, msg5, msg8, compressed_context_block]
                             ↑ computed on-the-fly before each call
Session save:              [msg1, msg2, ..., msg250K]  ← always full
```

**Problems:**

| # | Issue | Severity |
|---|-------|----------|
| D1 | **No persistent compaction**: session files stay large. 250K file stays 250K on disk. | MEDIUM |
| D2 | **Re-compression every call**: TreeShaker + Compressor run on every LLM call if the gate triggers. More CPU than cached approach. | LOW |
| D3 | **But**: session fidelity is perfect. Memories always see the full history. No evidence issues. No archive management. | NONE |

---

## Analysis

Approach D (read-only overlay) is the **safest** but doesn't give the
user compact session files.

Approach A (archive in session file) is the most feature-complete but
has real issues (A1–A4).

### Resolving the Issues

**A1 — Double compress:**
Simple rule: `/compress` is a no-op if `archive_` already exists in
memory.  User must `/decompress` first, then `/compress` again.
This is communicated in the status message.

**A2 — Memory decay during compressed sessions:**
When compression runs, the ExperienceExtractor runs BEFORE the
compression replaces `history_`.  It sees the full conversation.
Extracted memories are upserted.  After replacement, the next
compression cycle runs on the compressed history, so no NEW
memories will be extracted (because the pruned tool results aren't
visible).  Existing memories won't lose evidence unless they were
re-confirmed each cycle — but they won't be re-confirmed because
the evidence isn't there.

Fix: **Tag memories with their source session turn range.**  During
compression, mark memories sourced from pruned turns as `archived`
(not decayed, not deleted, just set aside).  When the session is
decompressed, those memories become active again.  The evidence
count stays intact.

**A3 — On-disk size:**
The archive is 250K.  The session file without archive is 15K.
Users who never decompress keep the 15K file permanently.
Users who decompress will see the file grow back to 250K.
This is a conscious choice: compact OR full, not both.

**A4 — Evidence inflation:**
Evidence is "number of compression cycles where this fact was
independently observed."  After decompress, the next compression
cycle will see the full history.  Old facts will be re-observed.
Evidence increases.  This is arguably correct — the fact IS still
true, and being re-confirmed.

But it could inflate: if the user compresses → decompresses →
compresses → decompresses repeatedly, evidence doubles each cycle.

Fix: **Session-scoped evidence.**  The MemoryStore tracks which
session IDs have confirmed each memory.  A session can only confirm
a given memory once, regardless of how many compression/decompress
cycles happen within that session.  This prevents inflation while
allowing natural cross-session accumulation.

---

## Recommended Architecture

**Approach A (archive in session file) + metadata separation:**

`history_` is sent to the LLM verbatim.  Any archive, mentions, or
internal tracking data embedded in `history_` would leak tokens and
confuse the model.  Therefore metadata lives in a separate `meta`
section of the session JSON that is **never** copied into `history_`.

### Session JSON Layout

```json
{
  "id": "1784285245039-0",
  "title": "implement context compression",
  "model": "gpt-4o",
  "created_ms": 1784285245039,
  "updated_ms": 1784312717867,
  "message_count": 42,

  "messages": [
    {"role": "user", "content": "How is this project built?"},
    {"role": "assistant", "content": "Let me check..."}
  ],

  "meta": {
    "archive": {
      "messages": [
        {"role": "tool", "content": "GNUmakefile...", "name": "read"},
        {"role": "assistant", "content": "This uses make."}
      ],
      "compressed_at": "2026-07-21T18:00:00Z",
      "stats": {
        "tokens_before": 250000, "tokens_after": 15000,
        "messages_before": 42, "messages_after": 8,
        "core": 5, "context": 30, "prune": 7
      }
    },
    "mentions": [
      {"timestamp": "2026-07-21T18:05:00Z", "turn": 28,
       "type": "memory", "id": "mem_2a1f8c"}
    ]
  }
}
```

### Data Flow

```
history_ (vector<Message>):        meta_ (json):
  [msg1, msg2, msg3, ...]           {"archive": {...}, "mentions": [...]}
       │                                      │
       ▼                                      ▼
  sent to LLM                           never sent to LLM
       │                                      │
       ▼                                      ▼
  autosave writes messages[]           autosave writes meta object
       │                                      │
       ▼                                      ▼
  load → history_                      load → meta_
```

The Agent carries a `json meta_` member alongside `history_`.
`Session::to_json()` and `from_json()` serialize both.
`chat_once()` sends only `history_` — `meta_` is never included in
the LLM prompt.

### Implementation

**Agent changes:**
```cpp
// include/agent/agent.h
json meta_;  // archive, mentions, internal tracking — never sent to LLM
bool has_archive() const;
bool decompress();
```

**Session serialisation:**
```cpp
// Session::to_json()
{"id": ..., "messages": ..., "meta": meta_}

// Session::from_json()
s.meta_ = j.value("meta", json::object());
```

**chat_once()** sends `history_` only — `meta_` is stripped before
the LLM call (it was never in `history_` to begin with, since it's
a separate member).

### Rules

1. `/compress` replaces `history_` with compressed messages.  Stores
   the previous `history_` in `meta_["archive"]["messages"]`.
   Sets `meta_["archive"]["compressed_at"]` and stats.

2. If `meta_["archive"]` already exists (non-null), `/compress` is
   a no-op with a status message.  User must `/decompress` first.

3. `autosave()` / `save()` writes `history_` → `messages` and
   `meta_` → `meta`.  On disk: separate sections, same file.

4. `load()` restores `messages` → `history_`, `meta` → `meta_`.
   The archive is never loaded into `history_`.

5. `/decompress` moves `meta_["archive"]["messages"]` into
   `history_` (replacing the compressed version) and clears
   `meta_["archive"]`.  Full conversation restored.

6. **Memory safety**: ExperienceExtractor runs BEFORE compression
   replaces `history_`.  Memories from pruned turns are tagged
   with their turn range.  On decompress they're re-activated.

7. **Evidence dedup**: MemoryStore tracks per-session confirmation
   (`std::set<std::string> confirming_sessions_`).  One confirmation
   per session per memory, regardless of compress/decompress cycles.

8. **Side-effect tools** (write, edit, bash) classified as `core`
   and never pruned.  Skills extracted from write-tool sequences
   survive compression.

9. **Memory archiving**: When compression prunes turns, memories
   sourced solely from those turns get an `archived` flag.
   Archived memories aren't injected by default.  Decompress
   removes the flag.

---

## Loose Ends Check

| Concern | Status |
|---------|--------|
| Session file integrity after compress | ✅ `messages` + `archive` both written atomically (`.tmp` + `rename`) |
| Restore after compress | ✅ `messages` loaded into `history_`, `archive` loaded into `archive_` |
| Restore without ever compressing | ✅ `archive` is null, `messages` is full |
| Multiple compresses | ✅ Blocked by rule 2 — must decompress first |
| Memory evidence during compressed session | ✅ Memories from pruned turns tagged `archived`, not decayed |
| Memory inflation on decompress | ✅ Per-session dedup prevents re-confirmation |
| Skill extraction from compressed history | ✅ Side-effect tools never pruned (rule 8) |
| On-disk size growth from archive | ✅ Conscious choice — 250K max |
| Decompress after restart | ✅ `archive.messages` stored in session file |
| `/compress` feedback (tokens saved) | ✅ Computed from TreeShaker stats |
| LLM never sees archive/metadata | ✅ `meta_` is separate from `history_`, never included in prompt |
| Metadata survives save/load cycle | ✅ `Session::to_json/from_json` serialize both |
| `mentions` tracking extensible | ✅ `meta_` is a free-form json object — any internal data can be added |
| Welcome mural | ✅ Permanent window 0 |
| Mouse wheel scroll | ✅ KEY_UP/DOWN for viewport |
| Ctrl+P/N history recall | ✅ Separate from scroll |
| Prompt history per window | ✅ Window.prompt_history saved in workspace.json |
