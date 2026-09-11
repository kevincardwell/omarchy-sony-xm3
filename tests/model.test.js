// tests/model.test.js
// Standalone unit tests for Model.js, executable via Deno or Node.
//   deno run --allow-read tests/model.test.js
//   node tests/model.test.js
//
// Model.js is loaded from disk and evaluated. There is deliberately no bundled
// "reference implementation" fallback: a test that can pass without the file it
// is testing is not a test.

let readFileSync;
if (typeof Deno !== "undefined") {
  readFileSync = (p) => Deno.readTextFileSync(p);
} else if (typeof process !== "undefined") {
  const fs = await import("fs");
  readFileSync = (p) => fs.readFileSync(p, "utf-8");
} else {
  throw new Error("Unsupported runtime: expected Deno or Node.js");
}

const CANDIDATE_PATHS = ["Model.js", "../Model.js", "plugin/Model.js"];

let modelSource = null;
let modelPath = null;
for (const p of CANDIDATE_PATHS) {
  try {
    modelSource = readFileSync(p);
    modelPath = p;
    break;
  } catch (_e) {
    // Try next candidate.
  }
}

if (!modelSource) {
  console.error(`[FATAL] Model.js not found. Looked in: ${CANDIDATE_PATHS.join(", ")}`);
  console.error("        Run this from the repository root.");
  if (typeof Deno !== "undefined") Deno.exit(1);
  if (typeof process !== "undefined") process.exit(1);
}
console.log(`[INFO] Loaded Model.js from: ${modelPath}`);

const Model = new Function(
  modelSource +
  `; return {
    SUPPORTED_SCHEMA, LEVEL_UNKNOWN,
    STEP_ANC, STEP_WIND, STEP_AMBIENT_MIN, STEP_AMBIENT_MAX_DEFAULT, MIN_VOICE_FOCUS_STEP,
    NOISE_ANC, NOISE_WIND, NOISE_AMBIENT, NOISE_OFF, NOISE_UNKNOWN,
    NOISE_MODES, EQ_PRESETS, SURROUND_PRESETS, SOUND_POSITIONS,
    AUTO_POWER_OFF_VALUES, CONNECTION_MODES,
    EQ_OFF, EQ_BRIGHT, EQ_EXCITED, EQ_MELLOW, EQ_RELAXED,
    EQ_VOCAL, EQ_TREBLE, EQ_BASS, EQ_SPEECH, EQ_CUSTOM,
    SURROUND_OFF, SOUND_POSITION_OFF,
    defaultStatus, parseStatus, clamp,
    stepToNoiseMode, isVoiceFocusAvailable, cycleNoiseMode,
    dspAvailable, connectionModeName, dseeDescription,
    noiseModeName, noiseModeIcon, eqPresetName, eqPresetButtonLabel,
    surroundName, surroundButtonLabel, soundPositionName,
    batteryIcon, formatBattery, levelFraction, elideError
  };`
)();

// ---------------------------------------------------------------------------
// Tiny assertion harness
// ---------------------------------------------------------------------------
let passed = 0;
let failed = 0;

function check(label, actual, expected) {
  const a = JSON.stringify(actual);
  const e = JSON.stringify(expected);
  if (a === e) {
    passed++;
  } else {
    failed++;
    console.error(`  FAIL  ${label}\n        expected ${e}\n        actual   ${a}`);
  }
}

function suite(name, fn) {
  console.log(`\n${name}`);
  fn();
}

function fixture(name) {
  for (const dir of ["tests/fixtures/", "fixtures/", "../tests/fixtures/"]) {
    try {
      return readFileSync(dir + name);
    } catch (_e) {
      // Try next directory.
    }
  }
  throw new Error(`fixture not found: ${name}`);
}

// ---------------------------------------------------------------------------
suite("Suite 1: Empty & malformed input", () => {
  for (const [name, label] of [
    ["status_empty.json", "empty file"],
    ["status_whitespace.json", "whitespace-only file"],
  ]) {
    const r = Model.parseStatus(fixture(name));
    check(`${label} -> not ok`, r.ok, false);
    check(`${label} -> not connected`, r.connected, false);
    check(`${label} -> reports an error`, r.lastError.length > 0, true);
  }

  const corrupted = Model.parseStatus(fixture("status_corrupted.json"));
  check("truncated JSON -> not ok", corrupted.ok, false);
  check("truncated JSON -> reports an error", corrupted.lastError.length > 0, true);

  const nonObject = Model.parseStatus(fixture("status_non_object.json"));
  check("JSON array -> not ok", nonObject.ok, false);

  check("null input -> not ok", Model.parseStatus(null).ok, false);
  check("undefined input -> not ok", Model.parseStatus(undefined).ok, false);
});

// ---------------------------------------------------------------------------
suite("Suite 2: Schema version handling", () => {
  const missing = Model.parseStatus(fixture("status_missing_schema.json"));
  check("missing schema_version -> not ok", missing.ok, false);
  check("missing schema_version -> reports an error", missing.lastError.length > 0, true);

  const tooNew = Model.parseStatus(fixture("status_schema_too_new.json"));
  check("future schema -> not ok", tooNew.ok, false);
  check("future schema -> flagged as too new", tooNew.schemaTooNew, true);
  check("future schema -> version retained", tooNew.schemaVersion, 99);
});

// ---------------------------------------------------------------------------
suite("Suite 3: Full payload parsing", () => {
  const amb = Model.parseStatus(fixture("status_connected_ambient.json"));
  check("ambient -> ok", amb.ok, true);
  check("ambient -> connected", amb.connected, true);
  check("ambient -> device name", amb.deviceName, "WH-1000XM3");
  check("ambient -> battery", amb.batteryLevel, 80);
  check("ambient -> mode", amb.noiseMode, "ambient");
  check("ambient -> level", amb.ambientSoundLevel, 14);
  check("ambient -> max level", amb.ambientMaxLevel, 19);
  check("ambient -> focus on voice", amb.voicePassthrough, true);
  check("ambient -> codec", amb.codec, "SBC");
  check("ambient -> dsee processing", amb.dseeHxActive, true);

  const anc = Model.parseStatus(fixture("status_connected_anc.json"));
  check("anc -> mode", anc.noiseMode, "anc");
  check("anc -> level is step 0", anc.ambientSoundLevel, 0);
  check("anc -> eq preset", anc.eqPreset, "bright");
  check("anc -> dsee hx", anc.dseeHx, true);
  check("anc -> dsee idle on LDAC", anc.dseeHxActive, false);

  const wind = Model.parseStatus(fixture("status_connected_wind.json"));
  check("wind -> mode", wind.noiseMode, "wind");
  check("wind -> level is step 1", wind.ambientSoundLevel, 1);
  check("wind -> codec", wind.codec, "aptX HD");

  const off = Model.parseStatus(fixture("status_connected_off.json"));
  check("off -> mode", off.noiseMode, "off");
  check("off -> surround", off.surround, "club");
  check("off -> sound position", off.soundPosition, "rear-left");

  const chg = Model.parseStatus(fixture("status_connected_charging.json"));
  check("charging -> flag", chg.batteryCharging, true);
  check("charging -> auto power off", chg.autoPowerOff, "off");

  const low = Model.parseStatus(fixture("status_connected_low_battery.json"));
  check("low battery -> level", low.batteryLevel, 8);
  check("low battery -> connection mode", low.connectionMode, "stable");

  const eq = Model.parseStatus(fixture("status_connected_custom_eq.json"));
  check("custom eq -> preset", eq.eqPreset, "custom");
  check("custom eq -> bands", eq.eqCustomBands, [-4, 2, 0, 3, -1]);
  check("custom eq -> clear bass", eq.clearBass, 5);
});

// ---------------------------------------------------------------------------
suite("Suite 4: Disconnected payload", () => {
  const r = Model.parseStatus(fixture("status_disconnected.json"));
  check("disconnected -> ok (well-formed)", r.ok, true);
  check("disconnected -> not connected", r.connected, false);
  check("disconnected -> battery unknown", r.batteryLevel, Model.LEVEL_UNKNOWN);
  check("disconnected -> no device name", r.deviceName, "");
});

// ---------------------------------------------------------------------------
suite("Suite 5: Clamping & unknown vocabulary", () => {
  const r = Model.parseStatus(fixture("status_extreme_values.json"));
  check("battery 250 -> clamped to 100", r.batteryLevel, 100);
  check("ambient 99 -> clamped to max", r.ambientSoundLevel, 19);
  check("bands -99/99 -> clamped", r.eqCustomBands, [-10, 10, 0, 0, 0]);
  check("clear bass -99 -> clamped", r.clearBass, -10);
  check("unknown noise mode -> unknown", r.noiseMode, Model.NOISE_UNKNOWN);
  check("unknown eq preset -> off", r.eqPreset, Model.EQ_OFF);
  check("unknown surround -> off", r.surround, Model.SURROUND_OFF);
  check("unknown sound position -> off", r.soundPosition, Model.SOUND_POSITION_OFF);
  check("unknown auto power off -> unknown", r.autoPowerOff, "unknown");
  check("unknown connection mode -> unknown", r.connectionMode, "unknown");

  check("clamp non-numeric -> default", Model.clamp("abc", 0, 10, 7), 7);
  check("clamp rounds", Model.clamp(3.7, 0, 10, 0), 4);
});

// ---------------------------------------------------------------------------
suite("Suite 6: Step <-> mode mapping", () => {
  check("step 0 -> anc", Model.stepToNoiseMode(0), Model.NOISE_ANC);
  check("step 1 -> wind", Model.stepToNoiseMode(1), Model.NOISE_WIND);
  check("step 2 -> ambient", Model.stepToNoiseMode(2), Model.NOISE_AMBIENT);
  check("step 19 -> ambient", Model.stepToNoiseMode(19), Model.NOISE_AMBIENT);

  check("voice focus in ambient at step 2", Model.isVoiceFocusAvailable(Model.NOISE_AMBIENT, 2), true);
  check("voice focus in ambient at step 12", Model.isVoiceFocusAvailable(Model.NOISE_AMBIENT, 12), true);
  check("no voice focus below step 2", Model.isVoiceFocusAvailable(Model.NOISE_AMBIENT, 1), false);
  check("no voice focus under anc", Model.isVoiceFocusAvailable(Model.NOISE_ANC, 0), false);
  check("no voice focus when off", Model.isVoiceFocusAvailable(Model.NOISE_OFF, 12), false);

  check("cycle anc -> ambient", Model.cycleNoiseMode(Model.NOISE_ANC), Model.NOISE_AMBIENT);
  check("cycle ambient -> off", Model.cycleNoiseMode(Model.NOISE_AMBIENT), Model.NOISE_OFF);
  check("cycle off -> anc", Model.cycleNoiseMode(Model.NOISE_OFF), Model.NOISE_ANC);
  check("cycle unknown -> anc", Model.cycleNoiseMode(Model.NOISE_UNKNOWN), Model.NOISE_ANC);
});

// ---------------------------------------------------------------------------
suite("Suite 7: Vocabulary shared with the daemon", () => {
  // These lists are the plugin's half of a contract with sony-xm3-ctl. If one
  // drifts, the panel starts issuing commands the daemon rejects.
  check("noise modes", Model.NOISE_MODES, ["anc", "wind", "ambient", "off"]);
  check("eq presets", Model.EQ_PRESETS,
        ["off", "bright", "excited", "mellow", "relaxed", "vocal", "treble", "bass", "speech", "custom"]);
  check("surround presets", Model.SURROUND_PRESETS, ["off", "outdoor", "arena", "concert", "club"]);
  check("sound positions", Model.SOUND_POSITIONS,
        ["off", "front-left", "front-right", "front", "rear-left", "rear-right"]);
  check("auto power off values", Model.AUTO_POWER_OFF_VALUES,
        ["off", "5min", "30min", "60min", "180min"]);
  check("connection modes", Model.CONNECTION_MODES, ["quality", "stable"]);
  check("ambient steps start at 2", Model.STEP_AMBIENT_MIN, 2);
  check("default max ambient step", Model.STEP_AMBIENT_MAX_DEFAULT, 19);
});

// ---------------------------------------------------------------------------
suite("Suite 8: Display formatters", () => {
  check("noiseModeName(anc)", Model.noiseModeName("anc"), "Noise Cancelling");
  check("noiseModeName(wind)", Model.noiseModeName("wind"), "Wind Noise Reduction");
  check("noiseModeName(ambient)", Model.noiseModeName("ambient"), "Ambient Sound");
  check("noiseModeName(off)", Model.noiseModeName("off"), "Off");

  check("eqPresetName(custom) is the Sony label", Model.eqPresetName("custom"), "Manual");
  check("eqPresetName(treble)", Model.eqPresetName("treble"), "Treble Boost");
  check("eqPresetButtonLabel(treble) is short", Model.eqPresetButtonLabel("treble"), "Treble");
  check("eqPresetButtonLabel(vocal)", Model.eqPresetButtonLabel("vocal"), "Vocal");

  check("surroundName(concert)", Model.surroundName("concert"), "Concert Hall");
  check("surroundButtonLabel(concert) is short", Model.surroundButtonLabel("concert"), "Concert");
  check("soundPositionName(rear-left)", Model.soundPositionName("rear-left"), "Rear L");

  check("formatBattery(78)", Model.formatBattery(78), "78%");
  check("formatBattery(0)", Model.formatBattery(0), "0%");
  check("formatBattery(-1)", Model.formatBattery(-1), "—");

  check("levelFraction(78)", Model.levelFraction(78), 0.78);
  check("levelFraction(100)", Model.levelFraction(100), 1.0);
  check("levelFraction(-1)", Model.levelFraction(-1), 0.0);

  check("batteryIcon charging is a string", typeof Model.batteryIcon(50, true), "string");
  check("batteryIcon normal is a string", typeof Model.batteryIcon(50, false), "string");
  check("batteryIcon differs when charging",
        Model.batteryIcon(50, true) !== Model.batteryIcon(50, false), true);

  const longErr = "Error: " + "a".repeat(200);
  const elided = Model.elideError(longErr);
  check("elideError length <= 140", elided.length <= 140, true);
  check("elideError ends with ellipsis", elided.endsWith("…"), true);
  check("elideError('') is empty", Model.elideError(""), "");
  check("elideError collapses whitespace", Model.elideError("a  \n b"), "a b");
});


// ---------------------------------------------------------------------------
suite("Suite 9: LDAC versus the headset's own processing (observed on hardware)", () => {
  // On "Priority on sound quality" the XM3 refuses EQ and VPT commands.
  check("EQ/surround unavailable on LDAC", Model.dspAvailable("quality"), false);
  check("EQ/surround available on stable", Model.dspAvailable("stable"), true);
  check("unknown mode does not lock the controls", Model.dspAvailable("unknown"), true);

  check("connection name: quality", Model.connectionModeName("quality"), "Sound quality (LDAC)");
  check("connection name: stable", Model.connectionModeName("stable"), "Stable connection");

  // DSEE HX: the setting and whether it is processing are different things.
  check("dsee off", Model.dseeDescription(false, false, "quality"), "Restores detail lost to compressed audio");
  check("dsee on and working", Model.dseeDescription(true, true, "stable"), "Upscaling compressed audio");
  check("dsee on but idle on LDAC", Model.dseeDescription(true, false, "quality"), "On, idle: LDAC has nothing to restore");
  check("dsee on but idle behind EQ", Model.dseeDescription(true, false, "stable"), "On, idle while EQ or surround is active");

  const legacy = Model.parseStatus(JSON.stringify({ schema_version: 1, connected: true, ear_detection: false }));
  check("a stale ear_detection field is ignored, not an error", legacy.ok, true);
});
// ---------------------------------------------------------------------------
console.log(`\nSummary: ${passed} passed, ${failed} failed`);
if (failed > 0) {
  if (typeof Deno !== "undefined") Deno.exit(1);
  if (typeof process !== "undefined") process.exit(1);
}
