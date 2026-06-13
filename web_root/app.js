const $ = (sel) => document.querySelector(sel);
const $$ = (sel) => Array.from(document.querySelectorAll(sel));

const DEVICE_STORAGE_KEY = 'gb28181-agent-device-config';
let logFilter = 'ALL';
let currentLang = window.GB_I18N ? GB_I18N.getLanguage() : 'zh';
let state = {status: null, platforms: [], channelMeta: {max_clients: 2, channels_per_client: 8, max_channels: 16}, av: null, device: null, mediaFiles: [], logs: [], alarms: [], oem: null, settings: null, preview: null};
let previewTimer = null;
let previewSeq = 0;
let previewFrameTimes = [];
let previewSourceKey = '';
let previewObjectUrl = '';
let previewController = null;
let previewInFlight = false;
let recognizeTargetForm = null;
let selectedAvChannelId = 1;
let selectedGbClientId = 1;
let avDirty = false;
const DEFAULT_PREVIEW_FPS = 5;

const DEFAULT_OEM = {
  brand: {
    manufacturerName: 'Open GB',
    productName: 'GB28181 Device Console',
    shortName: 'OG',
    subtitle: 'Professional National Standard Video Gateway',
    loginSubtitle: 'GB/T 28181 device management',
    logoText: 'GB',
    logoUrl: '',
    faviconUrl: '',
    copyright: 'Open GB Project'
  },
  theme: {
    background: '#f3f6f8',
    surface: '#ffffff',
    surfaceMuted: '#f8fafc',
    text: '#17212f',
    muted: '#667085',
    line: '#d8e0e7',
    primary: '#1f6feb',
    secondary: '#38bdf8',
    navigation: '#111c2b',
    success: '#117a46',
    warning: '#a45b00',
    danger: '#b42318'
  },
  modules: {dashboard: true, platforms: true, av: true, devices: true, logs: true, system: true},
  defaults: {language: 'zh'}
};

function defaultDeviceConfig() {
  return {
    source_mode: 'device',
    video_device: '',
    audio_device: '',
    media_file: 'data/Big_Buck_Bunny_720_10s_2MB.mp4',
    resolution: '',
    bitrate_kbps: 0,
    file_pacing: 'realtime',
    file_loop: true
  };
}

function deviceCaps() {
  return (state.device && state.device.capabilities) || {source_types: ['device', 'file'], video_devices: [], audio_devices: [], screen_sources: []};
}

function videoEncoderCaps() {
  return (state.av && state.av.capabilities && Array.isArray(state.av.capabilities.video_encoders))
    ? state.av.capabilities.video_encoders
    : [];
}

function hasHardwareVideoEncoder() {
  return videoEncoderCaps().some((encoder) => !!encoder.hardware);
}

function encoderModeOptions(video) {
  const current = video && video.prefer_hardware ? 'hardware' : 'software';
  const hardware = hasHardwareVideoEncoder();
  const options = [`<option value="software">${t('softwareEncoder')}</option>`];
  if (hardware) options.push(`<option value="hardware">${t('hardwareEncoder')}</option>`);
  if (current === 'hardware' && !hardware) options.push(`<option value="hardware" disabled>${t('hardwareEncoder')} | ${t('unavailable')}</option>`);
  return options.join('');
}

function appendMissingOption(select, value, label) {
  if (!select || !value || [...select.options].some((o) => o.value === value)) return;
  select.insertAdjacentHTML('afterbegin', `<option value="${escapeHtml(value)}">${escapeHtml(label)}</option>`);
}

function customBoundSources() {
  return (state.platforms || []).filter((p) => p.source_profile === 'custom');
}

function deviceInfoById(type, id) {
  if (!id) return null;
  const caps = deviceCaps();
  const list = type === 'audio' ? caps.audio_devices : (type === 'screen' ? caps.screen_sources : caps.video_devices);
  return (list || []).find((d) => d.id === id) || null;
}

function deviceDisplayName(type, id) {
  const info = deviceInfoById(type, id);
  return (info && info.name) || id || '';
}

function renderDeviceOptions() {
  const form = $('#deviceForm');
  if (!form) return;
  const caps = deviceCaps();
  const sourceTypes = Array.isArray(caps.source_types) && caps.source_types.length
    ? caps.source_types
    : ['device', 'file', ...((caps.screen_sources || []).length ? ['screen'] : [])];
  form.querySelectorAll('[data-source-card]').forEach((card) => {
    const supported = sourceTypes.includes(card.dataset.sourceCard);
    card.hidden = !supported;
    const input = card.querySelector('input[name="source_mode"]');
    if (input) input.disabled = !supported;
  });
  if (state.device && !sourceTypes.includes(state.device.source_mode)) state.device.source_mode = sourceTypes[0] || 'device';
  const videoSelect = form.querySelector('[name="video_device"]');
  const audioSelect = form.querySelector('[name="audio_device"]');
  const screenSelect = form.querySelector('[name="screen_source"]');
  if (!videoSelect || !audioSelect || !screenSelect) return;
  const optionHtml = (items) => items.map((d) => {
    const label = [d.name || d.id, d.backend].filter(Boolean).join(' | ');
    return `<option value="${escapeHtml(d.id)}">${escapeHtml(label)}</option>`;
  }).join('');
  videoSelect.innerHTML = optionHtml(caps.video_devices || []);
  audioSelect.innerHTML = optionHtml(caps.audio_devices || []);
  screenSelect.innerHTML = optionHtml(caps.screen_sources || []);
  videoSelect.disabled = !videoSelect.options.length;
  audioSelect.disabled = !audioSelect.options.length;
  screenSelect.disabled = !screenSelect.options.length;
  const isDeviceMode = state.device?.source_mode !== 'screen' && state.device?.source_mode !== 'file';
  if (isDeviceMode && state.device && state.device.video_device && [...videoSelect.options].some((o) => o.value === state.device.video_device)) {
    videoSelect.value = state.device.video_device;
  } else if (isDeviceMode && state.device && state.device.video_device) {
    appendMissingOption(videoSelect, state.device.video_device, `${state.device.video_device} | ${t('unavailable')}`);
    videoSelect.value = state.device.video_device;
    videoSelect.disabled = false;
  } else if (videoSelect.options.length) {
    videoSelect.value = videoSelect.options[0].value;
  }
  if (state.device && state.device.source_mode === 'screen' && state.device.video_device && [...screenSelect.options].some((o) => o.value === state.device.video_device)) {
    screenSelect.value = state.device.video_device;
  } else if (state.device && state.device.source_mode === 'screen' && state.device.video_device) {
    appendMissingOption(screenSelect, state.device.video_device, `${state.device.video_device} | ${t('unavailable')}`);
    screenSelect.value = state.device.video_device;
    screenSelect.disabled = false;
  } else if (screenSelect.options.length) {
    screenSelect.value = screenSelect.options[0].value;
  }
  if (state.device && state.device.audio_device && [...audioSelect.options].some((o) => o.value === state.device.audio_device)) {
    audioSelect.value = state.device.audio_device;
  } else if (state.device && state.device.audio_device) {
    appendMissingOption(audioSelect, state.device.audio_device, `${state.device.audio_device} | ${t('unavailable')}`);
    audioSelect.value = state.device.audio_device;
    audioSelect.disabled = false;
  } else if (audioSelect.options.length) {
    audioSelect.value = audioSelect.options[0].value;
  }
  customBoundSources().forEach((p) => {
    if (p.source_mode !== 'device') return;
    appendMissingOption(videoSelect, p.video_device, `${p.video_device} | ${t('unavailable')}`);
    appendMissingOption(audioSelect, p.audio_device, `${p.audio_device} | ${t('unavailable')}`);
  });
  customBoundSources().forEach((p) => {
    if (p.source_mode === 'screen') appendMissingOption(screenSelect, p.video_device, `${p.video_device} | ${t('unavailable')}`);
  });
  videoSelect.disabled = !videoSelect.options.length;
  audioSelect.disabled = !audioSelect.options.length;
  screenSelect.disabled = !screenSelect.options.length;
}

function selectOptions(items, current = '') {
  const options = (items || []).map((d) => {
    const label = [d.name || d.id, d.backend].filter(Boolean).join(' | ');
    return `<option value="${escapeHtml(d.id)}">${escapeHtml(label)}</option>`;
  }).join('');
  return current && !(items || []).some((d) => d.id === current)
    ? `<option value="${escapeHtml(current)}">${escapeHtml(current)} | ${t('unavailable')}</option>${options}`
    : options;
}

function mediaFileOptions(current = '') {
  const options = state.mediaFiles.map((file) => {
    const path = mediaFilePath(file);
    const res = mediaFileResolution(file);
    const bitrate = mediaFileBitrate(file);
    const meta = [res, bitrate ? `${bitrate} Kbps` : ''].filter(Boolean).join(' | ');
    return `<option value="${escapeHtml(path)}">${escapeHtml(meta ? `${path} (${meta})` : path)}</option>`;
  }).join('');
  const extra = [];
  if (current && !state.mediaFiles.some((file) => mediaFilePath(file) === current)) extra.push(current);
  customBoundSources().forEach((p) => {
    if (p.source_mode === 'file' && p.media_file && !state.mediaFiles.some((file) => mediaFilePath(file) === p.media_file) && !extra.includes(p.media_file)) {
      extra.push(p.media_file);
    }
  });
  return `${extra.map((path) => `<option value="${escapeHtml(path)}">${escapeHtml(path)} | ${t('unavailable')}</option>`).join('')}${options}`;
}

function normalizeDeviceConfig(data = {}) {
  const sourceMode = data.source_mode || data.mode || data.input_mode;
  const mediaFile = data.media_file || data.file || data.path;
  const bitrate = data.bitrate_kbps ?? data.bitrate ?? data.video_bitrate_kbps;
  const mode = sourceMode === 'file' || sourceMode === 'screen' ? sourceMode : 'device';
  return {
    ...defaultDeviceConfig(),
    ...data,
    source_mode: mode,
    media_file: mediaFile ?? defaultDeviceConfig().media_file,
    bitrate_kbps: Number(bitrate || 0),
    resolution: data.resolution || ''
  };
}

function loadDeviceConfig() {
  try { return normalizeDeviceConfig(JSON.parse(localStorage.getItem(DEVICE_STORAGE_KEY) || '{}')); }
  catch (_) { return defaultDeviceConfig(); }
}

function selectedMediaFile(path) {
  return state.mediaFiles.find((file) => mediaFilePath(file) === path) || null;
}

function mediaFilePath(file) {
  return typeof file === 'string' ? file : String(file.path || file.file || file.name || file.url || '');
}

function mediaFileResolution(file) {
  if (!file || typeof file === 'string') return '';
  if (file.resolution) return String(file.resolution);
  return file.width && file.height ? `${file.width}x${file.height}` : '';
}

function mediaFileBitrate(file) {
  if (!file || typeof file === 'string') return 0;
  return Number(file.bitrate_kbps ?? file.bitrate ?? file.video_bitrate_kbps ?? 0);
}

function mediaFilesFromPayload(payload) {
  payload = payload || {};
  const files = Array.isArray(payload) ? payload : (payload.files || payload.items || payload.media_files || []);
  return files.filter((file) => mediaFilePath(file));
}

function cssName(name) {
  if (globalThis.CSS && typeof CSS.escape === 'function') return CSS.escape(name);
  return String(name).replace(/\\/g, '\\\\').replace(/"/g, '\\"');
}

function fields(form, name) {
  return form ? Array.from(form.querySelectorAll(`[name="${cssName(name)}"]`)) : [];
}

function field(form, name) {
  return fields(form, name)[0] || null;
}

function buildDevicePayload(data) {
  const payload = normalizeDeviceConfig(data);
  if (payload.source_mode === 'device') {
    const deviceForm = $('#deviceForm');
    payload.video_device = payload.video_device || field(deviceForm, 'video_device')?.value || '';
    payload.audio_device = payload.audio_device || field(deviceForm, 'audio_device')?.value || '';
    payload.media_file = '';
    payload.resolution = '';
    payload.bitrate_kbps = 0;
  } else if (payload.source_mode === 'screen') {
    const deviceForm = $('#deviceForm');
    const screenId = payload.screen_source || field(deviceForm, 'screen_source')?.value || payload.video_device || '';
    const screen = deviceInfoById('screen', screenId);
    payload.video_device = screenId;
    payload.audio_device = '';
    payload.media_file = '';
    payload.resolution = screen && screen.width && screen.height ? `${screen.width}x${screen.height}` : '';
    payload.bitrate_kbps = 0;
  } else {
    const file = selectedMediaFile(payload.media_file);
    payload.video_device = '';
    payload.audio_device = '';
    payload.resolution = payload.resolution || mediaFileResolution(file);
    payload.bitrate_kbps = Number(payload.bitrate_kbps || mediaFileBitrate(file));
  }
  delete payload.screen_source;
  return payload;
}

async function saveDeviceConfig(data) {
  const payload = buildDevicePayload(data);
  const saved = await api('/api/devices', {method: 'PUT', body: JSON.stringify(payload)});
  const returned = saved.device || saved.config || saved;
  state.device = normalizeDeviceConfig(returned.source_mode || returned.mode || returned.input_mode ? returned : payload);
  localStorage.setItem(DEVICE_STORAGE_KEY, JSON.stringify(state.device));
}

async function api(path, opts = {}) {
  const res = await fetch(path, {headers: {'Content-Type': 'application/json'}, ...opts});
  if (res.status === 401) {
    location.href = '/login.html';
    throw new Error('401 Unauthorized');
  }
  if (!res.ok) {
    let detail = '';
    try {
      const body = await res.clone().json();
      detail = [body.error, body.field ? `${t('field')}: ${body.field}` : ''].filter(Boolean).join(', ');
    } catch (_) {
      try { detail = await res.text(); } catch (_) {}
    }
    throw new Error(`${res.status} ${res.statusText}${detail ? `: ${detail}` : ''}`);
  }
  if (res.status === 204) return {};
  return res.json();
}

function escapeHtml(v) {
  return String(v ?? '').replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;').replaceAll("'", '&#39;');
}
function secText(s) {
  const total = Math.max(0, Math.floor(Number(s || 0)));
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const sec = total % 60;
  return `${h}h ${m}m ${sec}s`;
}
function mb(n) { return Math.round(Number(n || 0) / 1048576); }
function t(key) { return window.GB_I18N ? GB_I18N.t(key, currentLang) : key; }
function tf(key, values = {}) {
  return Object.entries(values).reduce((text, [k, v]) => text.replaceAll(`{${k}}`, String(v)), t(key));
}
function boolText(v) { return v ? t('yes') : t('no'); }
function yesNo(v) { return v ? t('enabled') : t('disabled'); }
function setText(sel, text) { const el = $(sel); if (el) el.textContent = text; }
function maxClientValue(key) {
  return gbClients().reduce((max, c) => Math.max(max, Number(c[key] || 0)), 0);
}
function mediaRuntimeRunning() {
  return gbClients().some((c) => !!c.media_running);
}
function statusPill(level, text) {
  return `<span class="status-dot ${level}">${escapeHtml(text)}</span>`;
}

function mergeDeep(base, override) {
  const out = Array.isArray(base) ? [...base] : {...base};
  Object.entries(override || {}).forEach(([key, value]) => {
    if (value && typeof value === 'object' && !Array.isArray(value) && base && typeof base[key] === 'object') {
      out[key] = mergeDeep(base[key], value);
    } else {
      out[key] = value;
    }
  });
  return out;
}

async function loadOemConfig() {
  try {
    const res = await fetch('/api/oem', {cache: 'no-store', credentials: 'same-origin'});
    state.oem = mergeDeep(DEFAULT_OEM, res.ok ? await res.json() : {});
  } catch (_) {
    try {
      const res = await fetch('/oem.config.json', {cache: 'no-store', credentials: 'same-origin'});
      state.oem = mergeDeep(DEFAULT_OEM, res.ok ? await res.json() : {});
    } catch (_) {
      state.oem = DEFAULT_OEM;
    }
  }
  if (window.GB_I18N && !localStorage.getItem(GB_I18N.storageKey) && state.oem.defaults?.language) {
    currentLang = GB_I18N.setLanguage(state.oem.defaults.language);
  }
  applyOemConfig();
}

function deepValue(obj, path) {
  return String(path).split('.').reduce((cur, part) => cur && cur[part] !== undefined ? cur[part] : undefined, obj);
}

function setDeepValue(obj, path, value) {
  const parts = String(path).split('.');
  let cur = obj;
  parts.slice(0, -1).forEach((part) => {
    if (!cur[part] || typeof cur[part] !== 'object') cur[part] = {};
    cur = cur[part];
  });
  cur[parts[parts.length - 1]] = value;
}

function fillOemSettingsForm() {
  const form = $('#oemSettingsForm');
  if (!form) return;
  const oem = state.oem || DEFAULT_OEM;
  Array.from(form.elements).forEach((el) => {
    if (!el.name) return;
    const value = deepValue(oem, el.name);
    if (value !== undefined) el.value = value;
  });
}

function collectOemSettingsForm() {
  const next = mergeDeep(DEFAULT_OEM, state.oem || {});
  const form = $('#oemSettingsForm');
  if (!form) return next;
  new FormData(form).forEach((value, key) => setDeepValue(next, key, value));
  return next;
}

function applyOemConfig() {
  const oem = state.oem || DEFAULT_OEM;
  const brand = oem.brand || DEFAULT_OEM.brand;
  const theme = oem.theme || DEFAULT_OEM.theme;
  const vars = {
    background: '--bg',
    surface: '--panel',
    surfaceMuted: '--panel-2',
    text: '--text',
    muted: '--muted',
    line: '--line',
    primary: '--accent',
    secondary: '--accent-2',
    navigation: '--nav',
    success: '--ok',
    warning: '--warn',
    danger: '--danger'
  };
  Object.entries(vars).forEach(([key, cssVar]) => {
    if (theme[key]) document.documentElement.style.setProperty(cssVar, theme[key]);
  });
  document.title = t('productName');
  setText('#brandName', t('productName'));
  setText('#brandSubtitle', t('productSubtitle'));
  setText('#brandCopyright', brand.copyright || brand.manufacturerName || '');
  const mark = $('#brandMark');
  if (mark) {
    mark.textContent = brand.logoText || brand.shortName || 'GB';
    mark.style.backgroundImage = brand.logoUrl ? `url("${brand.logoUrl}")` : '';
    mark.classList.toggle('has-logo', !!brand.logoUrl);
  }
  if (brand.faviconUrl) {
    let icon = document.querySelector('link[rel="icon"]');
    if (!icon) {
      icon = document.createElement('link');
      icon.rel = 'icon';
      document.head.appendChild(icon);
    }
    icon.href = brand.faviconUrl;
  }
  applyModuleVisibility();
}

function applyModuleVisibility() {
  const modules = (state.oem && state.oem.modules) || DEFAULT_OEM.modules;
  let firstVisible = '';
  $$('.nav button[data-view]').forEach((btn) => {
    const enabled = modules[btn.dataset.view] !== false;
    btn.hidden = !enabled;
    if (enabled && !firstVisible) firstVisible = btn.dataset.view;
  });
  $$('.view').forEach((view) => {
    const enabled = modules[view.id] !== false;
    view.hidden = !enabled;
  });
  const active = $('.view.active');
  if ((!active || active.hidden) && firstVisible) {
    $$('.nav button[data-view]').forEach((b) => b.classList.toggle('active', b.dataset.view === firstVisible));
    $$('.view').forEach((v) => v.classList.toggle('active', v.id === firstVisible));
    setPreviewRunning(firstVisible === 'devices');
  }
}

function applyI18n() {
  document.documentElement.lang = currentLang === 'zh' ? 'zh-CN' : currentLang;
  document.title = t('productName');
  setText('#brandName', t('productName'));
  setText('#brandSubtitle', t('productSubtitle'));
  const bindings = [
    ['[data-view="dashboard"]', 'navDashboard'], ['[data-view="platforms"]', 'navPlatforms'], ['[data-view="av"]', 'navAv'], ['[data-view="devices"]', 'navDevices'], ['[data-view="logs"]', 'navLogs'], ['[data-view="system"]', 'navSystem'],
    ['#dashboard .page-head h1', 'dashboardTitle'], ['#dashboard .page-head p', 'dashboardDesc'],
    ['.kpi-grid .kpi:nth-child(1) label', 'kDevice'], ['.kpi-grid .kpi:nth-child(2) label', 'kSip'], ['.kpi-grid .kpi:nth-child(3) label', 'kStream'], ['.kpi-grid .kpi:nth-child(4) label', 'resourceLoad'],
    ['#dashboard .dashboard-grid .panel:nth-child(1) h2', 'platformLinks'], ['#dashboard .dashboard-grid .panel:nth-child(2) h2', 'alarms'], ['#dashboard .dashboard-grid .panel:nth-child(3) h2', 'recentLogs'], ['#dashboardReloadLogs', 'refresh'],
    ['#platforms .page-head h1', 'platformsTitle'], ['#platforms .page-head p', 'platformsDesc'], ['#addChannel', 'addChannel'],
    ['#av .page-head h1', 'avTitle'], ['#av .page-head p', 'avDesc'],
    ['#devices .page-head h1', 'devicesTitle'], ['#devices .page-head p', 'devicesDesc'], ['#saveDevices', 'saveDevices'], ['#refreshDevices', 'refreshDevices'], ['#deviceForm h2', 'inputSource'], ['#channelBindingsTitle', 'boundChannels'], ['#channelBindingsHint', 'boundChannelsHint'], ['.device-preview h2', 'currentSelection'],
    ['#logs .page-head h1', 'logsTitle'], ['#logs .page-head p', 'logsDesc'], ['#reloadLogs', 'reloadLogs'], ['.tabs button[data-log="ALL"]', 'all'],
    ['#settingsBtn', 'settings'], ['#settingsTitle', 'settings'], ['#settingsClose', 'close'], ['#settingsLanguageTitle', 'language'],
    ['#settingsOtaTitle', 'settingsUpdatesTitle'], ['#settingsAccountTitle', 'settingsAccountTitle'],
    ['#logoutBtn', 'logout']
  ];
  bindings.forEach(([sel, key]) => setText(sel, t(key)));
  const dataLabels = {
    'form.codec': t('codec'), 'form.resolution': t('resolution'), 'form.rateControl': t('rateControl'), 'form.bitrate': t('bitrate'), 'form.iframe': t('iframe'), 'form.enableAudio': t('enableAudio'), 'form.sampleRate': t('sampleRate')
  };
  $$('[data-i18n]').forEach((el) => { el.textContent = dataLabels[el.dataset.i18n] || t(el.dataset.i18n); });
  $$('[data-i18n-aria]').forEach((el) => { el.setAttribute('aria-label', t(el.dataset.i18nAria)); });
  $$('[data-device-label]').forEach((el) => { el.textContent = t(el.dataset.deviceLabel); });
  const pacing = $('#deviceForm select[name="file_pacing"]');
  if (pacing) { pacing.options[0].textContent = t('realtimePacing'); pacing.options[1].textContent = t('fastRead'); }
  renderLanguageSwitch();
  renderUpdateModeText();
  setText('#recognizeTitle', t('recognizeTitle'));
  setText('#recognizeClose', t('recognizeClose'));
  setText('#recognizeCancel', t('recognizeCancel'));
  setText('#recognizeApply', t('recognizeApply'));
  const recognizeText = $('#recognizeText');
  if (recognizeText) recognizeText.placeholder = t('recognizePlaceholder');
}

function renderLanguageSwitch() {
  const box = $('#settingsLangSwitch');
  if (!box || !window.GB_I18N) return;
  box.innerHTML = GB_I18N.supportedLanguages()
    .map((lang) => `<button type="button" data-lang="${escapeHtml(lang)}">${escapeHtml(GB_I18N.t('langName', lang))}</button>`)
    .join('');
  box.querySelectorAll('button[data-lang]').forEach((btn) => {
    btn.classList.toggle('active', btn.dataset.lang === currentLang);
    btn.addEventListener('click', () => setLanguage(btn.dataset.lang));
  });
}

function renderUpdateModeText() {
  const embedded = !!state.settings?.update?.embedded_ota;
  setText('#updateModeText', embedded ? t('embeddedOtaAvailable') : t('desktopUpdateOnly'));
}

function setLanguage(lang) {
  currentLang = window.GB_I18N ? GB_I18N.setLanguage(lang) : lang;
  applyI18n();
  renderMediaFileOptions();
  if (state.av) renderAvForms();
  renderDashboard();
  renderChannelBindings();
  if (state.platforms.length) loadPlatforms();
}

function gbClients() {
  const clients = state.status && Array.isArray(state.status.gb_clients) ? state.status.gb_clients : [];
  const ids = new Set((state.platforms || []).map((p) => Number(p.client_id || 1)).filter(Boolean));
  return ids.size ? clients.filter((c) => ids.has(Number(c.id))) : clients;
}
function gbClientForPlatform(p, idx) {
  const clients = gbClients();
  return clients.find((c) => Number(c.id) === Number(p.client_id || 1)) || clients[idx] || null;
}
function statusRegistered(idx) { const regs = (state.status && state.status.sip_registered) || []; return !!regs[idx]; }
function clientRegistered(client, idx) { return client ? !!client.registered : statusRegistered(idx); }
function clientEnabled(p, client) { return client && client.enabled !== undefined ? !!client.enabled : !!p.enabled; }
function clientPushActive(client) { return client ? !!client.push_active : !!(state.status && state.status.streaming); }
function channelPushActive(p, client) {
  return p && p.push_active !== undefined ? !!p.push_active : clientPushActive(client);
}
function channelStatusClass(p, client, idx) {
  if (!clientEnabled(p, client)) return 'idle';
  if (!client || !client.configured) return 'warn';
  if (channelPushActive(p, client)) return 'danger';
  if (!clientRegistered(client, idx)) return 'warn';
  return 'ok';
}
function channelStatusText(p, client, idx) {
  if (channelPushActive(p, client)) return t('pushing');
  if (!clientEnabled(p, client)) return t('disabled');
  if (!client) return t('noRuntime');
  if (!client.configured) return t('notConfigured');
  if (!clientRegistered(client, idx)) return t('registerFailed');
  return t('registered');
}

function setStatus(d) {
  state.status = {...(state.status || {}), ...d};
  if (Array.isArray(d.channel_statuses) && Array.isArray(state.platforms)) {
    const byId = new Map(d.channel_statuses.map((s) => [Number(s.id), s]));
    state.platforms = state.platforms.map((p) => {
      const s = byId.get(Number(p.id));
      return s ? {
        ...p,
        push_active: !!s.push_active,
        talkback_active: !!s.talkback_active,
        broadcast_active: !!s.broadcast_active,
        media_running: !!s.media_running,
        media_generation: s.media_generation
      } : p;
    });
  }
  const clients = gbClients();
  const regs = state.status.sip_registered || [];
  const clientIds = new Set((state.platforms || []).map((p) => Number(p.client_id || 1)).filter(Boolean));
  const createdTotal = clientIds.size || clients.length || regs.length || 1;
  const registered = clients.length ? clients.filter((c, i) => clientRegistered(c, i)).length : regs.slice(0, createdTotal).filter(Boolean).length;
  const total = createdTotal;
  const channels = state.platforms || [];
  const pushTotal = channels.length || Number(state.status.channel_count || 0) || 0;
  const activePush = channels.length
    ? channels.filter((p) => !!p.push_active).length
    : (state.status.streaming ? 1 : 0);
  const cpuPct = Math.min(100, Math.max(0, Number(state.status.cpu_load_percent || 0)));
  const memPct = state.status.mem_total ? Math.round(state.status.mem_used * 100 / state.status.mem_total) : 0;
  setText('#deviceStatus', state.status.device_status || 'running');
  setText('#appVersion', state.status.version ? `v${state.status.version}` : '--');
  setText('#sipStatus', `${registered}/${total} ${t('online')}`);
  setText('#sipDetail', clients.length ? clients.map((c) => `${t('channel')} ${c.id} ${c.registered ? t('registered') : t('offline')} / ${c.sdk_state || 'unknown'}`).join(' | ') : '--');
  setText('#streamStatus', `${activePush}/${pushTotal || total} ${t('pushing')}`);
  setText('#streamDetail', activePush ? `${activePush} ${t('channel')} ${t('pushing')}` : t('waitingInvite'));
  setText('#resourceStatus', `${cpuPct.toFixed(1)}% CPU`);
  setText('#resourceDetail', `${t('memory')} ${memPct}% | ${mb(state.status.mem_used)} / ${mb(state.status.mem_total)} MB`);
  setText('#uptime', `${t('uptime')} ${secText(state.status.uptime_sec || 0)}`);
  setText('#lastRefresh', new Date().toLocaleTimeString());
  renderAlerts();
}

function renderDashboard() { renderPlatforms(); renderRecentLogs(); renderAlerts(); renderDeviceSummary(); }

function renderPlatforms() {
  const box = $('#platformSummary');
  if (!box) return;
  if (!state.platforms.length) {
    box.innerHTML = `<div class="device-note">${escapeHtml(t('notConfigured'))}</div>`;
    return;
  }
  box.innerHTML = state.platforms.map((p, idx) => {
    const client = gbClientForPlatform(p, idx);
    const cls = channelStatusClass(p, client, idx);
    const text = channelStatusText(p, client, idx);
    const registeredText = clientRegistered(client, idx) ? t('registered') : t('offline');
    const pushText = channelPushActive(p, client) ? t('pushing') : t('idle');
    const title = `${t('channel')} ${p.id}${p.name ? ` · ${p.name}` : ''}`;
    const runtime = [
      client && client.sdk_state ? `SDK ${client.sdk_state}` : '',
      client && client.local_sip_port ? `${t('localSip')} ${client.local_sip_port}` : ''
    ].filter(Boolean).join(' | ');
    return `
      <article class="platform-card">
        <div class="platform-card-head">
          <span class="status-dot ${cls}" title="${escapeHtml(text)}"></span>
          <h3>${escapeHtml(title)}</h3>
          <span class="platform-card-state">${escapeHtml(text)}</span>
        </div>
        <p>${escapeHtml(p.server_ip || '--')}:${escapeHtml(p.sip_port || '--')}</p>
        <p class="platform-runtime-line">${escapeHtml(runtime || '--')}</p>
        <div class="platform-meta">
          <span class="chip">${escapeHtml(p.transport || '--')}</span>
          <span class="chip">${escapeHtml(registeredText)}</span>
          <span class="chip">${escapeHtml(pushText)}</span>
        </div>
      </article>`;
  }).join('');
}

function renderDeviceSummary() {
  const d = state.device || defaultDeviceConfig();
  const rows = d.source_mode === 'file'
    ? [[t('inputSource'), t('fileInput')], [t('mediaFile'), d.media_file || '--'], [t('inputMode'), t('decodeSource')], [t('resolution'), d.resolution || '--'], [t('bitrate'), d.bitrate_kbps ? `${d.bitrate_kbps} Kbps` : '--'], [t('pacing'), d.file_pacing === 'fast' ? t('fast') : t('realtimePacing')], [t('loopFile'), boolText(d.file_loop)]]
    : d.source_mode === 'screen'
      ? [[t('inputSource'), t('screenSource')], [t('screenInput'), deviceDisplayName('screen', d.video_device) || d.video_device || '--'], [t('resolution'), d.resolution || '--']]
    : [[t('inputSource'), t('captureInput')], [t('videoDevice'), d.video_device || '--'], [t('audioDevice'), d.audio_device || '--']];
  setText('#deviceModeBadge', d.source_mode === 'file' ? 'FILE' : (d.source_mode === 'screen' ? 'SCREEN' : 'DEVICE'));
  const box = $('#deviceSummary');
  if (box) box.innerHTML = rows.map(([k, v]) => `<div class="kv"><span>${escapeHtml(k)}</span><b>${escapeHtml(v)}</b></div>`).join('');
  renderDeviceRuntimeStatus();
}

function renderDeviceRuntimeStatus() {
  const box = $('#deviceRuntimeStatus');
  if (!box) return;
  const d = state.device || defaultDeviceConfig();
  const applied = (state.status && state.status.applied_source) || {};
  const sourceMode = applied.source_mode || d.source_mode;
  const videoId = applied.video_device || d.video_device || '';
  const audioId = applied.audio_device || d.audio_device || '';
  const videoInfo = sourceMode === 'device' && videoId ? deviceInfoById('video', videoId) : null;
  const audioInfo = sourceMode === 'device' && audioId ? deviceInfoById('audio', audioId) : null;
  const running = mediaRuntimeRunning();
  const preview = state.preview;
  const now = Date.now();
  previewFrameTimes = previewFrameTimes.filter((at) => now - at < 1000);
  const rows = [];
  if (sourceMode === 'device') {
    rows.push([t('selectedAudio'), audioInfo ? `${audioInfo.name || audioId} | ${audioInfo.backend || ''}` : (audioId || '--')]);
    const activeRefs = [];
    if (running) activeRefs.push(t('serviceMediaThread'));
    if (preview && preview.ok) activeRefs.push(t('previewSessionOwner'));
    rows.push([t('cameraRefs'), activeRefs.length ? activeRefs.join(' / ') : '--']);
  } else if (sourceMode === 'screen') {
    rows.push([t('screenInput'), deviceDisplayName('screen', videoId) || videoId || '--']);
  } else if (sourceMode !== 'none') {
    rows.push([t('mediaFile'), applied.media_file || d.media_file || '--']);
  }
  rows.push([t('previewFrames'), `${previewFrameTimes.length}/s`]);
  const notes = [];
  if (sourceMode === 'device' && running && videoInfo) notes.push(t('possibleCameraBusy'));
  const encoded = maxClientValue('media_frames_encoded');
  if (sourceMode === 'device' && running && encoded === 0) notes.push(t('noRawFrames'));
  box.innerHTML = `
    <div class="runtime-grid">
      ${rows.map(([k, v, html]) => `<div class="kv"><span>${escapeHtml(k)}</span><b>${html ? v : escapeHtml(v)}</b></div>`).join('')}
    </div>
    ${notes.length ? `<div class="device-note warn">${notes.map(escapeHtml).join('<br>')}</div>` : ''}
  `;
}

async function refreshPreviewFrame() {
  const img = $('#videoPreview');
  const form = $('#deviceForm');
  if (previewInFlight) return;
  if (img) {
    previewInFlight = true;
    const data = form ? formValues(form) : (state.device || defaultDeviceConfig());
    const params = new URLSearchParams({source_mode: data.source_mode || 'device', t: String(Date.now())});
    if (data.source_mode === 'file') params.set('media_file', data.media_file || '');
    else if (data.source_mode === 'screen') params.set('video_device', data.screen_source || data.video_device || '');
    else params.set('video_device', data.video_device || '');
    const sourceKey = `${params.get('source_mode') || ''}:${params.get('media_file') || params.get('video_device') || ''}`;
    if (sourceKey !== previewSourceKey) {
      previewSourceKey = sourceKey;
      previewFrameTimes = [];
    }
    const seq = ++previewSeq;
    previewController = new AbortController();
    try {
      const res = await fetch(`/api/preview.jpg?${params.toString()}`, {
        cache: 'no-store',
        credentials: 'same-origin',
        signal: previewController.signal,
      });
      if (!res.ok) throw new Error(`preview ${res.status}`);
      const blob = await res.blob();
      if (seq !== previewSeq) return;
      const frameAt = Date.now();
      previewFrameTimes.push(frameAt);
      previewFrameTimes = previewFrameTimes.filter((at) => frameAt - at < 1000);
      state.preview = {ok: true, bytes: blob.size, frames: previewFrameTimes.length, at: frameAt};
      const nextUrl = URL.createObjectURL(blob);
      const oldUrl = previewObjectUrl;
      previewObjectUrl = nextUrl;
      img.src = nextUrl;
      if (oldUrl) URL.revokeObjectURL(oldUrl);
      renderDeviceRuntimeStatus();
    } catch (_) {
      if (seq !== previewSeq) return;
      state.preview = {ok: false, bytes: 0, at: Date.now()};
      renderDeviceRuntimeStatus();
    } finally {
      if (seq === previewSeq) previewInFlight = false;
    }
  }
}

function setPreviewRunning(running) {
  if (running && !previewTimer) {
    const fps = Number(state.status?.preview_fps || DEFAULT_PREVIEW_FPS);
    const intervalMs = Number(state.status?.preview_interval_ms || Math.round(1000 / Math.max(1, fps)));
    refreshPreviewFrame();
    previewTimer = setInterval(refreshPreviewFrame, Math.max(1, intervalMs));
  } else if (!running && previewTimer) {
    clearInterval(previewTimer);
    previewTimer = null;
    previewSeq++;
    if (previewController) previewController.abort();
    previewController = null;
    previewInFlight = false;
  }
}

function renderAlerts() {
  const alarms = Array.isArray(state.alarms)
    ? state.alarms.map((a) => ({...a, text: a.message || a.text || ''}))
    : [];
  const empty = [{level: 'ok', title: t('noAlarms'), text: t('normal')}];
  const view = alarms.length ? alarms : empty;
  if ($('#alarmCount')) $('#alarmCount').textContent = String(alarms.length);
  if ($('#alertStrip')) $('#alertStrip').innerHTML = view.slice(0, 1).map((a) => `<div class="alert ${a.level}"><b>${escapeHtml(a.title)}</b> | ${escapeHtml(a.text)}</div>`).join('');
  if ($('#alarmList')) $('#alarmList').innerHTML = view.map((a) => `<div class="alarm-item ${a.level}"><strong>${escapeHtml(a.title)}</strong><span>${escapeHtml(a.text)}</span></div>`).join('');
}

function renderRecentLogs() {
  const box = $('#recentLogs');
  if (!box) return;
  box.innerHTML = state.logs.slice(0, 80).map((r) => `<div class="log-line"><span>${new Date(r.ts * 1000).toLocaleString()}</span><b>${escapeHtml(r.category)}</b><span>${escapeHtml(r.message)}</span></div>`).join('');
}

function renderLogTabs() {
  const tabs = $('#logs .tabs');
  if (!tabs) return;
  const categories = [...new Set(state.logs.map((r) => r.category).filter(Boolean))].sort();
  if (logFilter !== 'ALL' && !categories.includes(logFilter)) logFilter = 'ALL';
  tabs.innerHTML = [
    `<button data-log="ALL" class="${logFilter === 'ALL' ? 'active' : ''}">${escapeHtml(t('all'))}</button>`,
    ...categories.map((category) => `<button data-log="${escapeHtml(category)}" class="${logFilter === category ? 'active' : ''}">${escapeHtml(category)}</button>`)
  ].join('');
}

function renderMediaFileOptions(errorText = '') {
  const select = $('#mediaFileSelect');
  const status = $('#mediaFileStatus');
  if (!select) return;
  const current = (state.device && state.device.media_file) || select.value;
  select.innerHTML = state.mediaFiles.map((file) => {
    const path = mediaFilePath(file);
    const res = mediaFileResolution(file);
    const bitrate = mediaFileBitrate(file);
    const meta = [res, bitrate ? `${bitrate} Kbps` : '', t('decodeSource')].filter(Boolean).join(' | ');
    const label = meta ? `${path} (${meta})` : path;
    return `<option value="${escapeHtml(path)}">${escapeHtml(label)}</option>`;
  }).join('');
  if (current && !state.mediaFiles.some((file) => mediaFilePath(file) === current)) {
    select.insertAdjacentHTML('afterbegin', `<option value="${escapeHtml(current)}">${escapeHtml(current)} | ${t('unavailable')}</option>`);
  }
  customBoundSources().forEach((p) => {
    if (p.source_mode === 'file' && p.media_file && ![...select.options].some((o) => o.value === p.media_file)) {
      select.insertAdjacentHTML('afterbegin', `<option value="${escapeHtml(p.media_file)}">${escapeHtml(p.media_file)} | ${t('unavailable')}</option>`);
    }
  });
  if (select.options.length) select.value = current || select.options[0].value;
  if (state.device && (!state.device.media_file || !state.mediaFiles.some((file) => mediaFilePath(file) === state.device.media_file)) && select.options.length) {
    state.device.media_file = select.value;
  }
  select.disabled = !select.options.length;
  if (status) status.textContent = errorText || (select.options.length ? `${select.options.length} files` : t('noMediaFiles'));
  syncSelectedMediaFileMeta();
}

function syncSelectedMediaFileMeta() {
  const form = $('#deviceForm');
  if (!form) return;
  const mediaFile = field(form, 'media_file');
  const resolutionInput = field(form, 'resolution');
  const bitrateInput = field(form, 'bitrate_kbps');
  if (!mediaFile || !resolutionInput || !bitrateInput) return;
  const file = selectedMediaFile(mediaFile.value);
  const resolution = mediaFileResolution(file) || resolutionInput.value;
  const bitrate = mediaFileBitrate(file) || Number(bitrateInput.value || 0);
  resolutionInput.value = resolution;
  bitrateInput.value = bitrate || '';
}

function formValues(form) {
  const out = {};
  new FormData(form).forEach((value, key) => out[key] = value);
  form.querySelectorAll('input[type=number]').forEach((el) => out[el.name] = Number(el.value));
  form.querySelectorAll('input[type=checkbox]').forEach((el) => out[el.name] = el.checked);
  return out;
}

function fillForm(form, data) {
  if (!form || !data) return;
  Object.entries(data).forEach(([k, v]) => {
    const matches = fields(form, k);
    if (!matches.length) return;
    if (matches.some((el) => el.type === 'radio')) {
      matches.forEach((el) => { el.checked = String(el.value) === String(v); });
      return;
    }
    matches.forEach((el) => {
      if (el.type === 'checkbox') el.checked = !!v;
      else el.value = v;
    });
  });
}

function applyDeviceMode() {
  const form = $('#deviceForm');
  if (!form) return;
  const sourceMode = form.querySelector('input[name="source_mode"]:checked') || field(form, 'source_mode');
  const mode = sourceMode?.value || state.device?.source_mode || 'device';
  const isFile = mode === 'file';
  const isScreen = mode === 'screen';
  form.classList.toggle('is-file-mode', isFile);
  form.classList.toggle('is-screen-mode', isScreen);
  form.classList.toggle('is-device-mode', !isFile && !isScreen);
  if (isFile) syncSelectedMediaFileMeta();
  state.device = buildDevicePayload({...(state.device || defaultDeviceConfig()), ...formValues(form)});
  if (!isFile && !isScreen) {
    state.device.media_file = '';
    state.device.resolution = '';
    state.device.bitrate_kbps = 0;
  }
  renderDeviceSummary();
  renderChannelBindings();
  if (previewTimer) refreshPreviewFrame();
}

function sourceConfigured(source) {
  if (!source) return false;
  if (source.source_mode === 'file') return !!source.media_file;
  if (source.source_mode === 'screen') return !!source.video_device;
  if (source.source_mode === 'device') return !!source.video_device;
  return false;
}

function describeSource(source, profile = 'custom') {
  if (profile === 'none') return t('noSource');
  if (profile === 'global' && !sourceConfigured(source)) return t('defaultSourceNotConfigured');
  if (!sourceConfigured(source)) return t('noSource');
  if (source.source_mode === 'file') return `${t('fileInput')}: ${source.media_file}`;
  if (source.source_mode === 'screen') return `${t('screenSource')}: ${deviceDisplayName('screen', source.video_device) || source.video_device}`;
  const parts = [deviceDisplayName('video', source.video_device) || t('noSource')];
  if (source.audio_device) parts.push(deviceDisplayName('audio', source.audio_device));
  return `${t('captureInput')}: ${parts.join(' / ')}`;
}

function sourceDetailRows(source, profile = 'custom') {
  if (profile === 'none') return [[t('inputSource'), t('noSource')]];
  if (!sourceConfigured(source)) return [[t('inputSource'), profile === 'global' ? t('defaultSourceNotConfigured') : t('noSource')]];
  if (source.source_mode === 'file') return [
    [t('inputMode'), t('fileInput')],
    [t('mediaFile'), source.media_file || '--']
  ];
  if (source.source_mode === 'screen') {
    const screenInfo = deviceInfoById('screen', source.video_device);
    return [
      [t('inputMode'), t('screenSource')],
      [t('screenInput'), deviceDisplayName('screen', source.video_device) || '--'],
      [t('screenId'), source.video_device || '--'],
      [t('deviceEnumerated'), screenInfo ? t('present') : t('unavailable')]
    ];
  }
  const videoInfo = deviceInfoById('video', source.video_device);
  const audioInfo = deviceInfoById('audio', source.audio_device);
  return [
    [t('inputMode'), t('captureInput')],
    [t('videoDevice'), deviceDisplayName('video', source.video_device) || '--'],
    [t('videoId'), source.video_device || '--'],
    [t('audioDevice'), source.audio_device ? (deviceDisplayName('audio', source.audio_device) || source.audio_device) : '--'],
    [t('audioId'), source.audio_device || '--'],
    [t('deviceEnumerated'), videoInfo || audioInfo ? t('present') : t('unavailable')]
  ];
}

function showSourceDetails(source, profile = 'custom') {
  const rows = sourceDetailRows(source, profile);
  const text = rows.map(([k, v]) => `${k}: ${v}`).join('\n');
  alert(`${t('sourceDetails')}\n\n${text}`);
}

function currentDevicePayload() {
  const form = $('#deviceForm');
  return buildDevicePayload(form ? formValues(form) : (state.device || defaultDeviceConfig()));
}

function channelSourcePayload(channel, sourceProfile, source = null) {
  const payload = {...channel, source_profile: sourceProfile};
  delete payload.capabilities;
  if (sourceProfile === 'none') {
    payload.source_mode = 'device';
    payload.video_device = '';
    payload.audio_device = '';
    payload.media_file = '';
    payload.resolution = '';
    payload.bitrate_kbps = 0;
    payload.file_loop = true;
    payload.file_pacing = 'realtime';
  } else if (source) {
    Object.assign(payload, source);
    payload.source_profile = sourceProfile;
  }
  return payload;
}

async function bindChannelToCurrentSource(id) {
  const channel = state.platforms.find((p) => Number(p.id) === Number(id));
  if (!channel) return;
  await api(`/api/channels/${id}`, {method: 'PUT', body: JSON.stringify(channelSourcePayload(channel, 'custom', currentDevicePayload()))});
  await loadPlatforms();
  await loadStatus();
}

async function unbindChannelSource(id) {
  const channel = state.platforms.find((p) => Number(p.id) === Number(id));
  if (!channel) return;
  await api(`/api/channels/${id}`, {method: 'PUT', body: JSON.stringify(channelSourcePayload(channel, 'none'))});
  await loadPlatforms();
  await loadStatus();
}

function renderChannelBindings() {
  const box = $('#channelBindings');
  if (!box) return;
  if (!state.platforms.length) {
    box.innerHTML = `<div class="device-note">${escapeHtml(t('notConfigured'))}</div>`;
    return;
  }
  const globalSource = state.device || defaultDeviceConfig();
  box.innerHTML = state.platforms.map((p) => {
    const profile = p.source_profile || 'none';
    const source = profile === 'global' ? globalSource : p;
    return `
      <div class="binding-row" data-id="${p.id}">
        <b>${escapeHtml(platformTitle(p))}</b>
        <span class="binding-source">${escapeHtml(describeSource(source, profile))}</span>
        <button type="button" class="ghost" data-action="source-details">${t('viewDetails')}</button>
        <button type="button" class="ghost" data-action="bind-current">${t('bindCurrentSource')}</button>
        <button type="button" class="ghost" data-action="unbind-source">${t('unbindChannel')}</button>
      </div>`;
  }).join('');
  box.querySelectorAll('[data-action="source-details"]').forEach((btn) => {
    btn.addEventListener('click', () => {
      const p = state.platforms.find((item) => Number(item.id) === Number(btn.closest('.binding-row').dataset.id));
      if (!p) return;
      const profile = p.source_profile || 'none';
      showSourceDetails(profile === 'global' ? globalSource : p, profile);
    });
  });
  box.querySelectorAll('[data-action="bind-current"]').forEach((btn) => {
    btn.addEventListener('click', () => bindChannelToCurrentSource(btn.closest('.binding-row').dataset.id));
  });
  box.querySelectorAll('[data-action="unbind-source"]').forEach((btn) => {
    btn.addEventListener('click', () => unbindChannelSource(btn.closest('.binding-row').dataset.id));
  });
}

const PLATFORM_REQUIRED_FIELDS = ['server_ip', 'sip_port', 'sip_id', 'device_id', 'username', 'password', 'transport', 'media_proto', 'register_interval', 'heartbeat_interval'];

function platformTitle(p) {
  return p.name || `${t('channel')} ${p.id}`;
}

function platformsByClient() {
  const maxClients = Number(state.channelMeta.max_clients || 2);
  const groups = Array.from({length: maxClients}, (_, idx) => ({client_id: idx + 1, channels: []}));
  (state.platforms || []).forEach((p) => {
    if (isEmptyOptionalClientPrimary(p)) return;
    const id = Number(p.client_id || 1);
    const group = groups.find((g) => g.client_id === id) || groups[0];
    group.channels.push(p);
  });
  groups.forEach((g) => g.channels.sort((a, b) => Number(a.ordinal || a.id) - Number(b.ordinal || b.id)));
  return groups;
}

function clientGroup(clientId) {
  return platformsByClient().find((g) => Number(g.client_id) === Number(clientId)) || platformsByClient()[0];
}

function clientPrimaryChannel(group) {
  return group && group.channels && group.channels.length ? group.channels[0] : null;
}

function clientHasEnabledChannel(group) {
  if (!group) return false;
  return !!(group.channels && group.channels.some((ch) => !!ch.enabled));
}

function anyClientHasEnabledChannel(groups) {
  return (groups || platformsByClient()).some((group) => clientHasEnabledChannel(group));
}

function isEmptyOptionalClientPrimary(channel) {
  if (!channel || Number(channel.client_id || 1) === 1 || Number(channel.ordinal || 1) !== 1) return false;
  if (channel.enabled) return false;
  return PLATFORM_REQUIRED_FIELDS.every((key) => {
    if (key === 'transport') return !channel[key] || channel[key] === 'UDP';
    if (key === 'media_proto') return !channel[key] || channel[key] === 'RTC';
    return !channel[key] || Number(channel[key]) === 0;
  });
}

function platformFormData(p) {
  const data = {...p};
  if (!data.enabled) {
    ['sip_port', 'register_interval', 'heartbeat_interval'].forEach((key) => {
      if (Number(data[key] || 0) <= 0) data[key] = '';
    });
    if (!data.name && !data.server_ip && !data.sip_id && !data.device_id && !data.username) data.transport = '';
  }
  data.media_proto = data.media_proto || 'RTC';
  return data;
}

function setPlatformFormLocked(form, locked) {
  form.querySelectorAll('input, select').forEach((el) => {
    if (el.name !== 'enabled') el.disabled = locked;
  });
  form.querySelectorAll('[data-action="recognize-platform"]').forEach((btn) => {
    btn.disabled = locked;
  });
  form.classList.toggle('is-enabled', locked);
}

function setChannelConfigLocked(root, locked) {
  if (!root) return;
  root.classList.toggle('channels-locked', locked);
  root.querySelectorAll('.gb-channel-card input, .gb-channel-card select, .gb-channel-card button').forEach((el) => {
    el.disabled = locked;
  });
  root.querySelectorAll('[data-action="add-client-channel"], [data-action="delete-channel"]').forEach((el) => {
    el.disabled = locked;
  });
}

function normalizeFieldKey(key) {
  return String(key || '').trim().toLowerCase().replace(/[^a-z0-9]/g, '');
}

const PLATFORM_FIELD_KEYS = {
  name: 'name',
  svrip: 'server_ip',
  serverip: 'server_ip',
  platformip: 'server_ip',
  svrport: 'sip_port',
  serverport: 'sip_port',
  platformport: 'sip_port',
  svrid: 'sip_id',
  serverid: 'sip_id',
  platformsipid: 'sip_id',
  account: 'device_id',
  deviceid: 'device_id',
  devicesipid: 'device_id',
  username: 'username',
  authusername: 'username',
  password: 'password',
  authpassword: 'password',
  transport: 'transport',
  mediaproto: 'media_proto',
  mediaprotocol: 'media_proto',
  streamproto: 'media_proto',
  streamprotocol: 'media_proto',
  registerinterval: 'register_interval',
  heartbeatinterval: 'heartbeat_interval'
};

function fieldForAlias(key) {
  return PLATFORM_FIELD_KEYS[normalizeFieldKey(key)] || '';
}

function normalizePlatformValue(field, value) {
  let v = String(value ?? '').trim().replace(/^["']|["']$/g, '');
  if (field === 'transport') {
    const upper = v.toUpperCase();
    v = upper.includes('TRUDP') || upper.includes('TR-UDP') || upper.includes('TR_UDP') ? 'TrUdp' : (upper.includes('TCP') ? 'TCP' : 'UDP');
  }
  if (field === 'media_proto') {
    const upper = v.toUpperCase();
    v = upper.includes('RTP') && !upper.includes('RTC') ? 'RTP' : 'RTC';
  }
  if (['sip_port', 'register_interval', 'heartbeat_interval'].includes(field)) {
    const n = v.match(/\d+/);
    v = n ? n[0] : '';
  }
  return v;
}

function setRecognizedValue(out, rawValues, key, value) {
  const normalized = normalizeFieldKey(key);
  const field = fieldForAlias(key) || (PLATFORM_REQUIRED_FIELDS.includes(key) || key === 'name' ? key : '');
  rawValues[normalized] = value;
  if (field) out[field] = normalizePlatformValue(field, value);
}

function applyPlatformPortAliases(out, rawValues) {
  const transport = normalizePlatformValue('transport', out.transport || rawValues.transport || '');
  if (transport) out.transport = transport;
  const portKey = transport === 'TrUdp'
    ? 'svrtrudpport'
    : (transport === 'TCP' ? 'svrtcpport' : 'svrudpport');
  const fallback = rawValues.svrport || rawValues.serverport || rawValues.platformport;
  const selected = rawValues[portKey] || fallback;
  if (selected !== undefined) out.sip_port = normalizePlatformValue('sip_port', selected);
}

function parsePlatformText(text) {
  const out = {};
  const rawValues = {};
  const raw = String(text || '').trim();
  if (!raw) return out;
  try {
    const json = JSON.parse(raw);
    Object.entries(json || {}).forEach(([key, value]) => {
      setRecognizedValue(out, rawValues, key, value);
    });
  } catch (_) {}
  raw.split(/\r?\n/).forEach((line) => {
    const match = line.match(/^\s*([A-Za-z][A-Za-z0-9_-]*)\s*=\s*(.+?)\s*$/);
    if (!match) return;
    setRecognizedValue(out, rawValues, match[1], match[2]);
  });
  if (out.device_id && !out.username) out.username = out.device_id;
  applyPlatformPortAliases(out, rawValues);
  return out;
}

function openRecognizeModal(form) {
  if (form.classList.contains('is-enabled')) return;
  recognizeTargetForm = form;
  $('#recognizeError').textContent = '';
  $('#recognizeText').value = '';
  $('#recognizeModal').hidden = false;
  $('#recognizeText').focus();
}

function closeRecognizeModal() {
  $('#recognizeModal').hidden = true;
  recognizeTargetForm = null;
}

function openSettingsModal() {
  $('#settingsModal').hidden = false;
  loadSettings();
  fillOemSettingsForm();
}

function closeSettingsModal() {
  $('#settingsModal').hidden = true;
}

async function loadSettings() {
  try {
    state.settings = await api('/api/settings');
    const user = state.settings?.account?.username || 'admin';
    const form = $('#accountSettingsForm');
    if (form?.elements.username) form.elements.username.value = user;
    renderUpdateModeText();
  } catch (_) {
    const form = $('#accountSettingsForm');
    if (form?.elements.username) form.elements.username.value = 'admin';
  }
}

function selectSettingsTab(tabName) {
  $$('#settingsModal [data-settings-tab]').forEach((btn) => btn.classList.toggle('active', btn.dataset.settingsTab === tabName));
  $$('#settingsModal [data-settings-pane]').forEach((pane) => pane.classList.toggle('active', pane.dataset.settingsPane === tabName));
}

async function savePasswordSettings() {
  const form = $('#accountSettingsForm');
  const result = $('#accountSettingsResult');
  if (!form) return;
  try {
    await api('/api/settings/password', {method: 'POST', body: JSON.stringify(formValues(form))});
    form.elements.current_password.value = '';
    form.elements.new_password.value = '';
    if (result) result.textContent = t('passwordSaved');
  } catch (e) {
    if (result) result.textContent = e.message;
  }
}

async function saveOemSettings() {
  const result = $('#oemSettingsResult');
  try {
    const next = collectOemSettingsForm();
    await api('/api/oem', {method: 'PUT', body: JSON.stringify(next, null, 2)});
    state.oem = next;
    applyOemConfig();
    applyI18n();
    if (result) result.textContent = t('templateSaved');
  } catch (e) {
    if (result) result.textContent = e.message;
  }
}

function applyRecognizedPlatform() {
  if (!recognizeTargetForm) return;
  const parsed = parsePlatformText($('#recognizeText').value);
  const fields = ['name', ...PLATFORM_REQUIRED_FIELDS];
  const found = fields.filter((field) => parsed[field]);
  if (!found.length) {
    $('#recognizeError').textContent = t('recognizeFailed');
    return;
  }
  found.forEach((field) => {
    const el = recognizeTargetForm.elements[field];
    if (!el) return;
    el.value = parsed[field];
    el.classList.remove('invalid');
    el.setCustomValidity('');
  });
  closeRecognizeModal();
}

function validatePlatformForm(form) {
  let firstInvalid = null;
  PLATFORM_REQUIRED_FIELDS.forEach((name) => {
    const el = form.elements[name];
    if (!el) return;
    const missing = el.type === 'number' ? Number(el.value) <= 0 : !String(el.value || '').trim();
    el.classList.toggle('invalid', missing);
    el.setCustomValidity(missing ? t('requiredField') : '');
    if (missing && !firstInvalid) firstInvalid = el;
  });
  if (firstInvalid) {
    firstInvalid.reportValidity();
    firstInvalid.focus();
    return false;
  }
  return true;
}

function handleTransportSelectChange(select) {
  const previous = select.dataset.lastTransport || 'UDP';
  if (select.value === 'TrUdp' && !confirm(t('trudpConfirm'))) {
    select.value = previous && previous !== 'TrUdp' ? previous : 'UDP';
    return;
  }
  select.dataset.lastTransport = select.value || 'UDP';
}

async function savePlatformForm(form, enabled) {
  const payload = formValues(form);
  payload.enabled = !!enabled;
  await api(`/api/channels/${form.dataset.id}`, {method: 'PUT', body: JSON.stringify(payload)});
}

async function togglePlatform(form, nextEnabled) {
  const toggle = form.elements.enabled;
  if (nextEnabled && !validatePlatformForm(form)) {
    toggle.checked = false;
    return;
  }
  if (!nextEnabled && !confirm(t('disableChannelConfirm'))) {
    toggle.checked = true;
    return;
  }
  toggle.disabled = true;
  if (nextEnabled) setChannelConfigLocked($('#platformForms'), true);
  try {
    await savePlatformForm(form, nextEnabled);
    await loadStatus();
    await loadPlatforms();
  } catch (e) {
    alert(`${t('saveFailed')}: ${e.message}`);
    toggle.checked = !nextEnabled;
    toggle.disabled = false;
  }
}

async function loadPlatforms() {
  try { state.channelMeta = await api('/api/channels/meta'); } catch (_) { state.channelMeta = {max_clients: 2, channels_per_client: 8, max_channels: 16}; }
  state.platforms = await api('/api/channels');
  const groups = platformsByClient();
  if (!groups.some((g) => Number(g.client_id) === Number(selectedGbClientId))) selectedGbClientId = 1;
  const activeGroup = groups.find((g) => Number(g.client_id) === Number(selectedGbClientId)) || groups[0];
  const perClient = Number(state.channelMeta.channels_per_client || 8);
  const primary = clientPrimaryChannel(activeGroup);
  const clientSlotId = (Number(activeGroup.client_id) - 1) * perClient + 1;
  const clientFormSource = primary || {
    id: clientSlotId,
    client_id: activeGroup.client_id,
    ordinal: 1,
    enabled: false,
    name: 'CH1',
    media_proto: 'RTC',
    transport: 'UDP',
    server_ip: '',
    sip_port: '',
    sip_id: '',
    device_id: '',
    username: '',
    password: '',
    register_interval: '',
    heartbeat_interval: ''
  };
  const clientLocked = clientHasEnabledChannel(activeGroup);
  const channelsLocked = anyClientHasEnabledChannel(groups);
  const clientBody = `
    <form class="gb-client-form" data-id="${clientFormSource.id}" data-client-id="${activeGroup.client_id}" novalidate>
      <div class="form-head">
        <h2>${t('clientConfig')}</h2>
        <div class="channel-actions">
          <button type="button" class="ghost recognize-btn" data-action="recognize-platform">${t('smartRecognize')}</button>
          ${primary && Number(activeGroup.client_id) !== 1 && !clientLocked ? `<button type="button" class="ghost delete-btn" data-action="delete-client">${t('deleteGbClient')}</button>` : ''}
          <label class="toggle" title="${escapeHtml(clientFormSource.enabled ? t('disableChannel') : t('enableChannel'))}">
            <input name="enabled" type="checkbox">
            <span class="toggle-track"><span class="toggle-thumb"></span></span>
            <b class="toggle-text">${clientFormSource.enabled ? t('channelOn') : t('channelOff')}</b>
          </label>
        </div>
      </div>
      <input name="name" type="hidden">
      <input name="media_proto" type="hidden">
      <label>${t('serverIp')}<input name="server_ip" required></label>
      <label>${t('sipPort')}<input name="sip_port" type="number" min="1" required></label>
      <label>${t('sipId')}<input name="sip_id" required></label>
      <label>${t('deviceId')}<input name="device_id" required></label>
      <label>${t('username')}<input name="username" required></label>
      <label>${t('password')}<input name="password" type="password" required></label>
      <label>${t('transport')}<select name="transport" required><option value=""></option><option>UDP</option><option>TCP</option><option>TrUdp</option></select></label>
      <label>${t('registerInterval')}<input name="register_interval" type="number" min="1" required></label>
      <label>${t('heartbeatInterval')}<input name="heartbeat_interval" type="number" min="1" required></label>
    </form>
    <section class="gb-channel-section">
      <div class="sub-head">
        <h3>${t('channelList')}</h3>
        <div class="channel-actions">
          <span class="muted">${tf('clientChannels', {count: activeGroup.channels.length, max: perClient})}</span>
          ${!channelsLocked ? `<button type="button" data-action="add-client-channel" ${!primary || activeGroup.channels.length >= perClient ? 'disabled' : ''}>${t('addClientChannel')}</button>` : ''}
        </div>
      </div>
      <div class="gb-channel-cards">
        ${activeGroup.channels.length ? activeGroup.channels.map((p) => `
          <form class="gb-channel-card" data-id="${p.id}" novalidate>
            <div class="gb-channel-card-head">
              <div>
                <strong>${escapeHtml(platformTitle(p))}</strong>
                <span>${t('channelId')}: ${escapeHtml(p.channel_id || '--')}</span>
              </div>
              ${Number(p.ordinal || 1) !== 1 && !channelsLocked ? `<button type="button" class="ghost delete-btn" data-action="delete-channel">${t('deleteChannel')}</button>` : ''}
            </div>
            <label>${t('name')}<input name="name" ${clientLocked ? 'disabled' : ''}></label>
            <label>${t('mediaProto')}<select name="media_proto" required ${clientLocked ? 'disabled' : ''}><option value="RTP">${t('mediaProtoRtp')}</option><option value="RTC">${t('mediaProtoRtc')}</option></select></label>
            <div class="gb-card-actions">
              <span class="muted">${t('currentSource')}: ${escapeHtml(describeSource(p, p.source_profile || 'none'))}</span>
              <button type="submit" ${clientLocked ? 'disabled' : ''}>${t('save')}</button>
            </div>
          </form>`).join('') : `<section class="gb-client-empty"><p>${t('gbClientEmpty')}</p></section>`}
      </div>
    </section>`;
  $('#platformForms').innerHTML = `
    <div class="gb-platform-shell">
      <nav class="gb-client-tabs" aria-label="${escapeHtml(t('gbClient'))}">
        ${groups.map((group) => `
          <button type="button" class="${Number(group.client_id) === Number(activeGroup.client_id) ? 'active' : ''}" data-client-tab="${group.client_id}">
            <span>${t('gbClient')} ${group.client_id}</span>
            <b>${group.channels.length ? tf('clientChannels', {count: group.channels.length, max: perClient}) : t('notConfigured')}</b>
          </button>`).join('')}
      </nav>
      <div class="gb-client-content" data-client-id="${activeGroup.client_id}">
        ${clientBody}
      </div>
    </div>`;
  $$('#platformForms [data-client-tab]').forEach((btn) => {
    btn.addEventListener('click', async () => {
      selectedGbClientId = Number(btn.dataset.clientTab || 1);
      await loadPlatforms();
    });
  });
  $('#platformForms [data-action="add-client-channel"]')?.addEventListener('click', () => addChannel(activeGroup.client_id));
  const clientForm = $('#platformForms .gb-client-form');
  if (clientForm) {
    fillForm(clientForm, platformFormData(clientFormSource));
    clientForm.elements.media_proto.value = clientFormSource.media_proto || 'RTC';
    if (clientForm.elements.transport) {
      clientForm.elements.transport.dataset.lastTransport = clientForm.elements.transport.value || 'UDP';
      clientForm.elements.transport.addEventListener('change', () => handleTransportSelectChange(clientForm.elements.transport));
    }
    setPlatformFormLocked(clientForm, !!clientFormSource.enabled);
    PLATFORM_REQUIRED_FIELDS.forEach((name) => {
      const el = clientForm.elements[name];
      if (el) el.addEventListener('input', () => {
        el.classList.remove('invalid');
        el.setCustomValidity('');
      });
    });
    clientForm.querySelector('[data-action="recognize-platform"]').addEventListener('click', () => openRecognizeModal(clientForm));
    clientForm.querySelector('[data-action="delete-client"]')?.addEventListener('click', () => deleteClient(activeGroup));
    clientForm.elements.enabled.addEventListener('change', (ev) => togglePlatform(clientForm, ev.currentTarget.checked));
  }
  $$('#platformForms .gb-channel-card').forEach((form) => {
    const channel = state.platforms.find((p) => Number(p.id) === Number(form.dataset.id));
    fillForm(form, {name: channel?.name || '', media_proto: channel?.media_proto || 'RTC'});
    form.addEventListener('submit', async (ev) => {
      ev.preventDefault();
      if (channelsLocked) return;
      await saveChannelCard(form);
    });
    form.querySelector('[data-action="delete-channel"]')?.addEventListener('click', async () => {
      if (!confirm(t('deleteChannelConfirm'))) return;
      try {
        await api(`/api/channels/${form.dataset.id}`, {method: 'DELETE'});
        await loadStatus();
        await loadPlatforms();
        await loadAv();
      } catch (e) {
        alert(`${t('saveFailed')}: ${e.message}`);
        await loadStatus();
        await loadPlatforms();
      }
    });
  });
  setChannelConfigLocked($('#platformForms'), channelsLocked);
  renderDeviceOptions();
  renderMediaFileOptions();
  renderChannelBindings();
  if (state.av) renderAvForms();
  renderDashboard();
}

async function addChannel(clientId = 1) {
  try {
    await api('/api/channels', {method: 'POST', body: JSON.stringify({client_id: clientId})});
    selectedGbClientId = Number(clientId) || 1;
    await loadStatus();
    await loadPlatforms();
    await loadAv();
  } catch (e) {
    alert(`${t('saveFailed')}: ${e.message}`);
    await loadStatus();
    await loadPlatforms();
  }
}

async function saveChannelCard(form) {
  const channel = state.platforms.find((p) => Number(p.id) === Number(form.dataset.id));
  if (!channel) return;
  const payload = {...channel, ...formValues(form)};
  await api(`/api/channels/${form.dataset.id}`, {method: 'PUT', body: JSON.stringify(payload)});
  await loadPlatforms();
  await loadStatus();
}

async function deleteClient(group) {
  if (!group || Number(group.client_id) === 1) return;
  if (!confirm(t('deleteGbClientConfirm'))) return;
  const channels = [...group.channels].sort((a, b) => Number(b.id) - Number(a.id));
  for (const channel of channels) {
    await api(`/api/channels/${channel.id}`, {method: 'DELETE'});
  }
  selectedGbClientId = 1;
  await loadPlatforms();
  await loadAv();
  await loadStatus();
}

async function loadAv() {
  state.av = await api('/api/av');
  renderAvForms();
  renderDashboard();
}

function avChannelTitle(id) {
  const channel = state.platforms.find((p) => Number(p.id) === Number(id));
  return channel ? platformTitle(channel) : `${t('channel')} ${id}`;
}

function renderAvForms() {
  const box = $('#avForms');
  if (!box || !state.av) return;
  const channels = Array.isArray(state.av.channels) ? state.av.channels : [];
  if (!channels.length) {
    selectedAvChannelId = 1;
    avDirty = false;
    box.classList.add('av-editor-wrap');
    box.innerHTML = `<div class="device-note">${escapeHtml(t('noAvChannels'))}</div>`;
    return;
  }
  if (!channels.some((ch) => Number(ch.id) === Number(selectedAvChannelId))) selectedAvChannelId = Number(channels[0]?.id || 1);
  const current = channels.find((ch) => Number(ch.id) === Number(selectedAvChannelId)) || channels[0];
  box.classList.add('av-editor-wrap');
  box.innerHTML = `
    <div class="av-channel-picker">
      <label>${t('selectChannel')}<select id="avChannelSelect">
        ${channels.map((ch) => `<option value="${ch.id}">${escapeHtml(avChannelTitle(ch.id))}</option>`).join('')}
      </select></label>
    </div>
    <div class="forms two av-editor-forms" data-id="${current.id}">
      <form id="videoForm">
        <h2>${t('videoInput')}</h2>
        <label>${t('codec')}<select name="codec"><option>H264</option><option>H265</option></select></label>
        <label>${t('resolution')}<input name="resolution"></label>
        <label>FPS<input name="fps" type="number" min="1" max="60"></label>
        <label>${t('rateControl')}<select name="rc_mode"><option>CBR</option><option>VBR</option></select></label>
        <label>${t('bitrate')}<input name="bitrate_kbps" type="number"></label>
        <label>GOP<input name="gop" type="number"></label>
        <label>${t('iframe')}<input name="iframe_interval" type="number"></label>
        <label class="check"><input name="low_latency" type="checkbox"> <span>${t('lowLatency')}</span></label>
        <label>${t('encoderType')}<select name="encoder_mode">${encoderModeOptions(current.video || {})}</select></label>
      </form>
      <form id="audioForm">
        <h2>${t('audioInput')}</h2>
        <label class="check"><input name="enabled" type="checkbox"> <span>${t('enableAudio')}</span></label>
        <label>${t('codec')}<select name="codec"><option>G711A</option><option>AAC</option></select></label>
        <label>${t('sampleRate')}<input name="sample_rate" type="number"></label>
        <label>${t('bitrate')}<input name="bitrate_kbps" type="number"></label>
      </form>
    </div>
    <div class="form-actions"><button id="saveAv" type="button">${t('saveAv')}</button></div>`;
  const select = $('#avChannelSelect');
  const videoForm = $('#videoForm');
  const audioForm = $('#audioForm');
  select.value = String(current.id);
  fillForm(videoForm, current.video || {});
  const encoderMode = videoForm.elements.encoder_mode;
  if (encoderMode) encoderMode.value = current.video && current.video.prefer_hardware ? 'hardware' : 'software';
  fillForm(audioForm, current.audio || {});
  avDirty = false;
  [videoForm, audioForm].forEach((form) => {
    form.addEventListener('input', () => { avDirty = true; });
    form.addEventListener('change', () => { avDirty = true; });
  });
  $('#saveAv').addEventListener('click', () => saveAvChannel());
  select.addEventListener('change', () => changeAvChannel(Number(select.value)));
}

function flattenAvPayload(ch) {
  const out = {};
  Object.entries(ch.video || {}).forEach(([k, v]) => out[`video.${k}`] = v);
  Object.entries(ch.audio || {}).forEach(([k, v]) => out[`audio.${k}`] = v);
  return out;
}

function nestedAvPayload() {
  const video = formValues($('#videoForm'));
  if (Object.prototype.hasOwnProperty.call(video, 'encoder_mode')) {
    video.prefer_hardware = video.encoder_mode === 'hardware';
    delete video.encoder_mode;
  }
  return {
    video,
    audio: formValues($('#audioForm'))
  };
}

async function saveAvChannel() {
  await api(`/api/av/${selectedAvChannelId}`, {method: 'PUT', body: JSON.stringify(nestedAvPayload())});
  avDirty = false;
  await loadAv();
  await loadStatus();
}

async function changeAvChannel(nextId) {
  const select = $('#avChannelSelect');
  const previousId = selectedAvChannelId;
  if (avDirty) {
    if (confirm(t('unsavedAvConfirm'))) {
      try {
        await saveAvChannel();
      } catch (e) {
        alert(`${t('saveFailed')}: ${e.message}`);
        if (select) select.value = String(previousId);
        return;
      }
    }
  }
  selectedAvChannelId = nextId;
  avDirty = false;
  renderAvForms();
}

async function loadMediaFiles() {
  const status = $('#mediaFileStatus');
  if (status) status.textContent = t('mediaFilesLoading');
  try {
    state.mediaFiles = mediaFilesFromPayload(await api('/api/media/files'));
    renderMediaFileOptions();
  } catch (e) {
    state.mediaFiles = [];
    renderMediaFileOptions(`${t('mediaFilesLoadFailed')}: ${e.message}`);
  }
}

async function loadDevices() {
  try {
    const payload = await api('/api/devices');
    state.device = normalizeDeviceConfig(payload.device || payload.config || payload);
    localStorage.setItem(DEVICE_STORAGE_KEY, JSON.stringify(state.device));
  } catch (_) {
    state.device = loadDeviceConfig();
  }
  renderDeviceOptions();
  fillForm($('#deviceForm'), state.device);
  renderDeviceOptions();
  renderMediaFileOptions();
  applyDeviceMode();
  renderChannelBindings();
}

async function loadStatus() { setStatus(await api('/api/status')); renderDashboard(); }
async function loadAlarms() {
  const payload = await api('/api/alarms');
  state.alarms = Array.isArray(payload) ? payload : (payload.alarms || []);
  renderAlerts();
}
async function loadLogs() {
  state.logs = await api('/api/logs');
  renderLogTabs();
  const box = $('#logBox');
  if (box) box.textContent = state.logs.filter((r) => logFilter === 'ALL' || r.category === logFilter).map((r) => `${new Date(r.ts * 1000).toISOString()} [${r.level}] ${r.category} ${r.message}`).join('\n');
  renderRecentLogs();
}

function connectWs() {
  const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/api/ws`);
  ws.onopen = () => { $('#wsState').textContent = 'WS online'; $('#wsState').className = 'state-pill online'; };
  ws.onclose = () => { $('#wsState').textContent = 'WS offline'; $('#wsState').className = 'state-pill offline'; setTimeout(connectWs, 2000); };
  ws.onmessage = (ev) => { setStatus(JSON.parse(ev.data)); renderDashboard(); };
}

$$('.nav button[data-view]').forEach((btn) => btn.onclick = () => {
  $$('.nav button[data-view]').forEach((b) => b.classList.remove('active'));
  $$('.view').forEach((v) => v.classList.remove('active'));
  btn.classList.add('active');
  $(`#${btn.dataset.view}`).classList.add('active');
  setPreviewRunning(btn.dataset.view === 'devices');
});

$('#addChannel').onclick = () => addChannel(1);

$('#recognizeClose').onclick = closeRecognizeModal;
$('#recognizeCancel').onclick = closeRecognizeModal;
$('#recognizeApply').onclick = applyRecognizedPlatform;
$('#recognizeModal').addEventListener('click', (ev) => {
  if (ev.target.id === 'recognizeModal') closeRecognizeModal();
});

$('#saveDevices').onclick = async () => {
  await saveDeviceConfig(formValues($('#deviceForm')));
  fillForm($('#deviceForm'), state.device);
  renderMediaFileOptions();
  applyDeviceMode();
  renderChannelBindings();
  if (previewTimer) refreshPreviewFrame();
};

$('#refreshDevices').onclick = async () => {
  await loadDevices();
  setPreviewRunning(true);
};
$('#deviceForm .source-choice')?.addEventListener('click', (ev) => {
  const card = ev.target.closest('[data-source-card]');
  if (!card || card.hidden) return;
  const input = card.querySelector('input[name="source_mode"]');
  if (!input || input.disabled) return;
  if (!input.checked) {
    input.checked = true;
    applyDeviceMode();
  }
});
$('#deviceForm').addEventListener('change', applyDeviceMode);
$('#settingsBtn').onclick = openSettingsModal;
$('#settingsClose').onclick = closeSettingsModal;
$('#settingsModal').addEventListener('click', (ev) => {
  if (ev.target.id === 'settingsModal') closeSettingsModal();
});
$$('#settingsModal [data-settings-tab]').forEach((btn) => {
  btn.addEventListener('click', () => selectSettingsTab(btn.dataset.settingsTab));
});
$('#savePasswordBtn')?.addEventListener('click', savePasswordSettings);
$('#saveOemBtn')?.addEventListener('click', saveOemSettings);
$('#checkUpdatesBtn')?.addEventListener('click', async () => {
  const result = $('#updateSettingsResult');
  try {
    const data = await api('/api/updates/check', {method: 'POST', body: '{}'});
    if (result) {
      result.textContent = `${t('updateCheckComplete')} | ${data.current_version || '--'} | ${data.update_available ? t('updateAvailable') : t('noUpdateAvailable')}`;
    }
  } catch (e) {
    if (result) result.textContent = e.message;
  }
});
$('#logoutBtn').onclick = async () => {
  if (!confirm(t('logoutConfirm'))) return;
  try { await api('/api/logout', {method: 'POST', body: '{}'}); } catch (_) {}
  location.href = '/login.html';
};

$('#reloadLogs').onclick = loadLogs;
$('#dashboardReloadLogs').onclick = loadLogs;
$('#logs .tabs')?.addEventListener('click', (ev) => {
  const btn = ev.target.closest('button[data-log]');
  if (!btn) return;
  logFilter = btn.dataset.log;
  loadLogs();
});

async function init() {
  await loadOemConfig();
  applyI18n();
  await loadMediaFiles();
  await loadDevices();
  await Promise.all([loadStatus(), loadPlatforms(), loadAv(), loadLogs(), loadAlarms()]);
  renderDashboard();
  if ($('#devices')?.classList.contains('active')) setPreviewRunning(true);
  connectWs();
}

init();
setInterval(loadStatus, 1000);
setInterval(loadLogs, 1000);
setInterval(loadAlarms, 1000);
window.addEventListener('beforeunload', () => {
  setPreviewRunning(false);
  if (previewObjectUrl) URL.revokeObjectURL(previewObjectUrl);
});
