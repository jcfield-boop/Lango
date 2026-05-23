#!/usr/bin/env bash
# git_push.sh — commit all session changes and push to origin
# Run from the Lango repo root: bash scripts/git_push.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Clear any stale lock from a previous crashed git process
rm -f .git/index.lock

# --- Commit 1: Samsung Frame TV tool ---
git add main/tools/tool_frame_tv.c main/tools/tool_frame_tv.h main/tools/tool_registry.c
git commit -m "feat(tools): add Samsung Frame TV AI art tool

tool_frame_tv: POSTs a prompt to the Nanoframe Mac app (port 11436),
which calls DALL-E 3, upscales to 4K, and pushes to the Frame TV via
SmartThings. Fire-and-forget 202 pattern — device replies in ~1 s while
generation continues async on the Mac (~30–90 s).

Tool description tuned for the agent: triggers on 'put/show/display/hang
a picture on the TV', 'change the art/Frame/wall', etc. Agent expands
the user's idea into a rich visual prompt with style, lighting, and mood.

URL configurable via LANG_NANOFRAME_URL in langoustine_config.h.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"

# --- Commit 2: Delete stray '-' file ---
git rm -f -- '-' 2>/dev/null || true
git commit -m "chore: remove stray '-' file (shell redirect accident)

A bare 'command > -' left a file literally named '-' in the repo root.
Not a source file — deleted.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>" || echo "(nothing to commit for '-' cleanup)"

# --- Commit 3: Max tool calls + TLS log fix + test_device HTTP switch ---
git add main/langoustine_config.h main/gateway/ws_server.c scripts/test_device.py
git commit -m "fix: raise LANG_MAX_TOOL_CALLS 2→4, fix stale HTTPS log, update tests to HTTP

langoustine_config.h:
- LANG_MAX_TOOL_CALLS: 2 → 4. TLS removal freed ~25 KB SRAM at idle;
  the existing SRAM guard still blocks spawns when heap is low.
  4 concurrent tools allows compound briefing tasks in one LLM turn.

ws_server.c:
- Fix stale log message 'Failed to start HTTPS server' → 'HTTP server'.
  Server has been plain httpd_start() on port 80 for several sessions;
  the error string was the only remaining HTTPS reference.

scripts/test_device.py:
- Replace https:// → http:// and wss:// → ws:// throughout.
- Remove s.verify=False and urllib3.disable_warnings (no longer needed).
- s.mount() updated to http:// prefix.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"

# --- Commit 4: Docs — PLAN.md + README changelog ---
git add PLAN.md README.md
git commit -m "docs: 2026-05-22 changelog + refresh PLAN.md

README.md:
- Add 2026-05-22 changelog: Frame TV tool, LANG_MAX_TOOL_CALLS raise,
  log-rotation hardening, TLS-already-gone confirmation.

PLAN.md:
- Move 'Drop TLS' and 'LANG_MAX_TOOL_CALLS 2→4' to ✅ Already done.
- Add Frame TV to P1 test checklist.
- Add P2 items: Mac-side credential proxy (NanoClaw-inspired), context
  bloat reduction (PICO_AGENT direction).
- Remove stale P2 #1 'Enable I2S' (LANG_I2S_AUDIO_ENABLED already 1).
- Promote P3 #6 weekly compaction and #7 context bloat to P3.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"

# --- Push everything ---
echo ""
echo "Pushing to origin/master..."
git push origin master
echo ""
echo "Done. All commits pushed."
git log --oneline -8
