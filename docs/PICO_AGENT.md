# Pico Local Agent Architecture (Apple Silicon / MLX)

## Objective

Design a low-latency, high-reliability local agent system with:

- Effective memory >20K tokens
- Active context <=5K tokens
- Strong tool-use reliability
- Optimized for Apple Silicon (M5, 32GB)
- Migration path from Ollama -> MLX

---

## 1. Core Principle (Non-Negotiable)

DO NOT treat context window as memory.

Instead:

```
ACTIVE CONTEXT = working set only
MEMORY = externalized + retrieved on demand
```

The current Langoustine system stuffs SOUL.md + USER.md + MEMORY.md + skills + session
history into a 32KB system prompt, producing 8K-15K token contexts that balloon to 350K+
over multi-tool heartbeat turns. This caused today's crash: Ollama choked on a 350K token
context, the device hung, and the watchdog couldn't recover.

The Pico Agent inverts this: keep the LLM context tiny (<5K), externalize everything else,
and retrieve only what's needed per turn.

---

## 2. Memory Layers

### Layer 1: Working Memory (Ephemeral)
- **Size:** 1K-2K tokens
- **Lifetime:** Single turn or tool chain
- **Contents:**
  - Current task / user query
  - Recent tool outputs (last 1-2 only)
  - Current plan (if multi-step)
- **Implementation:** In-memory struct on ESP32 or Mac-side agent

### Layer 2: Episodic Memory (Vector DB)
- **Size:** Unbounded (stored on Mac, indexed)
- **Stores:**
  - Past tasks + outcomes
  - Tool results (summarized)
  - Errors + recovery actions
  - User preferences learned in conversation
- **Retrieval:** Top-K similarity search, inject 500-1500 tokens
- **Implementation:** SQLite + embeddings on Mac (mlx-embeddings or similar)
- **ESP32 interface:** HTTP call to Mac-side retrieval endpoint

### Layer 3: Semantic Memory (Structured)
- **Format:** JSON / key-value (never raw text)
- **Stores:**
  - Tool schemas (compressed, only active tools per query)
  - System rules (distilled, not SOUL.md verbatim)
  - User profile (structured facts, not prose)
  - Skill definitions (indexed by intent, loaded on demand)
- **NEVER embed raw files -> always structured extraction**

### Layer 4: Compressed History
- **Size:** 300-800 tokens
- **Rolling summary** of prior conversation steps
- **Updated every N turns** by a cheap summarizer call
- **Replaces:** Current 15-message session history approach

---

## 3. Context Composition (Target: 3K-5K tokens)

```
+----------------------------------------------+
| SYSTEM PROMPT (compact)           ~500 tokens |
|   - Role identity (2 sentences)               |
|   - Response constraints (3 bullets)          |
|   - Current time + timezone                   |
+----------------------------------------------+
| RETRIEVED MEMORY                ~500-1500 tok |
|   - Episodic: top-K relevant memories         |
|   - Semantic: user profile facts for query    |
|   - Compressed history (if continuing convo)  |
+----------------------------------------------+
| ACTIVE TOOLS (query-specific)   ~800-1500 tok |
|   - Only tools relevant to this query         |
|   - Selected by intent classifier             |
|   - Max 4-6 tool schemas per call             |
+----------------------------------------------+
| USER MESSAGE                     ~200-500 tok |
|   - Current query                             |
|   - Attached context (if any)                 |
+----------------------------------------------+
| WORKING MEMORY                   ~500-800 tok |
|   - Previous tool result (if mid-chain)       |
|   - Current plan step                         |
+----------------------------------------------+
= TOTAL: 2500-4800 tokens active context
```

### Tool Selection Strategy

Current system sends ALL 15+ tool schemas (~3K tokens) every call. Pico selects only
relevant tools per query:

| Query Type | Tools Loaded | Tokens |
|------------|-------------|--------|
| Simple chat | none | 0 |
| Weather | get_weather | ~150 |
| Briefing | web_search, read_file, send_email | ~500 |
| Device query | system_info | ~100 |
| Memory | memory_write, read_file | ~200 |
| Home control | ha_request | ~150 |

**Intent classifier** (Apfel or keyword match) determines tool set before the main
LLM call. This is essentially what the current smart routing does, but extended to
tool selection.

---

## 4. Agent Loop (Pico vs Current)

### Current Loop (Langoustine)
```
1. Build 32KB system prompt (SOUL + USER + MEMORY + skills + history)
2. Append all 15+ tool schemas
3. Send to LLM (8K-350K context depending on iteration)
4. Parse response, execute tools
5. Append tool results to messages array
6. GOTO 2 (context grows unbounded per iteration)
```

### Pico Loop
```
1. CLASSIFY intent (Apfel: <1s, ~400 tokens)
   -> Determines: tool set, memory query, complexity tier

2. RETRIEVE relevant context (~50ms local)
   -> Episodic: similarity search for query
   -> Semantic: user profile keys for intent
   -> History: compressed summary if continuing

3. COMPOSE context (CPU only, no LLM)
   -> System prompt (500 tok) + retrieved (1K) + tools (500-1500) + query (200)
   -> Total: 2.5K-4K tokens

4. EXECUTE via local LLM (qwen3.5 or mlx model)
   -> Small context = fast prefill + reliable tool calls
   -> Response: text OR tool_call

5. If tool_call:
   a. Execute tool (weather, email, etc.)
   b. SUMMARIZE result (trim to ~300 tokens if large)
   c. Update working memory with result summary
   d. GOTO 3 (recompose, don't accumulate)

6. STORE outcomes
   -> Episodic: task + result + any errors
   -> Update compressed history
```

**Key difference:** Step 5c — instead of appending raw tool results to an ever-growing
message array, summarize and recompose. Context stays bounded at every iteration.

---

## 5. Tiered LLM Routing (Enhanced)

```
                    +-----------+
    User Query ---->| Classifier|----> Simple? ----> Apfel (~1s)
                    | (Apfel)   |                    (no tools, <400 tok)
                    +-----------+
                         |
                    Tool needed?
                         |
                    +----+----+
                    |         |
               Local LLM    Cloud
            (qwen3.5/mlx)  (OpenRouter)
             ~2-10s          ~3-5s
             tool chains     complex chains
             <=3 iterations  unlimited
                    |
              Fallback on
              timeout/error
                    |
                  Cloud
```

### Tier Assignment (Enhanced from current)
| Signal | Tier | Rationale |
|--------|------|-----------|
| No tool keywords | Apfel | Conversational, no retrieval needed |
| Tool keywords (weather, time, remind) | Local LLM | Single tool call, bounded context |
| Complex keywords (briefing, research, email) | Local LLM | Multi-tool, but Pico keeps context small |
| System/heartbeat channel | Local LLM* | *NEW: Pico makes this safe locally |
| Local LLM timeout (>30s) | Cloud | Fallback only |
| Local LLM tool error | Cloud | Retry with better model |

**Critical change:** Heartbeat/system tasks currently MUST go to cloud because the 350K
context kills Ollama. With Pico's bounded context (<5K), local can handle heartbeats
safely. This eliminates the cloud dependency for morning briefings.

---

## 6. MLX Migration Path

### Phase 1: Current (Ollama + Native API) -- NOW
- Ollama 0.20 with `/api/chat` + `think:false`
- qwen3:8b or qwen3.5:9b (GGUF, Metal backend)
- ~100-165 tok/s prefill, ~17-27 tok/s decode
- Tool calling works but unreliable on large contexts
- **Bottleneck:** Context size, not model speed

### Phase 2: mlx-lm Server (Drop-in Ollama replacement)
- `mlx-lm` already installed on the Mac
- Serves OpenAI-compatible `/v1/chat/completions`
- Safetensor models (no GGUF conversion needed)
- MLX native: unified memory, GPU Neural Accelerators
- Expected: 200+ tok/s prefill, 50-100+ tok/s decode
- **ESP32 change:** Update `llm_api_url()` to point to mlx-lm endpoint
- **Model:** `mlx-community/Qwen2.5-Coder-7B-Instruct-4bit` or similar

### Phase 3: Dedicated Pico Server on Mac
- Custom Python server combining:
  - mlx-lm for inference
  - SQLite + mlx-embeddings for episodic memory
  - Intent classifier (tiny model or rule-based)
  - Context composer (no LLM needed)
  - Tool result summarizer
- Single HTTP endpoint for ESP32: `POST /pico/chat`
- ESP32 sends raw query, Pico server handles all orchestration
- ESP32 agent loop becomes a thin client

### Phase 4: Full On-Mac Agent (Future)
- Agent loop moves entirely to Mac
- ESP32 becomes I/O bridge only (mic, speaker, display, buttons)
- Mac runs: STT -> Pico Agent -> TTS pipeline
- Sub-second voice responses possible
- ESP32 overhead: WebSocket relay only

---

## 7. Episodic Memory Implementation

### Storage: SQLite on Mac

```sql
CREATE TABLE episodes (
    id INTEGER PRIMARY KEY,
    timestamp TEXT,
    query TEXT,             -- user's original query
    intent TEXT,            -- classified intent
    tools_used TEXT,        -- JSON array of tool names
    outcome TEXT,           -- success/failure/partial
    summary TEXT,           -- compressed result (<=200 tokens)
    embedding BLOB,         -- float16 vector from mlx-embeddings
    tokens_used INTEGER,    -- total tokens consumed
    latency_ms INTEGER      -- end-to-end time
);

CREATE TABLE facts (
    id INTEGER PRIMARY KEY,
    key TEXT UNIQUE,        -- e.g. "user.wake_time", "user.employer"
    value TEXT,             -- structured value
    source TEXT,            -- which episode taught this
    confidence REAL,        -- 0-1
    updated_at TEXT
);
```

### Retrieval API (Mac-side)

```
POST /pico/retrieve
{
  "query": "morning briefing",
  "top_k": 5,
  "max_tokens": 1500,
  "include_facts": true
}

Response:
{
  "episodes": [
    {"summary": "Briefing on 2026-04-14: ARM +2.3%, weather 62F...", "relevance": 0.92},
    ...
  ],
  "facts": {
    "user.wake_time": "6:00 AM weekdays",
    "user.employer": "Arm",
    "user.stocks": ["ARM"],
    ...
  },
  "compressed_history": "Last 3 turns: user asked about weather, got Pacifica forecast...",
  "total_tokens": 1200
}
```

---

## 8. Tool Result Compression

Current problem: weather API returns 12-16KB JSON, which becomes 4K+ tokens in the
message history. After 3 tool calls, context is 15K+ tokens.

### Pico Approach: Summarize Before Injecting

```
Tool Output (raw): 16KB JSON from wttr.in
                       |
                Summarizer (Apfel or template)
                       |
Compressed:    "Pacifica: 58F, overcast, wind 12mph W.
                Tomorrow: 62F, partly cloudy. Surf: 3-4ft,
                period 11s, WSW swell."  (~50 tokens)
```

**Summarization strategies by tool:**

| Tool | Strategy | Output Size |
|------|----------|-------------|
| get_weather | Template extraction (no LLM) | ~50 tokens |
| web_search | LLM summarize (Apfel) | ~100-200 tokens |
| read_file | Truncate + header | ~200 tokens |
| system_info | Template | ~30 tokens |
| send_email | Status only ("Email sent to jfield@me.com") | ~10 tokens |
| ha_request | JSON key extraction | ~30 tokens |

Most tools can use **template-based compression** (no LLM call needed). Only
web_search results genuinely need LLM summarization.

---

## 9. Performance Budget

### Current System (Ollama, unbounded context)
```
STT:        2-3s (local Whisper)
Classify:   0s (inline keyword match)
Context:    0s (pre-built 32KB prompt)
LLM iter 1: 5-15s (8K context, tool call)
Tool exec:  1-5s (weather, web search)
LLM iter 2: 10-30s (context now 15-25K, tool result injected)
LLM iter 3: 20-60s+ (context 30K+, second tool result)
TTS:        3-5s (local Kokoro)
TOTAL:      15-120s+ (grows per iteration)
```

### Pico System (bounded 5K context)
```
STT:        2-3s (local Whisper, unchanged)
Classify:   0.5-1s (Apfel, ~400 tokens)
Retrieve:   0.05s (SQLite + embedding search, local)
Compose:    0.001s (CPU string assembly)
LLM iter 1: 2-4s (3-5K context, fast prefill)
Tool exec:  1-5s (unchanged)
Compress:   0.01s (template) or 0.5s (Apfel summarize)
LLM iter 2: 2-4s (context STILL 3-5K, recomposed)
LLM iter 3: 2-4s (context STILL 3-5K)
TTS:        3-5s (local Kokoro, unchanged)
TOTAL:      10-25s (constant per iteration)
```

**Key win:** Iteration cost is CONSTANT, not growing. A 5-iteration heartbeat
briefing takes ~25s instead of 120s+.

---

## 10. Implementation Phases

### Phase A: Context Compression (ESP32 changes only)
**Effort: 1-2 days | Risk: Low**

Immediate wins without any Mac-side infrastructure:

1. **Tool result summarization** in `agent_loop.c`
   - Template-based compression for weather, system_info, email
   - Truncate web_search results to first 500 chars
   - Cap tool result injection at 1K tokens per tool

2. **Context recomposition** instead of accumulation
   - Before each LLM iteration, rebuild context from scratch
   - Include only: system prompt + last tool result + current plan
   - Drop raw message history after summarizing

3. **Tool schema selection** in agent_loop.c
   - Use existing keyword matching to select 3-5 tools per call
   - Strip unused tool schemas from the JSON

**Expected improvement:** Context capped at ~8K instead of 350K. Heartbeat
turns complete in <60s locally.

### Phase B: Episodic Memory Server (Mac-side)
**Effort: 3-5 days | Risk: Medium**

1. SQLite + embeddings server on Mac
2. `/pico/retrieve` endpoint
3. ESP32 calls retrieve before LLM call
4. Compress SOUL/USER/MEMORY into structured facts table
5. Session history -> compressed rolling summary

### Phase C: mlx-lm as LLM Backend
**Effort: 1 day | Risk: Low**

1. Start `mlx-lm` server on Mac (OpenAI-compatible)
2. Point ESP32 `local_url` to mlx-lm endpoint
3. Benchmark: expect 2-3x speed improvement over Ollama Metal
4. Launch Agent for auto-start

### Phase D: Unified Pico Server
**Effort: 1-2 weeks | Risk: Medium-High**

1. Single Python service combining: mlx-lm + memory + retrieval + compression
2. `POST /pico/chat` — ESP32 sends raw query, gets final response
3. Agent orchestration moves to Mac (faster CPU, more memory)
4. ESP32 agent_loop becomes thin client

---

## 11. Compatibility with Current System

The Pico architecture is designed as an **incremental migration**, not a rewrite:

| Component | Current | Phase A | Phase B+ |
|-----------|---------|---------|----------|
| System prompt | 32KB (SOUL+USER+MEMORY+skills) | 32KB (unchanged) | 500 tokens (compact) |
| Tool schemas | All 15+ tools every call | Keyword-selected subset | Intent-classified subset |
| Tool results | Raw, appended to messages | Compressed, capped at 1K | Summarized, recomposed |
| Session history | 15 messages, raw | 15 messages (unchanged) | Compressed rolling summary |
| Memory retrieval | None (all in prompt) | None | Episodic + semantic |
| LLM backend | Ollama native API | Ollama (unchanged) | mlx-lm |
| Agent loop | ESP32 Core 1 | ESP32 Core 1 | ESP32 (thin) or Mac |

Phase A can ship this week with zero Mac-side changes. Each subsequent phase
is independently valuable and backwards-compatible.
