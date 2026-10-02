import { createGMPlugin } from './vendor/gm-plugin-web-sdk.esm.js';
import {
  SETTINGS_CHANNEL,
  EVENT_CHANNEL,
  EVENT_SESSION_DONE,
  PRESETS,
  MIN_DURATION_MIN,
  MAX_DURATION_MIN,
  decodeEvent,
  encodeConfig,
  encodeStart,
  encodePause,
  encodeStop,
} from './protocol.js';
import { detectLocale, getStrings } from './translations.js';

const S = getStrings(detectLocale());

const STORAGE_KEY = 'breathing';

const gm = createGMPlugin();

const statusEl = document.querySelector('#status');
const presetButtons = Array.from(document.querySelectorAll('[data-preset]'));
const durationInput = document.querySelector('#duration');
const durationValue = document.querySelector('#duration-value');
const patternSummary = document.querySelector('#pattern-summary');
const startButton = document.querySelector('#start-button');
const stopButton = document.querySelector('#stop-button');
const todayCountEl = document.querySelector('#today-count');
const weekCountEl = document.querySelector('#week-count');
const weekMinutesEl = document.querySelector('#week-minutes');
const lastSessionEl = document.querySelector('#last-session');

let config = { preset: 0, inhale: 5, holdIn: 0, exhale: 5, holdOut: 0, durationMin: 5 };
let history = {}; // { 'YYYY-MM-DD': { sessions, seconds } }
let connected = false;
let running = false;
let paused = false;

function applyStrings() {
  document.documentElement.lang = detectLocale();
  document.querySelector('#app-title').textContent = S.appTitle;
  document.querySelector('#section-rhythm').textContent = S.sectionRhythm;
  document.querySelector('#preset-coherence').textContent = S.presetCoherence;
  document.querySelector('#preset-relax').textContent = S.presetRelax;
  document.querySelector('#preset-box').textContent = S.presetBox;
  document.querySelector('#duration-label').textContent = S.durationLabel;
  document.querySelector('#unit-min').textContent = S.unitMin;
  document.querySelector('#section-sessions').textContent = S.sectionSessions;
  document.querySelector('#stat-today').textContent = S.statToday;
  document.querySelector('#stat-week').textContent = S.statWeek;
  document.querySelector('#stat-week-min').textContent = S.statWeekMin;
  document.querySelector('#last-session').textContent = S.noSession;
  stopButton.textContent = S.stop;
  setStatus(S.statusStarting);
}

function setStatus(text, state = '') {
  statusEl.textContent = text;
  statusEl.className = `status ${state}`.trim();
}

function todayKey() {
  const now = new Date();
  const month = String(now.getMonth() + 1).padStart(2, '0');
  const day = String(now.getDate()).padStart(2, '0');
  return `${now.getFullYear()}-${month}-${day}`;
}

function formatTime(iso) {
  const date = new Date(iso);
  return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

async function loadState() {
  const result = await gm.storage.get(STORAGE_KEY);
  if (result.value) {
    if (result.value.config) config = { ...config, ...result.value.config };
    if (result.value.history) history = result.value.history;
  }
}

async function persistState() {
  await gm.storage.set(STORAGE_KEY, { config, history });
}

function patternLabel() {
  const parts = [`${config.inhale}s`];
  if (config.holdIn > 0) parts.push(`+${config.holdIn}s`);
  parts.push(`-${config.exhale}s`);
  if (config.holdOut > 0) parts.push(`+${config.holdOut}s`);
  return parts.join(' ');
}

function renderPattern() {
  patternSummary.textContent = S.patternSummary(patternLabel(), config.durationMin);
  durationInput.value = String(config.durationMin);
  durationValue.textContent = String(config.durationMin);
  presetButtons.forEach((button) => {
    button.classList.toggle('active', Number(button.dataset.preset) === config.preset);
  });
}

function renderStats() {
  const today = history[todayKey()] || { sessions: 0, seconds: 0 };
  let weekSessions = 0;
  let weekSeconds = 0;
  const now = new Date();
  for (let i = 0; i < 7; i += 1) {
    const d = new Date(now);
    d.setDate(now.getDate() - i);
    const month = String(d.getMonth() + 1).padStart(2, '0');
    const day = String(d.getDate()).padStart(2, '0');
    const entry = history[`${d.getFullYear()}-${month}-${day}`];
    if (entry) {
      weekSessions += entry.sessions;
      weekSeconds += entry.seconds;
    }
  }
  todayCountEl.textContent = String(today.sessions);
  weekCountEl.textContent = String(weekSessions);
  weekMinutesEl.textContent = String(Math.round(weekSeconds / 60));
}

function renderControls() {
  const busy = running;
  durationInput.disabled = !connected || busy;
  presetButtons.forEach((button) => {
    button.disabled = !connected || busy;
  });
  startButton.disabled = !connected;
  stopButton.disabled = !connected || !running;
  startButton.textContent = paused ? S.resume : running ? S.pause : S.start;
  renderPattern();
}

async function sendConfig() {
  if (!connected) return;
  try {
    await gm.plugin.sendMessage(SETTINGS_CHANNEL, encodeConfig(config));
  } catch (error) {
    setStatus(error.message || String(error), 'error');
  }
}

async function onConfigChanged() {
  await persistState();
  renderControls();
  await sendConfig();
}

async function sendCommand(encoder) {
  if (!connected) return;
  try {
    await gm.plugin.sendMessage(SETTINGS_CHANNEL, encoder());
  } catch (error) {
    setStatus(error.message || String(error), 'error');
  }
}

presetButtons.forEach((button) => {
  button.addEventListener('click', () => {
    const preset = PRESETS[button.dataset.preset];
    if (!preset) return;
    config = { ...config, ...preset };
    void onConfigChanged();
  });
});

durationInput.addEventListener('input', () => {
  const value = Math.min(MAX_DURATION_MIN, Math.max(MIN_DURATION_MIN, Number(durationInput.value)));
  config.durationMin = value;
  durationValue.textContent = String(value);
  void onConfigChanged();
});

startButton.addEventListener('click', () => {
  if (!running) {
    running = true;
    paused = false;
    renderControls();
    void sendCommand(encodeStart);
  } else {
    paused = !paused;
    renderControls();
    void sendCommand(encodePause);
  }
});

stopButton.addEventListener('click', () => {
  running = false;
  paused = false;
  renderControls();
  void sendCommand(encodeStop);
});

const offMessages = gm.plugin.onMessage((message) => {
  if (message.channel !== EVENT_CHANNEL) return;
  const decoded = decodeEvent(message.data);
  if (!decoded) return;
  if (decoded.event === EVENT_SESSION_DONE) {
    running = false;
    paused = false;
    const key = todayKey();
    const entry = history[key] || { sessions: 0, seconds: 0 };
    entry.sessions += 1;
    entry.seconds += decoded.seconds;
    history[key] = entry;
    void persistState();
    renderStats();
    renderControls();
    const minutes = Math.round(decoded.seconds / 60);
    lastSessionEl.textContent =
      S.lastSession(formatTime(new Date().toISOString()), minutes, decoded.cycles);
  }
});

async function start() {
  try {
    await gm.ready();
    await loadState();
    applyStrings();
    renderPattern();
    renderStats();
    renderControls();
    const info = await gm.device.getInfo();
    connected = Boolean(info.connected);
    setStatus(connected ? S.statusConnected : S.statusDisconnected, connected ? 'ready' : 'error');
    renderControls();
    // Push the saved pattern so the glasses resume the chosen behavior.
    await sendConfig();
    await gm.device.subscribeEvents(['connection']);
    gm.device.onConnection((event) => {
      connected = Boolean(event.connected);
      setStatus(connected ? S.statusConnected : S.statusDisconnected, connected ? 'ready' : 'error');
      renderControls();
      if (connected) void sendConfig();
    });
  } catch (error) {
    setStatus(error.message || String(error), 'error');
  }
}

window.addEventListener('pagehide', () => {
  offMessages();
  gm.close();
});

void start();
