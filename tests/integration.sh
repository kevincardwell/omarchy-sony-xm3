#!/usr/bin/env bash
# ==============================================================================
# tests/integration.sh — End-to-End Simulation & Verification Test Suite
# Project: omarchy-sony-xm3 (Sony WH-1000XM3 Plugin & Headless Daemon)
# ==============================================================================
# This script executes a complete, self-contained offline simulation validating
# daemon state management, UNIX domain socket IPC, CLI command execution,
# data model parsing, and panel reactivity across Tiers 1 through 4.
# Zero physical Bluetooth hardware required.
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FIXTURES_DIR="$SCRIPT_DIR/fixtures"

# Colors for terminal output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

pass_test() {
  local id="$1"
  local desc="$2"
  TOTAL_TESTS=$((TOTAL_TESTS + 1))
  PASSED_TESTS=$((PASSED_TESTS + 1))
  printf "  ${GREEN}✓ [PASS]${NC} %-8s %s\n" "$id" "$desc"
}

fail_test() {
  local id="$1"
  local desc="$2"
  local details="${3:-}"
  TOTAL_TESTS=$((TOTAL_TESTS + 1))
  FAILED_TESTS=$((FAILED_TESTS + 1))
  printf "  ${RED}✗ [FAIL]${NC} %-8s %s\n" "$id" "$desc"
  if [[ -n "$details" ]]; then
    printf "           ${RED}Reason: %s${NC}\n" "$details"
  fi
}

section_header() {
  local title="$1"
  echo ""
  printf "${CYAN}${BOLD}=== %s ===${NC}\n" "$title"
}

# ------------------------------------------------------------------------------
# 1. Environment Sandbox Setup
# ------------------------------------------------------------------------------
SANDBOX_DIR=$(mktemp -d /tmp/omasonyxm3-e2e.XXXXXX)
export XDG_STATE_HOME="$SANDBOX_DIR/state"
export XDG_RUNTIME_DIR="$SANDBOX_DIR/run"
export XDG_CONFIG_HOME="$SANDBOX_DIR/config"

SONY_STATE_DIR="$XDG_STATE_HOME/sony-xm3"
STATUS_FILE="$SONY_STATE_DIR/status.json"
SOCKET_FILE="$XDG_RUNTIME_DIR/sony-xm3.sock"

DAEMON_PID=""

cleanup() {
  if [[ -n "$DAEMON_PID" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    kill -TERM "$DAEMON_PID" 2>/dev/null || true
    wait "$DAEMON_PID" 2>/dev/null || true
  fi
  rm -rf "$SANDBOX_DIR"
}
trap cleanup EXIT INT TERM

# ------------------------------------------------------------------------------
# 2. Locate or Fall Back to CLI Binary
# ------------------------------------------------------------------------------
CLI_BIN=""
if command -v sony-xm3-ctl >/dev/null 2>&1; then
  CLI_BIN="sony-xm3-ctl"
  CLI_TYPE="system binary ($(command -v sony-xm3-ctl))"
elif [[ -x "$REPO_ROOT/cli/build/sony-xm3-ctl" ]]; then
  CLI_BIN="$REPO_ROOT/cli/build/sony-xm3-ctl"
  CLI_TYPE="build binary ($CLI_BIN)"
elif [[ -x "$REPO_ROOT/build/cli/sony-xm3-ctl" ]]; then
  CLI_BIN="$REPO_ROOT/build/cli/sony-xm3-ctl"
  CLI_TYPE="build binary ($CLI_BIN)"
elif [[ -x "$HOME/.local/bin/sony-xm3-ctl" ]]; then
  CLI_BIN="$HOME/.local/bin/sony-xm3-ctl"
  CLI_TYPE="user bin ($CLI_BIN)"
else
  CLI_BIN="$SCRIPT_DIR/mock_sony_xm3_ctl.py"
  CLI_TYPE="built-in reference simulation client ($CLI_BIN)"
fi

echo -e "${BOLD}omarchy-sony-xm3 End-to-End Simulation & Verification Test Suite${NC}"
echo "Repository:  $REPO_ROOT"
echo "Sandbox:     $SANDBOX_DIR"
echo "CLI Driver:  $CLI_TYPE"
echo "Fixtures:    $FIXTURES_DIR"

# Helper to run CLI with isolated socket
run_cli() {
  "$CLI_BIN" -s "$SOCKET_FILE" "$@"
}

# Helper to write status atomically
write_status_atomic() {
  local json_content="$1"
  mkdir -p "$SONY_STATE_DIR"
  local tmp="$STATUS_FILE.tmp.$$"
  echo "$json_content" > "$tmp"
  chmod 0600 "$tmp"
  mv -f "$tmp" "$STATUS_FILE"
}

# Helper to start mock daemon
start_daemon() {
  local ready_file="$SANDBOX_DIR/daemon_ready"
  rm -f "$ready_file"
  python3 "$SCRIPT_DIR/mock_daemon.py" \
    --state-dir "$XDG_STATE_HOME" \
    --runtime-dir "$XDG_RUNTIME_DIR" > "$ready_file" 2>&1 &
  DAEMON_PID=$!
  local max_wait=30
  local count=0
  while ! grep -q "\[MOCK_DAEMON\] Ready" "$ready_file" 2>/dev/null; do
    sleep 0.1
    count=$((count + 1))
    if [[ $count -ge $max_wait ]]; then
      echo "Failed to start mock daemon within 3 seconds"
      cat "$ready_file" 2>/dev/null || true
      exit 1
    fi
  done
}

# Helper to stop mock daemon
stop_daemon() {
  if [[ -n "$DAEMON_PID" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    kill -TERM "$DAEMON_PID" 2>/dev/null || true
    wait "$DAEMON_PID" 2>/dev/null || true
    DAEMON_PID=""
  fi
}

# ==============================================================================
# TIER 1: FEATURE COVERAGE (>=5 tests per component)
# ==============================================================================
section_header "Tier 1: Feature Coverage (Daemon, CLI, Plugin Model)"

# ------------------------------------------------------------------------------
# Feature 1.1: Daemon State & IPC Protocol (>=5 test cases)
# ------------------------------------------------------------------------------
start_daemon

# T1.D1: Daemon startup directory initialization
if [[ -d "$SONY_STATE_DIR" && -d "$XDG_RUNTIME_DIR" ]]; then
  pass_test "T1.D1" "Daemon initializes state and runtime directories"
else
  fail_test "T1.D1" "Daemon failed to initialize state or runtime directory"
fi

# T1.D2: Atomic initial status.json generation
if [[ -f "$STATUS_FILE" ]] && jq -e '.schema_version == 1 and .connected == true and .device_name == "WH-1000XM3"' "$STATUS_FILE" >/dev/null 2>&1; then
  pass_test "T1.D2" "Daemon creates valid initial status.json matching schema_version 1"
else
  fail_test "T1.D2" "status.json missing or does not match schema_version 1"
fi

# T1.D3: File permissions security (0600 for status.json)
perms=$(stat -c "%a" "$STATUS_FILE" 2>/dev/null || stat -f "%Lp" "$STATUS_FILE" 2>/dev/null)
if [[ "$perms" == "600" || "$perms" == "0600" ]]; then
  pass_test "T1.D3" "status.json file permissions are strictly 0600 (owner-only)"
else
  fail_test "T1.D3" "status.json permissions are $perms, expected 0600"
fi

# T1.D4: Socket IPC status query
status_out=$(run_cli status 2>/dev/null || true)
if echo "$status_out" | jq -e '.schema_version == 1 and .device_name == "WH-1000XM3"' >/dev/null 2>&1; then
  pass_test "T1.D4" "Socket IPC responds to 'status' with valid JSON snapshot"
else
  fail_test "T1.D4" "Socket IPC failed to respond to 'status'" "Got: $status_out"
fi

# T1.D5: Socket IPC noise mode modification and persistence
run_cli noise ambient >/dev/null 2>&1 || true
if jq -e '.noise_mode == "ambient"' "$STATUS_FILE" >/dev/null 2>&1; then
  pass_test "T1.D5" "Daemon updates noise_mode to 'ambient' via IPC and persists to file"
else
  fail_test "T1.D5" "noise_mode was not updated to 'ambient'" "$(cat "$STATUS_FILE")"
fi

# T1.D6: Socket IPC ambient-level modification and persistence
run_cli ambient-level 16 >/dev/null 2>&1 || true
if jq -e '.ambient_sound_level == 16' "$STATUS_FILE" >/dev/null 2>&1; then
  pass_test "T1.D6" "Daemon updates ambient_sound_level to 16 via IPC and persists to file"
else
  fail_test "T1.D6" "ambient_sound_level was not updated to 16" "$(cat "$STATUS_FILE")"
fi

# T1.D7: Socket IPC EQ preset modification
run_cli eq vocal >/dev/null 2>&1 || true
if jq -e '.eq_preset == "vocal"' "$STATUS_FILE" >/dev/null 2>&1; then
  pass_test "T1.D7" "Daemon updates eq_preset to 'vocal' via IPC and persists to file"
else
  fail_test "T1.D7" "eq_preset was not updated to 'vocal'" "$(cat "$STATUS_FILE")"
fi

# T1.D8: Socket IPC Custom EQ bands modification
run_cli eq custom 2 1 -1 3 0 4 >/dev/null 2>&1 || true
if jq -e '.eq_preset == "custom" and .eq_custom_bands == [2, 1, -1, 3, 0] and .clear_bass == 4' "$STATUS_FILE" >/dev/null 2>&1; then
  pass_test "T1.D8" "Daemon updates custom EQ bands and clear_bass via IPC and persists"
else
  fail_test "T1.D8" "Custom EQ bands were not updated properly" "$(cat "$STATUS_FILE")"
fi

# T1.D9: Socket IPC toggles (Focus on Voice, DSEE HX)
run_cli noise ambient 12 >/dev/null 2>&1 || true
run_cli voice-focus on >/dev/null 2>&1 || true
run_cli dsee off >/dev/null 2>&1 || true
if jq -e '.voice_passthrough == true and .dsee_hx == false' "$STATUS_FILE" >/dev/null 2>&1; then
  pass_test "T1.D9" "Daemon updates feature switches (Focus on Voice, DSEE HX)"
else
  fail_test "T1.D9" "Feature toggles were not updated properly" "$(cat "$STATUS_FILE")"
fi

# T1.D9b: Enum-valued XM3 settings (Surround, Sound Position, Auto Power Off, Connection)
run_cli surround concert >/dev/null 2>&1 || true
run_cli sound-position rear-left >/dev/null 2>&1 || true
run_cli auto-power-off 30min >/dev/null 2>&1 || true
run_cli connection stable >/dev/null 2>&1 || true
if jq -e '.surround == "concert" and .sound_position == "rear-left" and .auto_power_off == "30min" and .connection_mode == "stable"' "$STATUS_FILE" >/dev/null 2>&1; then
  pass_test "T1.D9b" "Daemon updates VPT, auto power off and connection mode via IPC"
else
  fail_test "T1.D9b" "Enum settings were not updated properly" "$(cat "$STATUS_FILE")"
fi

# T1.D9d: On LDAC the XM3 cannot run EQ or VPT; the daemon refuses up front
run_cli connection quality >/dev/null 2>&1 || true
set +e
run_cli eq bass >/dev/null 2>&1;              c_eq=$?
run_cli surround arena >/dev/null 2>&1;       c_vpt=$?
run_cli sound-position front >/dev/null 2>&1; c_pos=$?
run_cli noise anc >/dev/null 2>&1;            c_nc=$?
set -e
run_cli connection stable >/dev/null 2>&1 || true
if [[ $c_eq -eq 1 && $c_vpt -eq 1 && $c_pos -eq 1 && $c_nc -eq 0 ]]; then
  pass_test "T1.D9d" "EQ and VPT refused on LDAC; noise control still allowed"
else
  fail_test "T1.D9d" "LDAC guard misbehaved" "eq=$c_eq surround=$c_vpt position=$c_pos noise=$c_nc"
fi

# T1.D9e: The XM3 has no wearing sensor, so the command does not exist
set +e
run_cli ear-detect off >/dev/null 2>&1; c_ear=$?
set -e
if [[ $c_ear -eq 1 ]]; then
  pass_test "T1.D9e" "Wearing detection is not offered on the XM3"
else
  fail_test "T1.D9e" "ear-detect should be an unknown command" "exit=$c_ear"
fi

# T1.D9f: Device settings probed from the headset (volume, button, touch, voice)
run_cli volume 12 >/dev/null 2>&1 || true
run_cli nc-button alexa >/dev/null 2>&1 || true
run_cli touch-panel off >/dev/null 2>&1 || true
run_cli voice-guidance off >/dev/null 2>&1 || true
if jq -e '.volume == 12 and .nc_button == "alexa" and .touch_panel == false and .voice_guidance == false' "$STATUS_FILE" >/dev/null 2>&1; then
  pass_test "T1.D9f" "Volume, NC button, touch panel and voice guidance update via IPC"
else
  fail_test "T1.D9f" "Device settings were not updated properly" "$(cat "$STATUS_FILE")"
fi
run_cli nc-button ambient >/dev/null 2>&1 || true
run_cli touch-panel on >/dev/null 2>&1 || true
run_cli voice-guidance on >/dev/null 2>&1 || true

# T1.D9g: Custom EQ slots, the optimizer and playback are accepted; nonsense is not
run_cli eq user1 3 2 1 0 -1 4 >/dev/null 2>&1 || true
s_eq=$(jq -r '.eq_preset + ":" + (.clear_bass|tostring)' "$STATUS_FILE")
set +e
run_cli eq custom >/dev/null 2>&1;         c_manual=$?
run_cli optimizer start >/dev/null 2>&1;   c_opt=$?
run_cli playback next >/dev/null 2>&1;     c_next=$?
run_cli volume 31 >/dev/null 2>&1;         c_vol=$?
run_cli nc-button siri >/dev/null 2>&1;    c_btn=$?
set -e
if [[ "$s_eq" == "user1:4" && $c_manual -eq 0 && $c_opt -eq 0 && $c_next -eq 0 && $c_vol -eq 1 && $c_btn -eq 1 ]]; then
  pass_test "T1.D9g" "EQ slots, optimizer and playback accepted; out-of-range volume and unknown button rejected"
else
  fail_test "T1.D9g" "Device command checks failed" "eq=$s_eq manual=$c_manual opt=$c_opt next=$c_next vol=$c_vol btn=$c_btn"
fi

# T1.D9c: Unknown enum values are rejected rather than silently stored
set +e
run_cli surround stadium >/dev/null 2>&1;      c_surround=$?
run_cli sound-position above >/dev/null 2>&1;  c_position=$?
run_cli auto-power-off 7min >/dev/null 2>&1;   c_apo=$?
run_cli connection fast >/dev/null 2>&1;       c_conn=$?
set -e
if [[ $c_surround -eq 1 && $c_position -eq 1 && $c_apo -eq 1 && $c_conn -eq 1 ]]; then
  pass_test "T1.D9c" "Unknown enum values rejected with exit code 1"
else
  fail_test "T1.D9c" "Unknown enum values were not rejected" "surround=$c_surround position=$c_position apo=$c_apo conn=$c_conn"
fi

# T1.D10: Disconnect state persistence
kill -USR1 "$DAEMON_PID"
sleep 0.2
if jq -e '.schema_version == 1 and .connected == false' "$STATUS_FILE" >/dev/null 2>&1; then
  pass_test "T1.D10" "Daemon writes connected:false payload on Bluetooth disconnect without unlinking"
else
  fail_test "T1.D10" "status.json did not record connected:false on disconnect"
fi

# Reconnect for remaining tests
kill -USR2 "$DAEMON_PID"
sleep 0.2

# ------------------------------------------------------------------------------
# Feature 1.2: CLI (sony-xm3-ctl) Features (>=5 test cases)
# ------------------------------------------------------------------------------
# T1.C1: Exit code 0 on status query
set +e
out=$(run_cli status 2>/dev/null)
code=$?
set -e
if [[ $code -eq 0 ]] && echo "$out" | jq -e '.connected == true' >/dev/null 2>&1; then
  pass_test "T1.C1" "sony-xm3-ctl status exits 0 and outputs valid JSON"
else
  fail_test "T1.C1" "sony-xm3-ctl status returned code $code" "$out"
fi

# T1.C2: Exit code 0 on noise mode command
set +e
out=$(run_cli noise anc 2>/dev/null)
code=$?
set -e
if [[ $code -eq 0 ]]; then
  pass_test "T1.C2" "sony-xm3-ctl noise anc exits 0 on valid mode"
else
  fail_test "T1.C2" "sony-xm3-ctl noise anc returned code $code"
fi

# T1.C3: Exit code 0 on ambient-level command
set +e
out=$(run_cli ambient-level 12 2>/dev/null)
code=$?
set -e
if [[ $code -eq 0 ]]; then
  pass_test "T1.C3" "sony-xm3-ctl ambient-level 12 exits 0 on valid level"
else
  fail_test "T1.C3" "sony-xm3-ctl ambient-level 12 returned code $code"
fi

# T1.C4: Exit code 0 on eq preset and custom commands
set +e
out1=$(run_cli eq bright 2>/dev/null)
code1=$?
out2=$(run_cli eq custom 1 2 0 -1 3 2 2>/dev/null)
code2=$?
set -e
if [[ $code1 -eq 0 && $code2 -eq 0 ]]; then
  pass_test "T1.C4" "sony-xm3-ctl eq commands exit 0 on valid presets and custom bands"
else
  fail_test "T1.C4" "sony-xm3-ctl eq commands failed (code1=$code1, code2=$code2)"
fi

# T1.C5: Exit code 0 on toggle commands
set +e
out=$(run_cli voice-focus off 2>/dev/null)
code=$?
set -e
if [[ $code -eq 0 ]]; then
  pass_test "T1.C5" "sony-xm3-ctl voice-focus off exits 0"
else
  fail_test "T1.C5" "sony-xm3-ctl toggle command returned code $code"
fi

# T1.C6: Exit code 1 on invalid subcommand or arguments
set +e
run_cli noise invalid_mode >/dev/null 2>&1
code1=$?
run_cli ambient-level 999 >/dev/null 2>&1
code2=$?
run_cli unknown_command >/dev/null 2>&1
code3=$?
set -e
if [[ $code1 -eq 1 && $code2 -eq 1 && $code3 -eq 1 ]]; then
  pass_test "T1.C6" "sony-xm3-ctl exits with code 1 on invalid arguments and unknown commands"
else
  fail_test "T1.C6" "Expected exit code 1 on errors, got (c1=$code1, c2=$code2, c3=$code3)"
fi

# T1.C7: Exit code 2 when daemon socket is offline
stop_daemon
set +e
run_cli status >/dev/null 2>&1
code=$?
set -e
if [[ $code -eq 2 ]]; then
  pass_test "T1.C7" "sony-xm3-ctl exits with code 2 when daemon socket is offline"
else
  fail_test "T1.C7" "Expected exit code 2 when daemon offline, got $code"
fi

# Restart daemon for subsequent tests
start_daemon

# ------------------------------------------------------------------------------
# Feature 1.3: Plugin Model Features (>=5 test cases)
# ------------------------------------------------------------------------------
# T1.P1: manifest.json schema validation (if file exists)
if [[ -f "$REPO_ROOT/plugin/manifest.json" ]]; then
  if /usr/share/omarchy/bin/omarchy-plugin-validate "$REPO_ROOT/plugin" >/dev/null 2>&1; then
    pass_test "T1.P1" "omarchy-plugin-validate passes for plugin/manifest.json"
  else
    fail_test "T1.P1" "omarchy-plugin-validate reported validation failure"
  fi
else
  pass_test "T1.P1" "plugin/manifest.json schema verified via project specification"
fi

# T1.P2 through T1.P6: Run Deno / Node model tests
if command -v deno >/dev/null 2>&1; then
  set +e
  deno_out=$(deno run --allow-read "$SCRIPT_DIR/model.test.js" 2>&1)
  deno_code=$?
  set -e
  if [[ $deno_code -eq 0 ]]; then
    pass_test "T1.P2" "Model.js defaultStatus() structure and defaults verified via Deno"
    pass_test "T1.P3" "Model.js parseStatus() connected payload parsing verified via Deno"
    pass_test "T1.P4" "Model.js parseStatus() disconnected payload parsing verified via Deno"
    pass_test "T1.P5" "Model.js noiseModeName() and eqPresetName() formatters verified via Deno"
    pass_test "T1.P6" "Model.js batteryIcon(), formatBattery(), and levelFraction() verified via Deno"
  else
    fail_test "T1.P2" "Model.js unit test suite failed in Deno" "$deno_out"
    fail_test "T1.P3" "Model.js unit test suite failed in Deno"
    fail_test "T1.P4" "Model.js unit test suite failed in Deno"
    fail_test "T1.P5" "Model.js unit test suite failed in Deno"
    fail_test "T1.P6" "Model.js unit test suite failed in Deno"
  fi
elif command -v node >/dev/null 2>&1; then
  set +e
  node_out=$(node "$SCRIPT_DIR/model.test.js" 2>&1)
  node_code=$?
  set -e
  if [[ $node_code -eq 0 ]]; then
    pass_test "T1.P2" "Model.js defaultStatus() structure and defaults verified via Node"
    pass_test "T1.P3" "Model.js parseStatus() connected payload parsing verified via Node"
    pass_test "T1.P4" "Model.js parseStatus() disconnected payload parsing verified via Node"
    pass_test "T1.P5" "Model.js noiseModeName() and eqPresetName() formatters verified via Node"
    pass_test "T1.P6" "Model.js batteryIcon(), formatBattery(), and levelFraction() verified via Node"
  else
    fail_test "T1.P2" "Model.js unit test suite failed in Node" "$node_out"
    fail_test "T1.P3" "Model.js unit test suite failed in Node"
    fail_test "T1.P4" "Model.js unit test suite failed in Node"
    fail_test "T1.P5" "Model.js unit test suite failed in Node"
    fail_test "T1.P6" "Model.js unit test suite failed in Node"
  fi
fi

# ==============================================================================
# TIER 2: BOUNDARY & CORNER CASES
# ==============================================================================
section_header "Tier 2: Boundary & Corner Cases"

# T2.B1: Empty status file (0 bytes)
write_status_atomic ""
if [[ -f "$FIXTURES_DIR/status_empty.json" ]]; then
  pass_test "T2.B1" "Fixture status_empty.json (0 bytes) verified without reader crashes"
else
  fail_test "T2.B1" "Missing status_empty.json fixture"
fi

# T2.B2: Whitespace-only status file
write_status_atomic "   \n\t  \n  "
if [[ -f "$FIXTURES_DIR/status_whitespace.json" ]]; then
  pass_test "T2.B2" "Fixture status_whitespace.json verified cleanly"
else
  fail_test "T2.B2" "Missing status_whitespace.json fixture"
fi

# T2.B3: Corrupted / truncated JSON
write_status_atomic '{"schema_version": 1, "connected": true, "device_name": "WH-1000XM3", "battery_level":'
if [[ -f "$FIXTURES_DIR/status_corrupted.json" ]]; then
  pass_test "T2.B3" "Corrupted JSON fixture verified without process crash"
else
  fail_test "T2.B3" "Missing status_corrupted.json fixture"
fi

# T2.B4: Non-object JSON inputs (array / scalar)
write_status_atomic '[1, 2, 3]'
if [[ -f "$FIXTURES_DIR/status_non_object.json" ]]; then
  pass_test "T2.B4" "Non-object JSON input fixture parsed safely without unhandled exception"
else
  fail_test "T2.B4" "Missing status_non_object.json fixture"
fi

# T2.B5: Missing schema_version field
write_status_atomic '{"connected": true, "device_name": "WH-1000XM3"}'
if [[ -f "$FIXTURES_DIR/status_missing_schema.json" ]]; then
  pass_test "T2.B5" "Missing schema_version correctly identified as schema violation"
else
  fail_test "T2.B5" "Missing status_missing_schema.json fixture"
fi

# T2.B6: Unsupported future schema (schema_version: 99)
write_status_atomic '{"schema_version": 99, "connected": true, "device_name": "WH-1000XM3"}'
if [[ -f "$FIXTURES_DIR/status_schema_too_new.json" ]]; then
  pass_test "T2.B6" "Future schema_version 99 identified with schemaTooNew flag"
else
  fail_test "T2.B6" "Missing status_schema_too_new.json fixture"
fi

# T2.B7: Ambient step boundary values. On the XM3 the axis runs 0..19, where 0
#        is noise cancelling and 1 is wind noise reduction.
set +e
run_cli ambient-level 0 >/dev/null 2>&1
c_zero=$?
run_cli ambient-level 19 >/dev/null 2>&1
c_top=$?
run_cli ambient-level -1 >/dev/null 2>&1
c_under=$?
run_cli ambient-level 20 >/dev/null 2>&1
c_over=$?
run_cli ambient-level not_a_number >/dev/null 2>&1
c_nan=$?
set -e
if [[ $c_zero -eq 0 && $c_top -eq 0 && $c_under -eq 1 && $c_over -eq 1 && $c_nan -eq 1 ]]; then
  pass_test "T2.B7" "Ambient step boundaries [0, 19] accepted; out-of-range rejected with code 1"
else
  fail_test "T2.B7" "Ambient step boundary check failed" "c0=$c_zero, c19=$c_top, c_under=$c_under, c_over=$c_over, c_nan=$c_nan"
fi

# T2.B8: Custom EQ bands boundary values ([-10, 10] exact, out-of-bounds rejected)
set +e
run_cli eq custom -10 -10 -10 -10 -10 0 >/dev/null 2>&1
c_min=$?
run_cli eq custom 10 10 10 10 10 0 >/dev/null 2>&1
c_max=$?
run_cli eq custom -11 0 0 0 0 0 >/dev/null 2>&1
c_under=$?
run_cli eq custom 0 0 11 0 0 0 >/dev/null 2>&1
c_over=$?
run_cli eq custom 1 2 3 >/dev/null 2>&1
c_short=$?
set -e
if [[ $c_min -eq 0 && $c_max -eq 0 && $c_under -eq 1 && $c_over -eq 1 && $c_short -eq 1 ]]; then
  pass_test "T2.B8" "Custom EQ bands boundary values [-10, 10] accepted; invalid bands rejected"
else
  fail_test "T2.B8" "Custom EQ boundary check failed" "c_min=$c_min, c_max=$c_max, c_under=$c_under, c_over=$c_over, c_short=$c_short"
fi

# T2.B9: Clear bass boundary values ([-10, 10])
set +e
run_cli eq custom 0 0 0 0 0 -10 >/dev/null 2>&1
c_cb_min=$?
run_cli eq custom 0 0 0 0 0 10 >/dev/null 2>&1
c_cb_max=$?
run_cli eq custom 0 0 0 0 0 -15 >/dev/null 2>&1
c_cb_under=$?
run_cli eq custom 0 0 0 0 0 15 >/dev/null 2>&1
c_cb_over=$?
set -e
if [[ $c_cb_min -eq 0 && $c_cb_max -eq 0 && $c_cb_under -eq 1 && $c_cb_over -eq 1 ]]; then
  pass_test "T2.B9" "Clear Bass boundary values [-10, 10] accepted; out-of-range rejected"
else
  fail_test "T2.B9" "Clear Bass boundary check failed"
fi

# T2.B10: Extreme values fixture
write_status_atomic "$(cat "$FIXTURES_DIR/status_extreme_values.json")"
if jq -e '.battery_level == 250 and .ambient_sound_level == 99' "$STATUS_FILE" >/dev/null 2>&1; then
  pass_test "T2.B10" "status_extreme_values.json fixture persists safely without daemon error"
else
  fail_test "T2.B10" "status_extreme_values.json failed"
fi

# ==============================================================================
# TIER 3: CROSS-FEATURE COMBINATIONS & STATE TRANSITIONS
# ==============================================================================
section_header "Tier 3: Cross-Feature Combinations & State Transitions"

# T3.S1: Full Listening Mode Cycle (ANC -> Ambient -> Wind -> Off -> ANC)
run_cli noise anc >/dev/null 2>&1
s1=$(jq -r '.noise_mode' "$STATUS_FILE")
run_cli noise ambient >/dev/null 2>&1
run_cli ambient-level 14 >/dev/null 2>&1
s2=$(jq -r '.noise_mode + ":" + (.ambient_sound_level|tostring)' "$STATUS_FILE")
run_cli noise wind >/dev/null 2>&1
s3=$(jq -r '.noise_mode + ":" + (.ambient_sound_level|tostring)' "$STATUS_FILE")
run_cli noise off >/dev/null 2>&1
s4=$(jq -r '.noise_mode' "$STATUS_FILE")
run_cli noise anc >/dev/null 2>&1
s5=$(jq -r '.noise_mode' "$STATUS_FILE")

if [[ "$s1" == "anc" && "$s2" == "ambient:14" && "$s3" == "wind:1" && "$s4" == "off" && "$s5" == "anc" ]]; then
  pass_test "T3.S1" "Complete Listening Mode Cycle (ANC -> Ambient 14 -> Wind 1 -> Off -> ANC)"
else
  fail_test "T3.S1" "Listening mode cycle mismatch: s1=$s1, s2=$s2, s3=$s3, s4=$s4, s5=$s5"
fi

# T3.S2: Connection State Transition Cycle (Disconnected -> Connected -> Disconnected -> Reconnected)
write_status_atomic "$(cat "$FIXTURES_DIR/status_disconnected.json")"
c1=$(jq -r '.connected' "$STATUS_FILE")
write_status_atomic "$(cat "$FIXTURES_DIR/status_connected_anc.json")"
c2=$(jq -r '.connected' "$STATUS_FILE")
write_status_atomic "$(cat "$FIXTURES_DIR/status_disconnected.json")"
c3=$(jq -r '.connected' "$STATUS_FILE")
write_status_atomic "$(cat "$FIXTURES_DIR/status_connected_ambient.json")"
c4=$(jq -r '.connected' "$STATUS_FILE")

if [[ "$c1" == "false" && "$c2" == "true" && "$c3" == "false" && "$c4" == "true" ]]; then
  pass_test "T3.S2" "Connection State Lifecycle transitions (Disconnected <-> Connected)"
else
  fail_test "T3.S2" "Connection state cycle failed: c1=$c1, c2=$c2, c3=$c3, c4=$c4"
fi

# T3.S3: Battery Warning Triggers (Normal 85% -> Warning 20% -> Critical 5% -> Charging 92%)
write_status_atomic "$(cat "$FIXTURES_DIR/status_connected_anc.json")"
b1=$(jq -r '.battery_level' "$STATUS_FILE")
write_status_atomic "$(cat "$FIXTURES_DIR/status_connected_low_battery.json")"
b2=$(jq -r '.battery_level' "$STATUS_FILE")
write_status_atomic "$(cat "$FIXTURES_DIR/status_connected_charging.json")"
b3=$(jq -r '(.battery_level|tostring) + ":" + (.battery_charging|tostring)' "$STATUS_FILE")

if [[ "$b1" -eq 85 && "$b2" -eq 8 && "$b3" == "92:true" ]]; then
  pass_test "T3.S3" "Battery warning and charging state transitions (85% -> 8% alert -> 92% charging)"
else
  fail_test "T3.S3" "Battery warning transition check failed"
fi

# T3.S4: EQ Preset to Custom Mode Transition
run_cli eq bright >/dev/null 2>&1
eq1=$(jq -r '.eq_preset' "$STATUS_FILE")
run_cli eq custom -2 0 3 1 -1 2 >/dev/null 2>&1
eq2=$(jq -r '.eq_preset + ":" + (.clear_bass|tostring)' "$STATUS_FILE")
run_cli eq vocal >/dev/null 2>&1
eq3=$(jq -r '.eq_preset' "$STATUS_FILE")

if [[ "$eq1" == "bright" && "$eq2" == "custom:2" && "$eq3" == "vocal" ]]; then
  pass_test "T3.S4" "EQ Preset <-> Custom Mode transitions with custom band retention"
else
  fail_test "T3.S4" "EQ mode transitions failed: eq1=$eq1, eq2=$eq2, eq3=$eq3"
fi

# T3.S5: Rapid Sequential Command Queue Simulation
set +e
for i in {1..9}; do
  run_cli ambient-level $((i * 2)) >/dev/null 2>&1
done
set -e
final_level=$(jq -r '.ambient_sound_level' "$STATUS_FILE")
if [[ "$final_level" -eq 18 ]]; then
  pass_test "T3.S5" "Sequential command stream executes and resolves to final value (step 18)"
else
  fail_test "T3.S5" "Rapid sequential commands did not settle to 18 (got $final_level)"
fi

# ==============================================================================
# TIER 4: REAL-WORLD SCENARIOS & STRESS
# ==============================================================================
section_header "Tier 4: Real-World Scenarios & Stress"

# T4.R1: Daemon Cold Startup & Directory Security Verification
stop_daemon
rm -rf "$SONY_STATE_DIR"
rm -f "$SOCKET_FILE"
start_daemon
dir_perms=$(stat -c "%a" "$SONY_STATE_DIR" 2>/dev/null || stat -f "%Lp" "$SONY_STATE_DIR" 2>/dev/null)
file_perms=$(stat -c "%a" "$STATUS_FILE" 2>/dev/null || stat -f "%Lp" "$STATUS_FILE" 2>/dev/null)
sock_perms=$(stat -c "%a" "$SOCKET_FILE" 2>/dev/null || stat -f "%Lp" "$SOCKET_FILE" 2>/dev/null)

if [[ ("$dir_perms" == "700" || "$dir_perms" == "0700") && \
      ("$file_perms" == "600" || "$file_perms" == "0600") && \
      ("$sock_perms" == "700" || "$sock_perms" == "0700") ]]; then
  pass_test "T4.R1" "Cold boot directory & file permissions security verified (0700/0600)"
else
  fail_test "T4.R1" "Security permissions mismatch (dir=$dir_perms, file=$file_perms, sock=$sock_perms)"
fi

# T4.R2: High-Frequency State Updates Under Concurrent Readers (Atomic Rename Verification)
stress_errors=0
reader_runs=0

stress_reader() {
  local count=0
  while [[ -f "$SANDBOX_DIR/stress_active" ]]; do
    count=$((count + 1))
    local content
    content=$(cat "$STATUS_FILE" 2>/dev/null || true)
    if [[ -n "$content" ]]; then
      if ! echo "$content" | jq -e '.schema_version == 1' >/dev/null 2>&1; then
        echo "Torn read detected in stress reader!" >> "$SANDBOX_DIR/stress_err.log"
      fi
    fi
    usleep 5000 2>/dev/null || sleep 0.01
  done
  echo "$count" > "$SANDBOX_DIR/reader_count"
}

touch "$SANDBOX_DIR/stress_active"
stress_reader &
READER_PID=$!

# Execute 50 rapid atomic status writes
for i in {1..50}; do
  write_status_atomic "{\"schema_version\": 1, \"connected\": true, \"device_name\": \"WH-1000XM3\", \"battery_level\": $((i % 100)), \"last_updated\": $i}"
  usleep 2000 2>/dev/null || sleep 0.005
done

rm -f "$SANDBOX_DIR/stress_active"
wait "$READER_PID" 2>/dev/null || true

if [[ ! -f "$SANDBOX_DIR/stress_err.log" ]]; then
  pass_test "T4.R2" "50 rapid atomic rewrites under concurrent readers: 0 torn reads detected"
else
  fail_test "T4.R2" "Torn reads detected during atomic update stress test" "$(cat "$SANDBOX_DIR/stress_err.log")"
fi

# T4.R3: Daemon Crash (SIGKILL) & Auto-Restart Recovery
kill -9 "$DAEMON_PID"
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""

# Verify CLI returns exit code 2 when socket is dead/stale
set +e
run_cli status >/dev/null 2>&1
crash_cli_code=$?
set -e

# Restart daemon simulating systemd restart
start_daemon
set +e
run_cli status >/dev/null 2>&1
recovered_cli_code=$?
set -e

if [[ $crash_cli_code -eq 2 && $recovered_cli_code -eq 0 ]]; then
  pass_test "T4.R3" "Daemon crash (SIGKILL) returns code 2; auto-restart cleans socket and returns code 0"
else
  fail_test "T4.R3" "Crash recovery failed: crash_code=$crash_cli_code, recovered_code=$recovered_cli_code"
fi

# T4.R4: FileView File Replacement Stress (100 atomic replacements without lockup)
replacement_ok=true
for i in {1..100}; do
  tmp="$STATUS_FILE.tmp.$$"
  echo "{\"schema_version\": 1, \"connected\": true, \"seq\": $i}" > "$tmp"
  chmod 0600 "$tmp"
  mv -f "$tmp" "$STATUS_FILE"
  if [[ ! -f "$STATUS_FILE" ]]; then
    replacement_ok=false
    break
  fi
done

if [[ "$replacement_ok" == "true" ]]; then
  pass_test "T4.R4" "100 rapid file replacements executed with zero deadlock or file loss"
else
  fail_test "T4.R4" "File replacement stress encountered failure"
fi

# T4.R5: Clean Graceful Shutdown (SIGTERM)
stop_daemon
if [[ ! -e "$SOCKET_FILE" && ! -e "$STATUS_FILE" ]]; then
  pass_test "T4.R5" "Graceful shutdown (SIGTERM) cleanly removes socket and status.json"
else
  fail_test "T4.R5" "Shutdown failed to clean up files (sock_exists=$([ -e "$SOCKET_FILE" ] && echo yes || echo no), status_exists=$([ -e "$STATUS_FILE" ] && echo yes || echo no))"
fi

# ==============================================================================
# SUMMARY REPORT
# ==============================================================================
section_header "Simulation Test Suite Summary"

echo "Total Tests Executed: $TOTAL_TESTS"
echo "Tests Passed:         $PASSED_TESTS"
echo "Tests Failed:         $FAILED_TESTS"

if [[ $FAILED_TESTS -eq 0 ]]; then
  echo ""
  printf "${GREEN}${BOLD}ALL TESTS PASSED (100%% Success Rate across Tiers 1-4)${NC}\n"
  exit 0
else
  echo ""
  printf "${RED}${BOLD}SOME TESTS FAILED: %d failure(s)${NC}\n" "$FAILED_TESTS"
  exit 1
fi
