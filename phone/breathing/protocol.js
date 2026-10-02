// Binary wire protocol shared with GlassSDK/examples/breathing/breathing.c.
// Keep both sides in sync if you change any layout or constant here.

export const SETTINGS_CHANNEL = 0x5245; // 'RE', phone -> glasses
export const EVENT_CHANNEL = 0x5246; // 'RF', glasses -> phone
export const PROTOCOL_VERSION = 1;

export const COMMAND_CONFIG = 1;
export const COMMAND_START = 2;
export const COMMAND_PAUSE = 3;
export const COMMAND_STOP = 4;

export const EVENT_SESSION_DONE = 1;

export const MIN_PHASE_SEC = 0;
export const MAX_PHASE_SEC = 60;
export const MIN_DURATION_MIN = 1;
export const MAX_DURATION_MIN = 30;

// Preset patterns. `preset` is echoed back on session completion.
export const PRESETS = {
  coherence: { preset: 0, inhale: 5, holdIn: 0, exhale: 5, holdOut: 0, durationMin: 5 },
  relax: { preset: 1, inhale: 4, holdIn: 0, exhale: 6, holdOut: 0, durationMin: 5 },
  box: { preset: 2, inhale: 4, holdIn: 4, exhale: 4, holdOut: 4, durationMin: 5 },
};

// [version, COMMAND_CONFIG, preset, inhale, holdIn, exhale, holdOut, duration_min]
export function encodeConfig(config) {
  return new Uint8Array([
    PROTOCOL_VERSION,
    COMMAND_CONFIG,
    config.preset & 0xff,
    config.inhale,
    config.holdIn,
    config.exhale,
    config.holdOut,
    config.durationMin,
  ]);
}

// [version, COMMAND_START, 0, 0, 0, 0, 0, 0] (padded to the 8-byte packet).
export function encodeStart() {
  return new Uint8Array([PROTOCOL_VERSION, COMMAND_START, 0, 0, 0, 0, 0, 0]);
}

export function encodePause() {
  return new Uint8Array([PROTOCOL_VERSION, COMMAND_PAUSE, 0, 0, 0, 0, 0, 0]);
}

export function encodeStop() {
  return new Uint8Array([PROTOCOL_VERSION, COMMAND_STOP, 0, 0, 0, 0, 0, 0]);
}

// Decodes a glasses -> phone event. Returns null for anything unrecognized.
export function decodeEvent(data) {
  if (!data || data.length < 7 || data[0] !== PROTOCOL_VERSION) return null;
  if (data[1] === EVENT_SESSION_DONE) {
    const cycles = data[2] | (data[3] << 8);
    const seconds = data[4] | (data[5] << 8);
    return { event: EVENT_SESSION_DONE, cycles, seconds, preset: data[6] };
  }
  return null;
}
