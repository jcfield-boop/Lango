#!/usr/bin/env bash
# check_mac_models.sh — health probe for the three Mac-hosted model servers.
#
# Usage:
#   ./scripts/check_mac_models.sh                # one-shot, exit 0 if all healthy
#   ./scripts/check_mac_models.sh --inference    # also runs a tiny inference call
#                                                # against each (proves the model
#                                                # itself is loaded, not just the
#                                                # HTTP server)
#   ./scripts/check_mac_models.sh --watch        # repeat every 30 s until ctrl-c
#
# Exit code: 0 = all healthy, 1 = one or more down.
#
# Service map (matches /lfs/config/SERVICES.md on the device):
#   Ollama     192.168.0.51:11434  /api/tags
#   Apfel      192.168.0.51:11435  /v1/models
#   mlx-audio  192.168.0.51:8000   /v1/models   (Whisper STT + Kokoro TTS)

set -uo pipefail

HOST="${HOST:-192.168.0.51}"
TIMEOUT="${TIMEOUT:-3}"
INFERENCE=0
WATCH=0

for arg in "$@"; do
    case "$arg" in
        --inference) INFERENCE=1 ;;
        --watch)     WATCH=1 ;;
        --help|-h)
            sed -n '1,18p' "$0" | sed 's/^#//'
            exit 0
            ;;
    esac
done

check_one() {
    local name="$1" port="$2" path="$3"
    local out
    out=$(curl -s -o /dev/null -w "%{http_code} %{time_total}" --max-time "$TIMEOUT" "http://${HOST}:${port}${path}" 2>/dev/null)
    local code="${out%% *}"
    local time="${out##* }"
    if [ "$code" = "200" ]; then
        printf "  ✅ %-10s  HTTP 200  %ss\n" "$name" "$time"
        return 0
    else
        printf "  ❌ %-10s  HTTP %s  (timeout=%ss)\n" "$name" "$code" "$TIMEOUT"
        return 1
    fi
}

inference_ollama() {
    local out
    out=$(curl -s --max-time 10 -X POST "http://${HOST}:11434/api/chat" \
        -H "Content-Type: application/json" \
        -d '{"model":"qwen3:8b","messages":[{"role":"user","content":"ping"}],"stream":false,"options":{"num_predict":4}}' \
        2>/dev/null | python3 -c "import sys,json
try:
    d = json.load(sys.stdin)
    # qwen3 may put text in message.content OR message.thinking; either proves the model loaded.
    msg = d.get('message', {})
    if msg.get('content') or msg.get('thinking') or d.get('done') is True:
        print('ok')
    else:
        print('empty')
except: print('parse fail')" 2>/dev/null)
    if [ "$out" = "ok" ]; then
        printf "  ✅ %-10s  inference round-trip OK\n" "Ollama"
        return 0
    else
        printf "  ❌ %-10s  inference: %s\n" "Ollama" "${out:-no response}"
        return 1
    fi
}

inference_apfel() {
    local out
    out=$(curl -s --max-time 10 -X POST "http://${HOST}:11435/v1/chat/completions" \
        -H "Content-Type: application/json" \
        -d '{"model":"apple-foundationmodel","messages":[{"role":"user","content":"ping"}],"max_tokens":4}' \
        2>/dev/null | python3 -c "import sys,json
try: print('ok' if json.load(sys.stdin)['choices'][0]['message']['content'] else 'no content')
except: print('parse fail')" 2>/dev/null)
    if [ "$out" = "ok" ]; then
        printf "  ✅ %-10s  inference round-trip OK\n" "Apfel"
        return 0
    else
        printf "  ❌ %-10s  inference: %s\n" "Apfel" "${out:-no response}"
        return 1
    fi
}

inference_mlx_tts() {
    local bytes
    bytes=$(curl -s --max-time 30 -X POST "http://${HOST}:8000/v1/audio/speech" \
        -H "Content-Type: application/json" \
        -d '{"model":"mlx-community/Kokoro-82M-bf16","voice":"af_heart","input":"hi"}' \
        -o /dev/null -w "%{size_download}" 2>/dev/null)
    if [ "${bytes:-0}" -gt 1000 ] 2>/dev/null; then
        printf "  ✅ %-10s  Kokoro TTS OK (%s bytes)\n" "mlx-audio" "$bytes"
        return 0
    else
        printf "  ❌ %-10s  TTS returned %s bytes\n" "mlx-audio" "${bytes:-0}"
        return 1
    fi
}

run_checks() {
    local fail=0
    echo "== $(date '+%H:%M:%S') =="
    check_one "Ollama"    11434 "/api/tags"   || fail=$((fail+1))
    check_one "Apfel"     11435 "/v1/models"  || fail=$((fail+1))
    check_one "mlx-audio" 8000  "/v1/models"  || fail=$((fail+1))

    if [ "$INFERENCE" = "1" ]; then
        echo
        inference_ollama   || fail=$((fail+1))
        inference_apfel    || fail=$((fail+1))
        inference_mlx_tts  || fail=$((fail+1))
    fi

    return "$fail"
}

if [ "$WATCH" = "1" ]; then
    while true; do
        run_checks
        echo
        sleep 30
    done
else
    run_checks
    exit "$?"
fi
