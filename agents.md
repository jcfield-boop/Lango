# Lango Agent Codex

## Base Codex Rules

### Prompt-Dependent Agent Orchestration

For any non-trivial task involving tools, files, research, automation, generated artifacts, external actions, or stateful follow-up, use the Operational Decomposition Framework as an internal routing model before acting. Skip this for simple direct answers.

Treat each prompt as a demand signal and map it across:

**Functions:**
- **F1 Perception and Input:** search, sensing, ingestion, extraction, multimodal reading.
- **F2 Context and Memory:** session state, prior decisions, local files, user preferences, environment context.
- **F3 Intelligence and Reasoning:** planning, decision-making, synthesis, tradeoff analysis.
- **F4 Execution and Actuation:** file edits, commands, APIs, automation, OS or app control.
- **F5 Interaction and Experience:** user-facing explanations, clarifying questions, UI feedback, summaries.
- **F6 Learning and Adaptation:** recurring feedback, preference updates, model or workflow iteration.
- **F7 Safety, Trust, and Governance:** validation, privacy, policy, approvals, auditability.
- **F8 System and Platform:** tool routing, runtime orchestration, parallelization, infrastructure choices.

**Workload archetypes:**
- **W1 Ultra-light always-on:** cheap monitoring, triggers, periodic checks.
- **W2 Lightweight real-time:** low-latency triage, detection, short responses.
- **W3 Stateful interactive:** assistant workflows with context and iteration.
- **W4 Heavy cognitive or multimodal:** complex research, synthesis, code, documents, media, or analysis.
- **W5 Extreme compute or coordination:** bulk processing, many-agent comparison, broad search, large-scale evaluation.
- **W6 Control-critical or deterministic:** sending, deleting, publishing, spending, admin changes, security, legal, medical, financial, or safety-sensitive work.

**Execution domains:**
- **E1 Ambient edge:** passive, always-on, low-power local triggers.
- **E2 Interactive device edge:** user-facing local UI and immediate device context.
- **E3 Device-adjacent edge:** local workspace files, persistent session state, local scripts, local artifacts.
- **E4 Network or operator edge:** live retrieval, enterprise systems, remote services, regional offload.
- **E5 Cloud or intelligence scaling:** frontier reasoning, broad synthesis, expensive model work, large context analysis.

### Decision Policy

- If the prompt can be answered directly without tools, files, or current state, answer directly.
- If the prompt depends on a supplied artifact, local file, workbook, deck, codebase, screenshot, or live app state, inspect the real source before reasoning from assumptions.
- If the prompt depends on current or changing facts, verify them live when possible and cite or summarize the source of truth.
- If the task requires action, separate planning from execution and add an F7 validation gate before irreversible or external side effects.
- If the task is W6, require explicit user approval unless the user has already clearly authorized the exact action and the action is safe, reversible, and within scope.
- If subtasks can run independently, use F8 to route them in parallel, but keep one owner for final synthesis and conflict resolution.
- If multiple agents or tools may touch the same artifact, serialize writes and verify the final artifact state.
- Prefer the smallest capable flow. Do not over-orchestrate routine work.

### Failure Mode Checks

Before taking consequential action, check:
- **Latency instability:** Is the task time-sensitive, and would cross-domain delays make the answer stale or unusable?
- **Memory inconsistency:** Are prior memory, local artifacts, user-provided context, and current state aligned?
- **Orchestration conflict:** Are multiple tools or agents competing for the same files, state, permissions, or priorities?
- **Unsafe execution:** Could the action publish, send, delete, spend, disclose, or modify important state without enough context?
- **Governance breakdown:** Is there enough source validation, test evidence, approval trail, or accountability for the outcome?

Keep the framework mostly internal. Expose a concise plan or F/W/E mapping only when it helps the user understand a complex workflow or make a decision. Once enough context is available, act decisively and carry the work through verification.

---

## Lango Project Extensions

The sections below are project-specific overrides and extensions of the Base Codex Rules for the Langoustine ESP32 agent system.

### Brain / I/O Split (F8 System and Platform)

Claude (Cowork) and the ESP32 divide responsibility to avoid duplicate messages and keep latency low:

| Layer | Runs on | Responsibilities |
|-------|---------|-----------------|
| **Intelligence (E5)** | Claude (Cowork) | Morning briefing, ARM digest, Wirecutter deals, weekend planner, memory compaction, web search, content composition |
| **I/O (E1–E2)** | ESP32 | Email send (SMTP), Telegram send/receive, local sensors (printer, HA, NOAA buoy), audio (I2S, wake word), Frame TV art, ARM stock snapshot |

### Relay Bridge (F4/F8)

The Cowork sandbox is LAN-isolated. A Mac relay daemon bridges the gap:

```
Claude writes ──► ~/Lango/outbox.json ──► lango_relay.py ──► POST /api/relay ──► ESP32 sends
                  (Mac filesystem, E3)    (Mac daemon,         (firmware endpoint,
                                          polls every 5s)       E2 actuation)
```

**Sending from a Cowork Bash tool (F4 Actuation):**
```bash
python3 ~/Lango/scripts/lango_send.py email "Subject" "Body text"
python3 ~/Lango/scripts/lango_send.py telegram "Message text"
```

**F7 gate:** All relay actions are W6 (send / publish). Confirm message content and recipient before queuing. The relay daemon is one-way and fire-and-forget — there is no undo after `/api/relay` accepts the item.

### Task Ownership

**Claude owns (Cowork scheduled tasks — E5→E4 dispatch):**

| Task | Schedule | Output | F/W/E |
|------|----------|--------|-------|
| Morning briefing | Daily 06:20 | Email via relay | W4, E5→E3→E4 |
| Weekly ARM + PC digest | Monday 06:50 | Email via relay | W4, E5 |
| Wirecutter deals | Monday morning | Email via relay | W4, E5 |
| Weekend planner | Friday | Telegram via relay | W3, E5 |
| Memory compaction | Sunday | Writes MEMORY.md | W4, E3 |

**ESP32 owns (cron.json / HEARTBEAT.md — E1/E2):**

| Task | Schedule | Notes |
|------|----------|-------|
| `prefetch` | Every 4h | Gathers weather/markets/printer → brief_data.md |
| `armpre01` | Daily | ARM stock snapshot → arm_stock_today.md |
| `surf0002/0003` | Sat/Fri | NOAA buoy → Telegram verdict |
| `haupd001` | Daily | HA update check |
| `kupd0001` | Daily | Klipper update check |
| `tvart001` | Daily | Frame TV art generation |
| Nightly check | 22:00 (HEARTBEAT.md) | sysinfo → Telegram |
| Telegram polling | Continuous | Receive and respond to messages |

**Disabled ESP32 crons (Claude took over):** `brief001`, `wire0003`, `armnw005`, `wknd0004`.

**Scheduling rule (F2 Context):** Time-of-day checks go in `/lfs/HEARTBEAT.md` (read by `heartbeat.c` every 30 min). Weekly/one-shot jobs go in `/lfs/cron.json` (read by `cron_service.c`). Never mix them.

### On-Device LLM Routing (F3/F8)

The ESP32 agent loop implements a tiered routing model that maps directly onto the Base Codex workload archetypes:

| Signal | Tier | Workload | Rationale |
|--------|------|----------|-----------|
| No tool keywords | Apfel (~1s, ~400 tok) | W2 | Conversational, no retrieval needed |
| Tool keywords (weather, time, remind) | Ollama local | W3 | Single tool call, bounded context |
| Complex keywords (briefing, research) | Cloud direct | W4/W5 | Multi-step chain, full context needed |
| System/heartbeat channel | Cloud always | W4 | Multi-step tool chains need speed; local context may be stale |
| Local LLM timeout / error | Cloud fallback | W4 | Auto-retry, no user intervention |

**Voice (PTT/wake word)** adds `max_tokens=400` + VOICE MODE injection. Voice queries route through the EAGLE voice router (when enabled) before the standard tier chain.

**F7 gate for system channel:** Heartbeat/cron tasks MUST NOT hit Ollama without Pico-style bounded context (≤5K tokens). Until Phase A context compression ships, keep system channel pinned to cloud.

### Pico Agent Architecture (Target State)

The current ESP32 agent stuffs SOUL.md + USER.md + MEMORY.md + skills + session history into a 32KB system prompt. This causes context balloon (350K+ tokens on multi-tool heartbeat turns) and Ollama crashes.

The Pico Agent inverts this — keep LLM context tiny (<5K), externalize everything else, retrieve only what's needed per turn. This maps to F2 (externalized memory) + F8 (bounded context composition).

**Memory layers:**

| Layer | Size | Lifetime | Implementation |
|-------|------|----------|---------------|
| Working Memory (F2) | 1–2K tokens | Single turn/tool chain | In-memory struct |
| Episodic Memory (F2) | Unbounded | Persistent | SQLite + embeddings on Mac |
| Semantic Memory (F2) | Structured JSON | Persistent | facts table — never raw text |
| Compressed History (F2) | 300–800 tokens | Rolling | Summarized every N turns by Apfel |

**Pico loop (vs current accumulation):**
```
1. CLASSIFY intent (Apfel: <1s, ~400 tokens)          → F3 Reasoning
2. RETRIEVE relevant context (~50ms, SQLite local)     → F2 Context
3. COMPOSE context (<5K total — CPU only, no LLM)     → F8 Platform
4. EXECUTE via local LLM                               → F4 Execution
5. If tool_call: execute → SUMMARIZE result → recompose → GOTO 3
   (never accumulate raw tool output into message history)
6. STORE outcomes → episodic + compressed history     → F6 Learning
```

**Tool result compression targets:**

| Tool | Strategy | Output Size |
|------|----------|-------------|
| get_weather | Template extraction (no LLM) | ~50 tokens |
| web_search | Apfel summarize | ~100–200 tokens |
| read_file | Truncate + header | ~200 tokens |
| system_info | Template | ~30 tokens |
| send_email | Status only | ~10 tokens |
| ha_request | JSON key extraction | ~30 tokens |

**Migration phases:**
- **Phase A** (ESP32 only, 1–2 days): tool result compression + context recomposition + keyword-based tool schema selection. Context capped at ~8K. Zero Mac-side changes.
- **Phase B** (Mac-side, 3–5 days): SQLite + embeddings retrieval server. SOUL/USER/MEMORY → structured facts. Session history → compressed rolling summary.
- **Phase C** (1 day): `mlx-lm` as LLM backend — point `local_url` to mlx-lm. Expected 2–3× speed over Ollama.
- **Phase D** (1–2 weeks): Unified Pico server (`POST /pico/chat`). Agent orchestration moves to Mac. ESP32 becomes thin I/O client.

### F7 Safety Gates for Lango

These are always-on regardless of Base Codex workload tier:

1. **Relay actions are irreversible.** Never queue email or Telegram without confirming content and recipient. Check `outbox.json` is empty before writing new entries if previous dispatch status is unknown.
2. **OTA is W6.** `idf.py flash` and `ota_deploy.sh` require explicit user confirmation. Check `agent_loop_is_busy()` is false before initiating.
3. **LittleFS writes via `/api/file`** overwrite silently. Read the current file content first; diff before posting.
4. **cron.json edits** take effect immediately on `POST /api/file?name=cron` (calls `cron_service_reload()`). Validate JSON and job IDs before pushing.
5. **NVS writes** (`set_wifi`, `set_api_key`, etc.) survive reboots and take priority over build-time secrets. Confirm before running.
