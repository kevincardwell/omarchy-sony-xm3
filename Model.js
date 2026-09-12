// plugin/Model.js
// Pure ECMAScript model library for Sony WH-1000XM3 headphone management.
// Zero QML dependencies; runnable in QML, Deno, and Node.js runtimes.

var SUPPORTED_SCHEMA = 1;
var LEVEL_UNKNOWN = -1;

// The XM3 exposes noise control as one continuous axis: step 0 is noise
// cancelling, step 1 is wind noise reduction, and 2..max are ambient sound.
// The panel presents those as named modes plus a slider over the ambient range.
var STEP_ANC = 0;
var STEP_WIND = 1;
var STEP_AMBIENT_MIN = 2;
var STEP_AMBIENT_MAX_DEFAULT = 19;
// Focus on Voice is only accepted at ambient step 2 and above.
var MIN_VOICE_FOCUS_STEP = 2;

var NOISE_ANC = "anc";
var NOISE_WIND = "wind";
var NOISE_AMBIENT = "ambient";
var NOISE_OFF = "off";
var NOISE_UNKNOWN = "unknown";

var EQ_OFF = "off";
var EQ_BRIGHT = "bright";
var EQ_EXCITED = "excited";
var EQ_MELLOW = "mellow";
var EQ_RELAXED = "relaxed";
var EQ_VOCAL = "vocal";
var EQ_TREBLE = "treble";
var EQ_BASS = "bass";
var EQ_SPEECH = "speech";
var EQ_CUSTOM = "custom";

var SURROUND_OFF = "off";
var SOUND_POSITION_OFF = "off";

var MAX_ERROR_CHARS = 140;
var ELIDED_ERROR_CHARS = 137;

var EQ_USER1 = "user1";
var EQ_USER2 = "user2";

var NOISE_MODES = [NOISE_ANC, NOISE_WIND, NOISE_AMBIENT, NOISE_OFF];
// The XM3's full preset list, in the order its EQ capability reports them.
var EQ_PRESETS = [
  EQ_OFF, EQ_BRIGHT, EQ_EXCITED, EQ_MELLOW, EQ_RELAXED, EQ_VOCAL,
  EQ_TREBLE, EQ_BASS, EQ_SPEECH, EQ_CUSTOM, EQ_USER1, EQ_USER2
];
// Presets whose five bands and Clear Bass the user sets.
var EQ_CUSTOM_SLOTS = [EQ_CUSTOM, EQ_USER1, EQ_USER2];
var EQ_BAND_LABELS = ["400", "1k", "2.5k", "6.3k", "16k"];
var NC_BUTTONS = ["ambient", "google-assistant", "alexa"];
var PLAYBACK_ACTIONS = ["previous", "play", "pause", "next"];
var OPTIMIZER_RUNNING_STATES = ["measuring-fit", "measuring-pressure", "optimizing"];
var SURROUND_PRESETS = ["off", "outdoor", "arena", "concert", "club"];
var SOUND_POSITIONS = ["off", "front-left", "front-right", "front", "rear-left", "rear-right"];
// Display order; the XM3's capability reports exactly these five.
var AUTO_POWER_OFF_VALUES = ["5min", "30min", "60min", "180min", "off"];
var CONNECTION_MODES = ["quality", "stable"];

function defaultStatus() {
  return {
    ok: false,
    lastError: "",
    schemaVersion: 0,
    schemaTooNew: false,
    connected: false,
    deviceName: "",
    batteryLevel: LEVEL_UNKNOWN,
    batteryCharging: false,
    noiseMode: NOISE_UNKNOWN,
    ambientSoundLevel: 0,
    ambientMaxLevel: STEP_AMBIENT_MAX_DEFAULT,
    voicePassthrough: false,
    eqPreset: EQ_OFF,
    eqCustomBands: [0, 0, 0, 0, 0],
    clearBass: 0,
    dseeHx: false,
    dseeHxActive: false,
    surround: SURROUND_OFF,
    soundPosition: SOUND_POSITION_OFF,
    autoPowerOff: "unknown",
    connectionMode: "unknown",
    codec: "",
    firmwareVersion: "",
    optimizerState: "idle",
    optimizerPressure: "",
    volume: LEVEL_UNKNOWN,
    volumeMax: 30,
    ncButton: "unknown",
    touchPanel: true,
    voiceGuidance: true,
    voiceGuidanceLanguage: ""
  };
}

function clamp(val, min, max, def) {
  var n = Number(val);
  if (isNaN(n)) return def;
  return Math.max(min, Math.min(max, Math.round(n)));
}

function oneOf(list, value, fallback) {
  var v = String(value || "").toLowerCase();
  return list.indexOf(v) !== -1 ? v : fallback;
}

function parseStatus(raw) {
  if (raw === null || raw === undefined) {
    var res = defaultStatus();
    res.lastError = "The sony daemon sent no status";
    return res;
  }
  var text = String(raw).trim();
  if (!text) {
    var res = defaultStatus();
    res.lastError = "The sony daemon sent no status";
    return res;
  }

  var parsed;
  try {
    parsed = JSON.parse(text);
  } catch (_e) {
    var res = defaultStatus();
    res.lastError = "Could not read the sony status";
    return res;
  }

  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
    var res = defaultStatus();
    res.lastError = "The sony status is invalid";
    return res;
  }

  if (parsed.schema_version === undefined || parsed.schema_version === null) {
    var res = defaultStatus();
    res.lastError = "The sony status carried no schema_version";
    return res;
  }

  var version = Number(parsed.schema_version);
  if (isNaN(version) || version > SUPPORTED_SCHEMA) {
    var res = defaultStatus();
    res.schemaTooNew = true;
    res.schemaVersion = isNaN(version) ? 0 : version;
    res.lastError = "sony daemon speaks status schema " + version + ", this panel reads " + SUPPORTED_SCHEMA;
    return res;
  }

  var res = defaultStatus();
  res.ok = true;
  res.schemaVersion = version;
  res.connected = parsed.connected === true;

  if (!res.connected) {
    return res;
  }

  res.deviceName = String(parsed.device_name || "");

  if (parsed.battery_level !== undefined && parsed.battery_level !== null) {
    var bl = Number(parsed.battery_level);
    if (!isNaN(bl) && bl >= 0) {
      res.batteryLevel = Math.min(100, Math.round(bl));
    } else {
      res.batteryLevel = LEVEL_UNKNOWN;
    }
  }

  res.batteryCharging = parsed.battery_charging === true || parsed.charging === true;

  res.noiseMode = oneOf([NOISE_ANC, NOISE_WIND, NOISE_AMBIENT, NOISE_OFF],
                        parsed.noise_mode, NOISE_UNKNOWN);

  res.ambientMaxLevel = clamp(parsed.ambient_max_level, STEP_AMBIENT_MIN, 30, STEP_AMBIENT_MAX_DEFAULT);
  var rawAmbient = parsed.ambient_sound_level !== undefined ? parsed.ambient_sound_level : parsed.ambient_level;
  res.ambientSoundLevel = clamp(rawAmbient, 0, res.ambientMaxLevel, 0);
  res.voicePassthrough = parsed.voice_passthrough === true;

  res.eqPreset = oneOf(EQ_PRESETS, parsed.eq_preset, EQ_OFF);

  var rawBands = Array.isArray(parsed.eq_custom_bands) ? parsed.eq_custom_bands : (Array.isArray(parsed.eq_bands) ? parsed.eq_bands : []);
  var bands = [];
  for (var i = 0; i < 5; i++) {
    bands.push(clamp(rawBands[i], -10, 10, 0));
  }
  res.eqCustomBands = bands;
  res.clearBass = clamp(parsed.clear_bass, -10, 10, 0);

  res.dseeHx = parsed.dsee_hx === true || parsed.dsee === true;
  res.dseeHxActive = parsed.dsee_hx_active === true;
  res.surround = oneOf(SURROUND_PRESETS, parsed.surround, SURROUND_OFF);
  res.soundPosition = oneOf(SOUND_POSITIONS, parsed.sound_position, SOUND_POSITION_OFF);
  res.autoPowerOff = oneOf(AUTO_POWER_OFF_VALUES, parsed.auto_power_off, "unknown");
  res.connectionMode = oneOf(CONNECTION_MODES, parsed.connection_mode, "unknown");
  res.codec = String(parsed.codec || "");
  res.firmwareVersion = String(parsed.firmware_version || "");

  res.optimizerState = oneOf(["idle", "done"].concat(OPTIMIZER_RUNNING_STATES), parsed.optimizer_state, "idle");
  res.optimizerPressure = String(parsed.optimizer_pressure || "");

  res.volumeMax = clamp(parsed.volume_max, 1, 100, 30);
  if (parsed.volume !== undefined && parsed.volume !== null && Number(parsed.volume) >= 0) {
    res.volume = clamp(parsed.volume, 0, res.volumeMax, 0);
  }

  res.ncButton = oneOf(NC_BUTTONS, parsed.nc_button, "unknown");
  res.touchPanel = parsed.touch_panel !== false;
  res.voiceGuidance = parsed.voice_guidance !== false;
  res.voiceGuidanceLanguage = String(parsed.voice_guidance_language || "");

  return res;
}

// ---------------------------------------------------------------------------
// Step <-> mode mapping (mirrors the daemon's protocol helpers)
// ---------------------------------------------------------------------------

// On "Priority on sound quality" the XM3 streams LDAC and cannot run its EQ
// or VPT (surround / sound position) processing at the same time.
function dspAvailable(connectionMode) {
  return connectionMode !== "quality";
}

function connectionModeName(mode) {
  switch (mode) {
    case "quality": return "Sound quality (LDAC)";
    case "stable": return "Stable connection";
    default: return "Unknown";
  }
}

function dseeDescription(enabled, active, connectionMode) {
  if (!enabled) return "Restores detail lost to compressed audio";
  if (active) return "Upscaling compressed audio";
  if (connectionMode === "quality") return "On, idle: LDAC has nothing to restore";
  return "On, idle while EQ or surround is active";
}

function isCustomEqSlot(preset) {
  return EQ_CUSTOM_SLOTS.indexOf(preset) !== -1;
}

function isOptimizerRunning(state) {
  return OPTIMIZER_RUNNING_STATES.indexOf(state) !== -1;
}

function optimizerDescription(state, pressure) {
  switch (state) {
    case "measuring-fit": return "Measuring how the headset fits…";
    case "measuring-pressure": return "Measuring atmospheric pressure…";
    case "optimizing": return "Optimizing noise cancelling…";
    case "done": return pressure ? "Optimized · " + pressure + " atm" : "Optimized";
    default:
      return pressure
        ? "Last run measured " + pressure + " atm. Wear the headset, then optimize."
        : "Tunes noise cancelling to your fit. Wear the headset first.";
  }
}

function ncButtonName(button) {
  switch (button) {
    case "ambient": return "Noise control";
    case "google-assistant": return "Google Assistant";
    case "alexa": return "Alexa";
    default: return "Unknown";
  }
}

function ncButtonDescription(button) {
  switch (button) {
    case "ambient": return "The NC/AMBIENT button switches noise control.";
    case "google-assistant": return "The NC/AMBIENT button talks to Google Assistant on your phone.";
    case "alexa": return "The NC/AMBIENT button talks to Alexa on your phone.";
    default: return "";
  }
}

function autoPowerOffLabel(value) {
  switch (value) {
    case "5min": return "5 min";
    case "30min": return "30 min";
    case "60min": return "1 hr";
    case "180min": return "3 hr";
    case "off": return "Never";
    default: return "—";
  }
}

function voiceGuidanceDescription(enabled, language) {
  if (!enabled) return "Spoken prompts are off";
  return language ? "Spoken prompts in " + language : "Spoken prompts on connect, battery and mode changes";
}

function volumeFraction(volume, max) {
  if (volume === undefined || volume === null || volume < 0 || !max) return 0.0;
  return Math.max(0.0, Math.min(1.0, volume / max));
}

function stepToNoiseMode(step) {
  if (step === STEP_ANC) return NOISE_ANC;
  if (step === STEP_WIND) return NOISE_WIND;
  return NOISE_AMBIENT;
}

function isVoiceFocusAvailable(mode, level) {
  return mode === NOISE_AMBIENT && level >= MIN_VOICE_FOCUS_STEP;
}

// ---------------------------------------------------------------------------
// Display names and icons
// ---------------------------------------------------------------------------

function noiseModeName(mode) {
  switch (mode) {
    case NOISE_ANC: return "Noise Cancelling";
    case NOISE_WIND: return "Wind Noise Reduction";
    case NOISE_AMBIENT: return "Ambient Sound";
    case NOISE_OFF: return "Off";
    default: return "Unknown";
  }
}

function noiseModeButtonLabel(mode) {
  switch (mode) {
    case NOISE_ANC: return "ANC";
    case NOISE_WIND: return "Wind";
    case NOISE_AMBIENT: return "Ambient";
    case NOISE_OFF: return "Off";
    default: return "?";
  }
}

// Nerd Font Material Design glyphs, checked by rendering them in the bar font.
// (The upstream codepoints were a zoom icon, a PDF file, a bag and a plus.)
function noiseModeIcon(mode) {
  switch (mode) {
    case NOISE_ANC: return "\u{F0A45}";      // md-ear-hearing-off: outside sound blocked
    case NOISE_WIND: return "\u{F059D}";     // md-weather-windy
    case NOISE_AMBIENT: return "\u{F07C5}";  // md-ear-hearing: outside sound let in
    case NOISE_OFF: return "\u{F015A}";      // md-close-circle-outline
    default: return "\u{F0A45}";
  }
}

function eqPresetName(preset) {
  switch (preset) {
    case EQ_OFF: return "Off";
    case EQ_BRIGHT: return "Bright";
    case EQ_EXCITED: return "Excited";
    case EQ_MELLOW: return "Mellow";
    case EQ_RELAXED: return "Relaxed";
    case EQ_VOCAL: return "Vocal";
    case EQ_TREBLE: return "Treble Boost";
    case EQ_BASS: return "Bass Boost";
    case EQ_SPEECH: return "Speech";
    case EQ_CUSTOM: return "Manual";
    case EQ_USER1: return "Custom 1";
    case EQ_USER2: return "Custom 2";
    default: return "Unknown";
  }
}

function eqPresetButtonLabel(preset) {
  switch (preset) {
    case EQ_TREBLE: return "Treble";
    case EQ_BASS: return "Bass";
    default: return eqPresetName(preset);
  }
}

function surroundName(preset) {
  switch (preset) {
    case "off": return "Off";
    case "outdoor": return "Outdoor Festival";
    case "arena": return "Arena";
    case "concert": return "Concert Hall";
    case "club": return "Club";
    default: return "Off";
  }
}

function surroundButtonLabel(preset) {
  switch (preset) {
    case "outdoor": return "Outdoor";
    case "concert": return "Concert";
    default: return surroundName(preset);
  }
}

function soundPositionName(position) {
  switch (position) {
    case "off": return "Off";
    case "front-left": return "Front L";
    case "front-right": return "Front R";
    case "front": return "Front";
    case "rear-left": return "Rear L";
    case "rear-right": return "Rear R";
    default: return "Off";
  }
}

function formatBattery(level) {
  if (level === undefined || level === null || level < 0) return "—";
  return Math.round(level) + "%";
}

function levelFraction(level) {
  if (level === undefined || level === null || level < 0) return 0.0;
  return Math.max(0.0, Math.min(1.0, level / 100.0));
}

function batteryIcon(level, charging) {
  if (charging) return "󰂄";
  if (level < 0) return "󰂃";
  if (level >= 95) return "󰁹";
  if (level >= 85) return "󰂂";
  if (level >= 75) return "󰂁";
  if (level >= 65) return "󰂀";
  if (level >= 55) return "󰁿";
  if (level >= 45) return "󰁾";
  if (level >= 35) return "󰁽";
  if (level >= 25) return "󰁼";
  if (level >= 15) return "󰁻";
  return "󰂎";
}

function elideError(text) {
  if (!text) return "";
  var cleaned = String(text).replace(/\s+/g, " ").trim();
  if (cleaned.length > MAX_ERROR_CHARS) {
    return cleaned.substring(0, ELIDED_ERROR_CHARS) + "…";
  }
  return cleaned;
}

// Middle-click / right-click cycle. Wind noise reduction is deliberately not in
// the cycle: it is a niche setting, and three stops is what the headset's own
// NC button does.
function cycleNoiseMode(currentMode) {
  if (currentMode === NOISE_ANC) return NOISE_AMBIENT;
  if (currentMode === NOISE_AMBIENT) return NOISE_OFF;
  return NOISE_ANC;
}
