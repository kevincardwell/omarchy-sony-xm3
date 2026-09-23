#!/usr/bin/env bash
# ==============================================================================
# tests/integration.sh — End-to-End Simulation & Verification Test Suite
# Project: omarchy-sony-xm3 (Sony WH-1000XM3 Plugin & Headless Daemon)
# ==============================================================================
# Runs this checkout's real daemon in --mock mode (build/daemon/sony-xm3-daemon)
# against this checkout's CLI (build/cli/sony-xm3-ctl), validating daemon state
# management, UNIX domain socket IPC and CLI command execution across Tiers 1-4.
# State is read back over the socket (`status`). Zero Bluetooth hardware required.
# Model.js is covered separately by tests/model.test.js.
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

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
export XDG_RUNTIME_DIR="$SANDBOX_DIR/run"

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
# 2. Locate This Checkout's Build (the suite tests the tree, not an install)
# ------------------------------------------------------------------------------
DAEMON_BIN="${DAEMON_BIN:-$REPO_ROOT/build/daemon/sony-xm3-daemon}"
CLI_BIN="${CLI_BIN:-$REPO_ROOT/build/cli/sony-xm3-ctl}"
for bin in "$DAEMON_BIN" "$CLI_BIN"; do
  if [[ ! -x "$bin" ]]; then
    printf "${RED}Not built: %s${NC}\n" "$bin" >&2
    echo "Build first: cmake -B build -G Ninja && cmake --build build" >&2
    exit 1
  fi
done

echo -e "${BOLD}omarchy-sony-xm3 End-to-End Simulation & Verification Test Suite${NC}"
echo "Repository:  $REPO_ROOT"
echo "Sandbox:     $SANDBOX_DIR"
echo "Daemon:      $DAEMON_BIN --mock"
echo "CLI:         $CLI_BIN"

# Helper to run CLI with isolated socket
run_cli() {
  "$CLI_BIN" -s "$SOCKET_FILE" "$@"
}

# Helper to read the daemon's current state over the socket
get_status() {
  run_cli status 2>/dev/null || true
}

# Helper to start the daemon in mock mode
start_daemon() {
  local ready_file="$SANDBOX_DIR/daemon_ready"
  rm -f "$ready_file"
  "$DAEMON_BIN" --mock --runtime-dir "$XDG_RUNTIME_DIR" > "$ready_file" 2>&1 &
  DAEMON_PID=$!
  local max_wait=30
  local count=0
  while ! grep -q "\[MOCK_DAEMON\] Ready" "$ready_file" 2>/dev/null; do
    sleep 0.1
    count=$((count + 1))
    if [[ $count -ge $max_wait ]]; then
      echo "Failed to start daemon within 3 seconds"
      cat "$ready_file" 2>/dev/null || true
      exit 1
    fi
  done
}

# Helper to stop the daemon
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
section_header "Tier 1: Feature Coverage (Daemon, CLI)"

# ------------------------------------------------------------------------------
# Feature 1.1: Daemon State & IPC Protocol (>=5 test cases)
# ------------------------------------------------------------------------------
start_daemon

# T1.D1: Daemon startup runtime directory and socket
if [[ -d "$XDG_RUNTIME_DIR" && -S "$SOCKET_FILE" ]]; then
  pass_test "T1.D1" "Daemon initializes runtime directory and socket"
else
  fail_test "T1.D1" "Daemon failed to create runtime directory or socket"
fi

# T1.D4: Socket IPC status query (schema 1, connected mock, no legacy keys)
status_out=$(get_status)
if echo "$status_out" | jq -e '.schema_version == 1 and .connected == true and .device_name == "WH-1000XM3"
    and ([has("charging", "ambient_level", "eq_bands", "dsee", "last_updated")] | any | not)' >/dev/null 2>&1; then
  pass_test "T1.D4" "Socket IPC responds to 'status' with valid schema 1 JSON, no legacy keys"
else
  fail_test "T1.D4" "Socket IPC failed to respond to 'status'" "Got: $status_out"
fi

# T1.D5: Socket IPC noise mode modification
run_cli noise ambient >/dev/null 2>&1 || true
if jq -e '.noise_mode == "ambient"' <<<"$(get_status)" >/dev/null 2>&1; then
  pass_test "T1.D5" "Daemon updates noise_mode to 'ambient' via IPC"
else
  fail_test "T1.D5" "noise_mode was not updated to 'ambient'" "$(get_status)"
fi

# T1.D6: Socket IPC ambient-level modification
run_cli ambient-level 16 >/dev/null 2>&1 || true
if jq -e '.ambient_sound_level == 16' <<<"$(get_status)" >/dev/null 2>&1; then
  pass_test "T1.D6" "Daemon updates ambient_sound_level to 16 via IPC"
else
  fail_test "T1.D6" "ambient_sound_level was not updated to 16" "$(get_status)"
fi

# T1.D7: Socket IPC EQ preset modification
run_cli eq vocal >/dev/null 2>&1 || true
if jq -e '.eq_preset == "vocal"' <<<"$(get_status)" >/dev/null 2>&1; then
  pass_test "T1.D7" "Daemon updates eq_preset to 'vocal' via IPC"
else
  fail_test "T1.D7" "eq_preset was not updated to 'vocal'" "$(get_status)"
fi

# T1.D8: Socket IPC Custom EQ bands modification
run_cli eq custom 2 1 -1 3 0 4 >/dev/null 2>&1 || true
if jq -e '.eq_preset == "custom" and .eq_custom_bands == [2, 1, -1, 3, 0] and .clear_bass == 4' <<<"$(get_status)" >/dev/null 2>&1; then
  pass_test "T1.D8" "Daemon updates custom EQ bands and clear_bass via IPC"
else
  fail_test "T1.D8" "Custom EQ bands were not updated properly" "$(get_status)"
fi

# T1.D9: Socket IPC toggles (Focus on Voice, DSEE HX)
run_cli noise ambient 12 >/dev/null 2>&1 || true
run_cli voice-focus on >/dev/null 2>&1 || true
run_cli dsee off >/dev/null 2>&1 || true
if jq -e '.voice_passthrough == true and .dsee_hx == false' <<<"$(get_status)" >/dev/null 2>&1; then
  pass_test "T1.D9" "Daemon updates feature switches (Focus on Voice, DSEE HX)"
else
  fail_test "T1.D9" "Feature toggles were not updated properly" "$(get_status)"
fi

# T1.D9b: Enum-valued XM3 settings (Surround, Sound Position, Auto Power Off, Connection)
run_cli surround concert >/dev/null 2>&1 || true
run_cli sound-position rear-left >/dev/null 2>&1 || true
run_cli auto-power-off 30min >/dev/null 2>&1 || true
run_cli connection stable >/dev/null 2>&1 || true
if jq -e '.surround == "concert" and .sound_position == "rear-left" and .auto_power_off == "30min" and .connection_mode == "stable"' <<<"$(get_status)" >/dev/null 2>&1; then
  pass_test "T1.D9b" "Daemon updates VPT, auto power off and connection mode via IPC"
else
  fail_test "T1.D9b" "Enum settings were not updated properly" "$(get_status)"
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
if jq -e '.volume == 12 and .nc_button == "alexa" and .touch_panel == false and .voice_guidance == false' <<<"$(get_status)" >/dev/null 2>&1; then
  pass_test "T1.D9f" "Volume, NC button, touch panel and voice guidance update via IPC"
else
  fail_test "T1.D9f" "Device settings were not updated properly" "$(get_status)"
fi
run_cli nc-button ambient >/dev/null 2>&1 || true
run_cli touch-panel on >/dev/null 2>&1 || true
run_cli voice-guidance on >/dev/null 2>&1 || true

# T1.D9g: Custom EQ slots, the optimizer and playback are accepted; nonsense is not
run_cli eq user1 3 2 1 0 -1 4 >/dev/null 2>&1 || true
s_eq=$(get_status | jq -r '.eq_preset + ":" + (.clear_bass|tostring)')
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

# T1.D10: Simulated disconnect / reconnect signals (SIGUSR1 / SIGUSR2)
kill -USR1 "$DAEMON_PID"
sleep 0.2
s_down=$(get_status)
kill -USR2 "$DAEMON_PID"
sleep 0.2
s_up=$(get_status)
if jq -e '.schema_version == 1 and .connected == false' <<<"$s_down" >/dev/null 2>&1 &&
   jq -e '.connected == true' <<<"$s_up" >/dev/null 2>&1; then
  pass_test "T1.D10" "SIGUSR1 reports connected:false; SIGUSR2 restores connected:true"
else
  fail_test "T1.D10" "Signal disconnect/reconnect not reflected in status" "down=$s_down up=$s_up"
fi

# T1.D11: connect / disconnect verbs drive the (mock) link
set +e
run_cli disconnect >/dev/null 2>&1; c_down=$?
s_down=$(get_status)
run_cli connect >/dev/null 2>&1;    c_up=$?
s_up=$(get_status)
set -e
if [[ $c_down -eq 0 && $c_up -eq 0 ]] &&
   jq -e '.connected == false and .paired == true' <<<"$s_down" >/dev/null 2>&1 &&
   jq -e '.connected == true' <<<"$s_up" >/dev/null 2>&1; then
  pass_test "T1.D11" "disconnect reports connected:false (still paired); connect restores connected:true"
else
  fail_test "T1.D11" "connect/disconnect verbs misbehaved" "disconnect=$c_down connect=$c_up down=$s_down up=$s_up"
fi

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

# ==============================================================================
# TIER 2: BOUNDARY & CORNER CASES
# ==============================================================================
section_header "Tier 2: Boundary & Corner Cases"

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

# ==============================================================================
# TIER 3: CROSS-FEATURE COMBINATIONS & STATE TRANSITIONS
# ==============================================================================
section_header "Tier 3: Cross-Feature Combinations & State Transitions"

# T3.S1: Full Listening Mode Cycle (ANC -> Ambient -> Wind -> Off -> ANC)
run_cli noise anc >/dev/null 2>&1
s1=$(get_status | jq -r '.noise_mode')
run_cli noise ambient >/dev/null 2>&1
run_cli ambient-level 14 >/dev/null 2>&1
s2=$(get_status | jq -r '.noise_mode + ":" + (.ambient_sound_level|tostring)')
run_cli noise wind >/dev/null 2>&1
s3=$(get_status | jq -r '.noise_mode + ":" + (.ambient_sound_level|tostring)')
run_cli noise off >/dev/null 2>&1
s4=$(get_status | jq -r '.noise_mode')
run_cli noise anc >/dev/null 2>&1
s5=$(get_status | jq -r '.noise_mode')

if [[ "$s1" == "anc" && "$s2" == "ambient:14" && "$s3" == "wind:1" && "$s4" == "off" && "$s5" == "anc" ]]; then
  pass_test "T3.S1" "Complete Listening Mode Cycle (ANC -> Ambient 14 -> Wind 1 -> Off -> ANC)"
else
  fail_test "T3.S1" "Listening mode cycle mismatch: s1=$s1, s2=$s2, s3=$s3, s4=$s4, s5=$s5"
fi

# T3.S4: EQ Preset to Custom Mode Transition
run_cli eq bright >/dev/null 2>&1
eq1=$(get_status | jq -r '.eq_preset')
run_cli eq custom -2 0 3 1 -1 2 >/dev/null 2>&1
eq2=$(get_status | jq -r '.eq_preset + ":" + (.clear_bass|tostring)')
run_cli eq vocal >/dev/null 2>&1
eq3=$(get_status | jq -r '.eq_preset')

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
final_level=$(get_status | jq -r '.ambient_sound_level')
if [[ "$final_level" -eq 18 ]]; then
  pass_test "T3.S5" "Sequential command stream executes and resolves to final value (step 18)"
else
  fail_test "T3.S5" "Rapid sequential commands did not settle to 18 (got $final_level)"
fi

# ==============================================================================
# TIER 4: REAL-WORLD SCENARIOS & STRESS
# ==============================================================================
section_header "Tier 4: Real-World Scenarios & Stress"

# T4.R1: Daemon Cold Startup & Socket Security Verification
stop_daemon
rm -rf "$XDG_RUNTIME_DIR"
start_daemon
dir_perms=$(stat -c "%a" "$XDG_RUNTIME_DIR" 2>/dev/null || stat -f "%Lp" "$XDG_RUNTIME_DIR" 2>/dev/null)
sock_perms=$(stat -c "%a" "$SOCKET_FILE" 2>/dev/null || stat -f "%Lp" "$SOCKET_FILE" 2>/dev/null)

if [[ ("$dir_perms" == "700" || "$dir_perms" == "0700") && \
      ("$sock_perms" == "700" || "$sock_perms" == "0700") ]]; then
  pass_test "T4.R1" "Cold boot creates runtime dir and socket owner-only (0700/0700)"
else
  fail_test "T4.R1" "Security permissions mismatch (dir=$dir_perms, sock=$sock_perms)"
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

# T4.R5: Clean Graceful Shutdown (SIGTERM)
stop_daemon
if [[ ! -e "$SOCKET_FILE" ]]; then
  pass_test "T4.R5" "Graceful shutdown (SIGTERM) cleanly removes the socket"
else
  fail_test "T4.R5" "Shutdown left the socket behind: $SOCKET_FILE"
fi

# ==============================================================================
# SUMMARY REPORT
# ==============================================================================
section_header "Integration Test Suite Summary"

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
