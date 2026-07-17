// MARK: - Icons

// A single consistent line-icon style throughout: 24x24 viewBox, 1.75
// stroke, round caps/joins, no fills except where a solid glyph reads
// better at small sizes (play/stop/close-dot).
const ICONS = {
  sun: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="4"/><path d="M12 2v2.5M12 19.5V22M4.9 4.9l1.8 1.8M17.3 17.3l1.8 1.8M2 12h2.5M19.5 12H22M4.9 19.1l1.8-1.8M17.3 6.7l1.8-1.8"/></svg>`,
  moon: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M20 14.5A8.5 8.5 0 1 1 9.5 4a6.7 6.7 0 0 0 10.5 10.5Z"/></svg>`,
  providers: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="4" width="18" height="6" rx="1.6"/><rect x="3" y="14" width="18" height="6" rx="1.6"/><circle cx="7" cy="7" r=".6" fill="currentColor" stroke="none"/><circle cx="7" cy="17" r=".6" fill="currentColor" stroke="none"/></svg>`,
  monitor: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M3 13h3.5l2-7 4 14 2-9 1.5 2H21"/></svg>`,
  logs: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M14.5 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8.5L14.5 3Z"/><path d="M14.5 3v4.5a1 1 0 0 0 1 1H20M9 13h6M9 16.5h6"/></svg>`,
  keys: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><circle cx="7.5" cy="12" r="4"/><path d="M11.3 12H21M17 12v3.2M20 12v2.2"/></svg>`,
  settings: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.6a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1Z"/></svg>`,
  plus: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><path d="M12 5v14M5 12h14"/></svg>`,
  search: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><circle cx="11" cy="11" r="6.5"/><path d="M20 20l-4.3-4.3"/></svg>`,
  play: `<svg viewBox="0 0 24 24" fill="currentColor" stroke="none"><path d="M7.5 5.1v13.8l11-6.9-11-6.9Z"/></svg>`,
  stop: `<svg viewBox="0 0 24 24" fill="currentColor" stroke="none"><rect x="6" y="6" width="12" height="12" rx="2"/></svg>`,
  check: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.5l5 5L20 6.5"/></svg>`,
  refresh: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M20 11A8 8 0 1 0 20.9 15.5M20 4v6h-6"/></svg>`,
  trash: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M4 7h16M9.5 7V5a1.5 1.5 0 0 1 1.5-1.5h2A1.5 1.5 0 0 1 14.5 5v2M7 7l.8 12.1a1.6 1.6 0 0 0 1.6 1.4h5.2a1.6 1.6 0 0 0 1.6-1.4L17 7M10.2 11v6M13.8 11v6"/></svg>`,
  copy: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><rect x="8.5" y="8.5" width="11.5" height="11.5" rx="1.8"/><path d="M15.5 8.5V5.8a1.8 1.8 0 0 0-1.8-1.8H5.8A1.8 1.8 0 0 0 4 5.8v7.9a1.8 1.8 0 0 0 1.8 1.8h2.7"/></svg>`,
  logout: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M9.5 21H6a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.5M16 16.5l4.5-4.5-4.5-4.5M20.5 12H9.5"/></svg>`,
  broadcast: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="2" fill="currentColor" stroke="none"/><path d="M8.3 8.3a5.2 5.2 0 0 0 0 7.4M15.7 8.3a5.2 5.2 0 0 1 0 7.4M5 5a10 10 0 0 0 0 14M19 5a10 10 0 0 1 0 14"/></svg>`,
  grid: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="8" height="8" rx="1.6"/><rect x="13" y="3" width="8" height="8" rx="1.6"/><rect x="3" y="13" width="8" height="8" rx="1.6"/><rect x="13" y="13" width="8" height="8" rx="1.6"/></svg>`,
  close: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round"><path d="M6 6l12 12M18 6L6 18"/></svg>`,
  pip: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="4.5" width="18" height="14" rx="1.8"/><rect x="12.5" y="12" width="7" height="5" rx="1.1" fill="currentColor" stroke="none"/></svg>`,
  expand: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M8.5 3.5H4v4.5M15.5 3.5H20v4.5M20 15.5V20h-4.5M4 15.5V20h4.5"/></svg>`,
  download: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3.5v11.5M7.5 11l4.5 4.5L16.5 11M5 18.5h14"/></svg>`,
};

function iconSvg(name) {
  const raw = ICONS[name] || "";
  return raw.replace("<svg ", '<svg class="icon" ');
}

function applyIcons(root) {
  (root || document).querySelectorAll("[data-icon]").forEach((el) => {
    const name = el.dataset.icon;
    if (el.dataset.iconApplied === name) return;
    el.innerHTML = iconSvg(name);
    el.dataset.iconApplied = name;
  });
}

// MARK: - State

let state = { providers: [], apiKeys: [] };
let selectedProviderId = null;
let selectedStreamId = null;
let hls = null;
let pipHls = null;
let currentView = "monitor";
let eventSource = null;
let authenticated = false;
let probeResult = null;
let selectedRepIds = new Set();
let bootStarted = false;
let editingNewStream = false;
let lastEditorStreamId = null;
let decryptionKeysManuallyShown = false;

const $ = (selector) => document.querySelector(selector);

async function request(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: {
      "content-type": "application/json",
      ...(options.headers || {}),
    },
  });
  if (response.status === 401) {
    authenticated = false;
    showAuthScreen();
    throw new Error("Sign-in required.");
  }
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(payload.error || `HTTP ${response.status}`);
  return payload;
}

async function refresh() {
  state = await request("/api/state");
  if (!selectedProviderId && state.providers[0]) selectedProviderId = state.providers[0].id;
  const provider = selectedProvider();
  if (!provider) selectedProviderId = null;
  // Never steal the selection away from an in-progress "New Stream" draft —
  // otherwise the periodic background poll silently swaps selectedStreamId
  // to some existing stream mid-edit, and the whole draft (including any
  // representations just picked) gets replaced by that stream's saved data.
  if (!editingNewStream && !selectedStream() && provider?.streams[0]) selectedStreamId = provider.streams[0].id;
  render();
}

function selectedProvider() {
  return state.providers.find((provider) => provider.id === selectedProviderId);
}

function selectedStream() {
  const provider = selectedProvider();
  return provider?.streams.find((stream) => stream.id === selectedStreamId);
}

function render() {
  renderProviders();
  renderEditor();
  renderKeys();
  renderStreamsGrid();
  renderViewHeader();
  renderUpdateBanner();
  applyIcons();
}

function appVersionLabel() {
  const v = state.version || {};
  return v.isRelease ? `v${v.version}` : "dev build";
}

function appVersionSubtitle() {
  const v = state.version || {};
  if (v.updateAvailable) return `update available: v${v.latest}`;
  if (v.commit && v.commit !== "unknown") return v.commit;
  if (v.error) return v.error;
  return v.latest ? "up to date" : "";
}

// A dismissible banner under the nav when a newer release than this binary
// exists on GitHub. Only a real (non-dev) build that the update check found to
// be behind shows it.
let updateBannerDismissed = false;
function renderUpdateBanner() {
  const banner = $("#updateBanner");
  if (!banner) return;
  const v = state.version || {};
  const show = Boolean(v.updateAvailable) && !updateBannerDismissed;
  banner.classList.toggle("hidden", !show);
  if (show) {
    banner.querySelector(".update-banner-text").textContent =
      `A newer build is available — v${v.latest} (you're on v${v.version}). Download it from the latest GitHub release and restart.`;
  }
}

function renderViewHeader() {
  if (currentView === "monitor") {
    $("#viewTitle").textContent = "Server information";
    $("#viewMeta").textContent = "Live bandwidth, active clients, and server usage.";
  } else if (currentView === "grid") {
    const filtered = streamsGridProviderId && state.providers.find((p) => p.id === streamsGridProviderId);
    $("#viewTitle").textContent = filtered ? filtered.name : "All streams";
    $("#viewMeta").textContent = filtered
      ? `${filtered.streams.length} configured stream${filtered.streams.length === 1 ? "" : "s"}`
      : "Every stream across every provider, at a glance.";
  } else if (currentView === "logs") {
    $("#viewTitle").textContent = "Logs";
    $("#viewMeta").textContent = "Manifest fetches, segment downloads, and proxy activity.";
  } else if (currentView === "keys") {
    $("#viewTitle").textContent = "API Keys";
    $("#viewMeta").textContent = "Generate keys to require authentication for stream playback.";
  } else if (currentView === "settings") {
    $("#viewTitle").textContent = "Settings";
    $("#viewMeta").textContent = "Server port and admin accounts.";
  } else {
    $("#viewTitle").textContent = "Providers";
    $("#viewMeta").textContent = "Click a provider to see and manage its streams.";
  }
}

let logsPollTimer = null;

function switchView(view) {
  const apply = () => {
    currentView = view;
    document.querySelectorAll(".nav-btn").forEach((button) => button.classList.toggle("active", button.dataset.view === view));
    $("#providersView").classList.toggle("hidden", view !== "providers");
    $("#streamsGridView").classList.toggle("hidden", view !== "grid");
    $("#monitorView").classList.toggle("hidden", view !== "monitor");
    $("#logsView").classList.toggle("hidden", view !== "logs");
    $("#keysView").classList.toggle("hidden", view !== "keys");
    $("#settingsView").classList.toggle("hidden", view !== "settings");
    renderViewHeader();
    if (view === "grid") renderStreamsGrid();
    if (view === "monitor") repaintMonitorIfActive();
    if (view === "settings") loadSettingsView();
    // Logs auto-refresh only while the tab is actually open, same reasoning
    // as the existing "no overhead when closed" comment on loadLogs itself.
    clearInterval(logsPollTimer);
    logsPollTimer = null;
    if (view === "logs") {
      loadLogs();
      logsPollTimer = setInterval(loadLogs, 2000);
    }
  };
  if (document.startViewTransition) {
    document.startViewTransition(apply);
  } else {
    apply();
  }
}

let providerSearchQuery = "";

// The Providers tab is a picker, nothing else — it only ever shows provider
// cards. Streams (for all providers or filtered to one) live entirely in the
// All Streams tab; clicking a card here just jumps there pre-filtered.
function renderProviders() {
  const container = $("#providerChips");
  container.innerHTML = "";
  const query = providerSearchQuery.trim().toLowerCase();
  const providers = query ? state.providers.filter((p) => p.name.toLowerCase().includes(query)) : state.providers;
  if (!state.providers.length) {
    container.className = "provider-grid empty-state";
    container.textContent = "No providers yet — create one to get started.";
    return;
  }
  if (!providers.length) {
    container.className = "provider-grid empty-state";
    container.textContent = "No providers match your search.";
    return;
  }
  container.className = "provider-grid";
  for (const provider of providers) {
    const runningCount = provider.streams.filter((s) => s.running).length;
    const card = document.createElement("div");
    card.className = "provider-grid-card";
    card.innerHTML = `
      <div class="provider-card-corner">
        <button type="button" class="icon-btn ghost" data-action="playlist" title="Export an M3U playlist of this provider's streams"><span data-icon="logs"></span></button>
        <button type="button" class="icon-btn ghost" data-action="export" title="Export this provider (settings, accounts, streams, and its script file) as one file"><span data-icon="download"></span></button>
        <button type="button" class="icon-btn ghost" data-action="settings" title="Provider settings"><span data-icon="settings"></span></button>
        <button type="button" class="icon-btn ghost" data-action="delete" title="Delete this provider"><span data-icon="trash"></span></button>
      </div>
      <span class="provider-logo-lg">${provider.logo ? `<img src="${escapeAttr(provider.logo)}" alt="">` : escapeHtml(provider.name.slice(0, 1).toUpperCase())}</span>
      <div class="provider-card-name">${escapeHtml(provider.name)}</div>
      <div class="provider-card-meta">${provider.streams.length} stream${provider.streams.length === 1 ? "" : "s"}${runningCount ? ` · ${runningCount} running` : ""}</div>
    `;
    card.addEventListener("click", () => {
      selectedProviderId = provider.id;
      streamsGridProviderId = provider.id;
      switchView("grid");
    });
    card.querySelector('[data-action="settings"]').addEventListener("click", (event) => {
      event.stopPropagation();
      selectedProviderId = provider.id;
      openProviderSettingsDialog();
    });
    card.querySelector('[data-action="delete"]').addEventListener("click", async (event) => {
      event.stopPropagation();
      selectedProviderId = provider.id;
      await deleteSelectedProvider();
    });
    card.querySelector('[data-action="export"]').addEventListener("click", (event) => {
      event.stopPropagation();
      exportProvider(provider.id, provider.name);
    });
    card.querySelector('[data-action="playlist"]').addEventListener("click", (event) => {
      event.stopPropagation();
      exportProviderPlaylist(provider.id, provider.name);
    });
    applyIcons(card);
    container.appendChild(card);
  }
}

// MARK: - Provider export / import
//
// Export downloads straight from GET /api/providers/:id/export (the browser
// handles the Content-Disposition attachment on its own — no client-side
// Blob/anchor dance needed, just navigating there). Import reads the picked
// file client-side and POSTs its raw contents, since the endpoint expects
// exactly what export produced.

function exportProvider(providerId, providerName) {
  const link = document.createElement("a");
  link.href = `/api/providers/${providerId}/export`;
  link.download = `${providerName}.restreamair-provider.json`;
  document.body.appendChild(link);
  link.click();
  link.remove();
}

function exportProviderPlaylist(providerId, providerName) {
  const link = document.createElement("a");
  link.href = `/api/providers/${providerId}/playlist.m3u8`;
  link.download = `${providerName}.m3u8`;
  document.body.appendChild(link);
  link.click();
  link.remove();
}

async function importProviderFromFile(file) {
  const text = await file.text();
  try {
    state = await request("/api/providers/import", { method: "POST", body: text });
  } catch (error) {
    alert(`Couldn't import provider: ${error.message || error}`);
    return;
  }
  render();
}

// MARK: - All streams grid

let streamsGridProviderId = "";
let streamsGridTypeFilter = "";
let streamsGridSearchQuery = "";
let streamsGridRunningOnly = false;

// The currently-visible stream rows after all filters (provider, type, search,
// running-only). Shared by the grid render and the bulk actions so "Start all"
// etc. act on exactly what's on screen.
function filteredStreamRows() {
  const query = streamsGridSearchQuery.trim().toLowerCase();
  return state.providers.flatMap((provider) => provider.streams.map((stream) => ({ provider, stream })))
    .filter(({ provider, stream }) => {
      if (streamsGridProviderId && provider.id !== streamsGridProviderId) return false;
      // "manual" means a hand-created stream — sourceType is only ever set by a
      // channels/events import, so an empty sourceType is exactly that.
      if (streamsGridTypeFilter && (stream.sourceType || "manual") !== streamsGridTypeFilter) return false;
      if (streamsGridRunningOnly && !stream.running) return false;
      if (query && !stream.name.toLowerCase().includes(query) && !stream.url.toLowerCase().includes(query)) return false;
      return true;
    });
}

// Enable/disable the bulk buttons to match what the filtered set can do, and
// reflect the running-only toggle's state.
function updateBulkActionBar(rows) {
  const anyStopped = rows.some(({ stream }) => !stream.running);
  const anyRunning = rows.some(({ stream }) => stream.running);
  const startAll = $("#gridStartAllBtn");
  const stopAll = $("#gridStopAllBtn");
  const deleteAll = $("#gridDeleteAllBtn");
  const runningOnly = $("#gridRunningOnlyBtn");
  if (startAll) startAll.disabled = !anyStopped;
  if (stopAll) stopAll.disabled = !anyRunning;
  if (deleteAll) deleteAll.disabled = rows.length === 0;
  if (runningOnly) runningOnly.classList.toggle("active", streamsGridRunningOnly);
}

// Run one action over every filtered stream, then re-render once. Bulk calls go
// sequentially so the shared state.json write path isn't racing itself.
async function bulkStreamAction(kind) {
  const rows = filteredStreamRows();
  if (kind === "delete") {
    if (!rows.length) return;
    if (!confirm(`Delete ${rows.length} stream${rows.length === 1 ? "" : "s"}? This stops them and removes their configuration. This cannot be undone.`)) return;
  }
  let count = 0;
  for (const { stream } of rows) {
    try {
      if (kind === "start" && !stream.running) { state = await request(`/api/streams/${stream.id}/start`, { method: "POST", body: "{}" }); count++; }
      else if (kind === "stop" && stream.running) { state = await request(`/api/streams/${stream.id}/stop`, { method: "POST", body: "{}" }); count++; }
      else if (kind === "delete") { state = await request(`/api/streams/${stream.id}`, { method: "DELETE" }); count++; }
    } catch (error) {
      $("#streamsGridImportStatus").classList.remove("hidden");
      $("#streamsGridImportStatus").textContent = `${kind} failed on "${stream.name}": ${error.message || error}`;
    }
  }
  if (kind === "delete") { selectedStreamId = null; lastEditorStreamId = null; }
  render();
}

function renderStreamsGrid() {
  const container = $("#streamsGrid");
  const providerFilter = $("#streamsGridProviderFilter");
  const currentFilterValue = streamsGridProviderId;
  providerFilter.innerHTML = `<option value="">All providers</option>${state.providers.map((p) => `<option value="${escapeAttr(p.id)}" ${p.id === currentFilterValue ? "selected" : ""}>${escapeHtml(p.name)}</option>`).join("")}`;
  // Adding a stream needs a provider to attach it to — only disabled when
  // there are none at all (create one from the Providers tab first).
  $("#gridNewStreamBtn").disabled = !state.providers.length;
  // Import needs a specific script provider, not "whichever's first" — only
  // shown once the grid is filtered down to one that actually has a script
  // configured, unlike New Stream's more permissive fallback.
  const filteredProvider = state.providers.find((p) => p.id === streamsGridProviderId);
  const showImport = Boolean(filteredProvider?.scriptPath);
  $("#gridImportChannelsBtn").classList.toggle("hidden", !showImport);
  $("#gridImportEventsBtn").classList.toggle("hidden", !showImport);
  // Unlike import, settings make sense for any filtered provider — script
  // or not.
  $("#gridProviderSettingsBtn").classList.toggle("hidden", !filteredProvider);

  const allRows = state.providers.flatMap((provider) => provider.streams.map((stream) => ({ provider, stream })));
  const rows = filteredStreamRows();

  // Bulk actions and the running-only toggle operate on this same filtered set.
  updateBulkActionBar(rows);

  $("#streamsGridCount").textContent = allRows.length
    ? `${rows.length} of ${allRows.length} stream${allRows.length === 1 ? "" : "s"}`
    : "";
  if (!allRows.length) {
    container.className = "stream-grid empty-state";
    container.textContent = "No streams yet — create a provider and add one.";
    return;
  }
  if (!rows.length) {
    container.className = "stream-grid empty-state";
    container.textContent = "No streams match your filters.";
    return;
  }
  container.className = "stream-grid";
  container.innerHTML = "";
  for (const { provider, stream } of rows) {
    const bandwidth = stream.bandwidth || {};
    const inputBandwidth = stream.inputBandwidth || {};
    const card = document.createElement("div");
    card.className = "stream-grid-card";
    const eventWindow = stream.sourceType === "event" && stream.scriptStart
      ? `${new Date(stream.scriptStart * 1000).toLocaleString()}${stream.scriptEnd ? ` – ${new Date(stream.scriptEnd * 1000).toLocaleTimeString()}` : ""}`
      : "";
    card.innerHTML = `
      <div class="provider-tag">${escapeHtml(provider.name)}${stream.sourceType ? ` · <span class="source-type-tag">${stream.sourceType}</span>` : ""}</div>
      <div class="stream-logo-hero">
        <span class="provider-logo">${stream.logo ? `<img src="${escapeAttr(stream.logo)}" alt="">` : escapeHtml(stream.name.slice(0, 1).toUpperCase())}</span>
      </div>
      <div class="stream-title">
        <span class="stream-title-name">
          <strong>${escapeHtml(stream.name)}</strong>
        </span>
        <span class="badge ${stream.running ? "running" : ""}">${escapeHtml(stream.status || "stopped")}</span>
      </div>
      <div class="stream-url">${escapeHtml(stream.kind.toUpperCase())} · ${escapeHtml(stream.url)}</div>
      ${eventWindow ? `<div class="stream-url">${escapeHtml(eventWindow)}${stream.autostart ? " · autostart" : ""}</div>` : ""}
      <div class="stream-stats">
        <span>${stream.activeClients ?? 0} active</span>
        <span>↓ ${formatBytesPerSecond(inputBandwidth.bytesPerSecond)}</span>
        <span>↑ ${formatBytesPerSecond(bandwidth.bytesPerSecond)}</span>
      </div>
      <div class="actions">
        <button type="button" class="ghost mini-icon-btn" data-action="copyurl" title="Copy the HLS (m3u8) output URL"><span data-icon="copy"></span></button>
        <button type="button" class="ghost mini-icon-btn" data-action="bigplayer" title="Open big player"><span data-icon="expand"></span></button>
        <button type="button" class="ghost mini-icon-btn" data-action="play" title="Play in Picture-in-Picture"><span data-icon="pip"></span></button>
        <button type="button" class="ghost mini-icon-btn" data-action="logs" title="View logs for this stream"><span data-icon="logs"></span></button>
        <button type="button" class="${stream.running ? "danger" : "success"}" data-action="toggle"><span data-icon="${stream.running ? "stop" : "play"}"></span>${stream.running ? "Stop" : "Start"}</button>
        <button type="button" class="icon-btn ghost" data-action="delete" title="Delete stream"><span data-icon="trash"></span></button>
      </div>
    `;
    card.addEventListener("click", () => {
      selectedProviderId = provider.id;
      selectedStreamId = stream.id;
      renderEditor();
      openStreamEditorDialog();
    });
    card.querySelector('[data-action="copyurl"]').addEventListener("click", async (event) => {
      event.stopPropagation();
      await copyStreamOutputUrl(stream, event.currentTarget);
    });
    card.querySelector('[data-action="bigplayer"]').addEventListener("click", (event) => {
      event.stopPropagation();
      openBigPlayer(provider, stream);
    });
    card.querySelector('[data-action="play"]').addEventListener("click", async (event) => {
      event.stopPropagation();
      await playInPictureInPicture(stream);
    });
    card.querySelector('[data-action="logs"]').addEventListener("click", (event) => {
      event.stopPropagation();
      openStreamLogs(stream.id);
    });
    card.querySelector('[data-action="toggle"]').addEventListener("click", async (event) => {
      event.stopPropagation();
      await toggleStreamRun(stream.id, stream.running);
    });
    card.querySelector('[data-action="delete"]').addEventListener("click", async (event) => {
      event.stopPropagation();
      await deleteStreamId(stream.id, stream.name);
    });
    applyIcons(card);
    container.appendChild(card);
  }
}


// Copy the stream's HLS (m3u8) output URL — the panel always serves playback
// as `/play/<id>/index.m3u8`, so this is the one URL to hand a player. Falls
// back to building it from the current origin if the server hasn't populated
// playUrl yet (e.g. the stream has never been started).
async function copyStreamOutputUrl(stream, button) {
  const url = stream.playUrl || `${location.origin}/play/${stream.id}/index.m3u8`;
  try {
    await navigator.clipboard.writeText(url);
    if (button) flashButtonCopied(button);
  } catch (_) {
    window.prompt("Copy the output URL:", url);
  }
}

// Briefly swap an icon button to a check mark so a copy registers visually.
function flashButtonCopied(button) {
  const original = button.innerHTML;
  button.innerHTML = '<span data-icon="check"></span>';
  applyIcons(button);
  button.classList.add("copied");
  setTimeout(() => {
    button.innerHTML = original;
    applyIcons(button);
    button.classList.remove("copied");
  }, 1200);
}

// CDN mirrors: a progressive list rather than a raw textarea — each mirror is
// its own input row you add/remove, so an empty stream shows just an "Add CDN
// mirror" button instead of a big empty box.
function renderCdnMirrors(urls) {
  const list = $("#cdnMirrorList");
  if (!list) return;
  list.innerHTML = "";
  for (const url of urls) addCdnMirrorRow(url);
}

function addCdnMirrorRow(value = "") {
  const list = $("#cdnMirrorList");
  if (!list) return;
  const row = document.createElement("div");
  row.className = "cdn-mirror-row";
  const input = document.createElement("input");
  input.type = "url";
  input.className = "cdn-mirror-input";
  input.placeholder = "https://cdn2.example.com/index.mpd";
  input.value = value;
  const remove = document.createElement("button");
  remove.type = "button";
  remove.className = "icon-btn ghost";
  remove.title = "Remove this mirror";
  remove.innerHTML = '<span data-icon="trash"></span>';
  remove.addEventListener("click", () => row.remove());
  row.append(input, remove);
  list.appendChild(row);
  applyIcons(row);
  if (!value) input.focus();
}

function collectCdnMirrors() {
  return Array.from(document.querySelectorAll("#cdnMirrorList .cdn-mirror-input"))
    .map((input) => input.value.trim())
    .filter(Boolean);
}

$("#addCdnMirrorBtn")?.addEventListener("click", () => addCdnMirrorRow(""));

function updateKindVisibility() {
  const kind = $("#streamForm").elements.kind.value;
  document.querySelectorAll("[data-kind-group]").forEach((group) => {
    group.classList.toggle("hidden", group.dataset.kindGroup !== kind);
  });
}

function updateRunToggleButton(stream) {
  const button = $("#toggleRunBtn");
  const running = Boolean(stream?.running);
  button.className = running ? "danger" : "success";
  button.innerHTML = `<span data-icon="${running ? "stop" : "play"}"></span>${running ? "Stop" : "Start"}`;
  applyIcons(button);
}

function renderEditor() {
  // Fetches once (memoized) regardless of which entry point opened the
  // editor (New Stream, Open big player, ...) — a narrower trigger missed
  // some paths and left ffmpegStatus stuck at null, which the ffmpeg-gated
  // option-disabling logic below treats as "unavailable" forever.
  fetchFfmpegStatus();
  const stream = selectedStream();
  const form = $("#streamForm");
  const disabled = !selectedProvider();
  for (const el of form.elements) el.disabled = disabled;
  $("#toggleRunBtn").disabled = !stream;
  $("#deleteStreamBtn").disabled = !stream;
  updateRunToggleButton(stream);

  if (!stream) {
    lastEditorStreamId = null;
    // If the user is mid-way through filling out a brand-new (unsaved)
    // stream, the periodic background refresh() must not wipe their draft
    // out from under them just because no stream is "selected" yet.
    if (!editingNewStream) {
      form.reset();
      form.elements.playlistSegments.value = 6;
      form.elements.keepSegments.value = 10;
      form.elements.downloadAhead.value = 8;
      form.elements.pollInterval.value = 2;
      $("#playLink").value = "";
      setLinkField($("#directLink"), {});
      $("#statusBox").textContent = "Select or create a stream.";
      decryptionKeysManuallyShown = false;
      clearRepresentationPicker();
      updateKindVisibility();
    }
    return;
  }

  editingNewStream = false;

  // Read-only live status can always reflect the latest server state.
  $("#playLink").value = stream.playUrl || "";
  setLinkField($("#directLink"), stream.directStreamUrls || {});
  $("#statusBox").textContent = JSON.stringify({
    status: stream.status,
    running: stream.running,
    activeClients: stream.activeClients,
    playUrl: stream.playUrl,
    directUrl: stream.directUrl,
    directStreamUrls: stream.directStreamUrls || {},
    lastError: stream.lastError,
  }, null, 2);

  // Only (re)populate the editable fields + representation picker when the
  // selected stream actually changes. Otherwise the periodic background
  // refresh() (every 4s) would wipe out in-progress edits — including
  // freshly detected representations that haven't been saved yet.
  if (stream.id === lastEditorStreamId) {
    renderRepresentationPicker();
    updateKindVisibility();
    updateDecryptionKeysVisibility();
    updatePipelineFieldVisibility();
    return;
  }
  lastEditorStreamId = stream.id;
  decryptionKeysManuallyShown = false;

  form.elements.id.value = stream.id;
  form.elements.name.value = stream.name || "";
  form.elements.kind.value = stream.kind || "mpd";
  form.elements.url.value = stream.url || "";
  // Seed the auto-detect dedupe guard so simply clicking into (and back out
  // of) an unmodified Source URL field doesn't silently re-trigger a probe —
  // that would reset selectedRepIds and replace whatever was picked/saved
  // with just the highest-bandwidth default.
  lastAutoDetectedUrl = stream.url || "";
  form.elements.logo.value = stream.logo || "";
  renderCdnMirrors(stream.cdnUrls || []);
  form.elements.representation.value = stream.representation || "";
  form.elements.period.value = stream.period || "";
  form.elements.proxy.value = stream.proxy || "";
  form.elements.playlistSegments.value = stream.playlistSegments || 6;
  form.elements.keepSegments.value = stream.keepSegments || 10;
  form.elements.downloadAhead.value = stream.downloadAhead || 8;
  form.elements.pollInterval.value = stream.pollInterval || 2;
  form.elements.audioDelayMs.value = stream.audioDelayMs || 0;
  form.elements.forceOffline.checked = Boolean(stream.forceOffline);
  form.elements.manifestHeaders.value = stream.manifestHeaders || "";
  form.elements.mediaHeaders.value = stream.mediaHeaders || "";
  form.elements.hlsKeyHeaders.value = stream.hlsKeyHeaders || "";
  form.elements.decryptionKeys.value = stream.decryptionKeys || "";
  form.elements.hlsKey.value = stream.hlsKey || "";
  form.elements.hlsIV.value = stream.hlsIV || "";
  form.elements.inputMode.value = stream.inputMode || "internal";
  form.elements.outputMode.value = stream.outputMode || "hls";
  form.elements.outputTarget.value = stream.outputTarget || "";
  updatePipelineFieldVisibility();

  selectedRepIds = new Set(stream.representations || []);
  probeResult = {
    kind: stream.kind,
    representations: Object.entries(stream.representationMeta || {}).map(([id, meta]) => ({ id, ...meta })),
    protection: {},
  };
  renderRepresentationPicker();
  updateKindVisibility();
  updateDecryptionKeysVisibility();
  // Re-enumerate the source's current qualities so the picker always shows the
  // full list (with the saved selection checked), even for streams saved
  // before all-quality metadata was persisted, or when the source's available
  // renditions have changed. Non-destructive: on any failure the saved picker
  // stays as-is.
  enlistQualitiesForEditor(stream);
}

// Background re-probe for the editor: merges the freshly-detected full
// representation list with whatever was saved, keeping the saved selection.
async function enlistQualitiesForEditor(stream) {
  const url = (stream.url || "").trim();
  if (!url || !/^https?:\/\//i.test(url)) return;
  const streamId = stream.id;
  try {
    const result = await request("/api/probe", {
      method: "POST",
      body: JSON.stringify({ url, proxy: stream.proxy || "", headers: stream.manifestHeaders || "" }),
    });
    // The user may have switched to another stream (or started editing the
    // URL, which triggers its own detect) while the probe was in flight.
    if (lastEditorStreamId !== streamId || detecting) return;
    const detected = result.representations || [];
    const byId = new Map(detected.map((rep) => [rep.id, rep]));
    // Keep any saved representation the probe didn't return (e.g. a selected
    // rendition the source dropped) so the selection isn't silently lost.
    for (const rep of probeResult?.representations || []) {
      if (!byId.has(rep.id)) byId.set(rep.id, rep);
    }
    probeResult = { ...result, representations: Array.from(byId.values()) };
    renderRepresentationPicker();
    updateDecryptionKeysVisibility();
  } catch (_) {
    // Source unreachable / needs headers not set yet — leave the saved picker.
  }
}

function streamPayload() {
  const form = $("#streamForm");
  const representationMeta = {};
  if (probeResult) {
    // Persist metadata for *every* detected representation, not just the
    // selected ones. Storing only the selected set meant that reopening a
    // saved stream rebuilt the picker from that subset, so the qualities you
    // didn't pick vanished and could never be re-selected without a manual
    // re-detect. Keeping them all lets the picker show the full quality list
    // with the saved selection checked; `representations` (below) still
    // records which ones actually get streamed.
    for (const rep of probeResult.representations || []) {
      representationMeta[rep.id] = {
        type: rep.type, bandwidth: rep.bandwidth ?? null, width: rep.width ?? null,
        height: rep.height ?? null, codecs: rep.codecs ?? null, language: rep.language ?? null,
      };
    }
  }
  return {
    name: form.elements.name.value,
    kind: form.elements.kind.value,
    url: form.elements.url.value,
    logo: form.elements.logo.value,
    cdnUrls: collectCdnMirrors(),
    representation: form.elements.representation.value,
    representations: Array.from(selectedRepIds),
    representationMeta,
    period: form.elements.period.value,
    proxy: form.elements.proxy.value,
    playlistSegments: Number(form.elements.playlistSegments.value),
    keepSegments: Number(form.elements.keepSegments.value),
    downloadAhead: Number(form.elements.downloadAhead.value),
    pollInterval: Number(form.elements.pollInterval.value),
    audioDelayMs: Number(form.elements.audioDelayMs.value) || 0,
    forceOffline: form.elements.forceOffline.checked,
    manifestHeaders: form.elements.manifestHeaders.value,
    mediaHeaders: form.elements.mediaHeaders.value,
    hlsKeyHeaders: form.elements.hlsKeyHeaders.value,
    decryptionKeys: form.elements.decryptionKeys.value,
    hlsKey: form.elements.hlsKey.value,
    hlsIV: form.elements.hlsIV.value,
    inputMode: form.elements.inputMode.value,
    outputMode: form.elements.outputMode.value,
    outputTarget: form.elements.outputTarget.value,
  };
}

async function saveStream(event) {
  event.preventDefault();
  const provider = selectedProvider();
  if (!provider) return;
  const id = $("#streamForm").elements.id.value;
  const isNewStream = !id;
  // A rejected save (e.g. the server 400s on an empty/invalid URL) used to
  // fail silently here — the dialog just sat there looking like nothing had
  // happened, with no indication anything went wrong. Surface it instead of
  // swallowing it, and leave the dialog open so the fields can be fixed.
  try {
    if (id) {
      state = await request(`/api/streams/${id}`, { method: "PUT", body: JSON.stringify(streamPayload()) });
    } else {
      state = await request(`/api/providers/${provider.id}/streams`, { method: "POST", body: JSON.stringify(streamPayload()) });
      const updated = selectedProvider();
      selectedStreamId = updated?.streams.at(-1)?.id || null;
    }
  } catch (error) {
    alert(`Couldn't save stream: ${error.message || error}`);
    return;
  }
  render();
  // Creating a brand-new stream closes the dialog once it exists; editing an
  // existing one leaves it open so Start/Stop/Delete stay reachable.
  if (isNewStream) closeStreamEditorDialog();
}

async function toggleStreamRun(id, running) {
  state = await request(`/api/streams/${id}/${running ? "stop" : "start"}`, { method: "POST", body: "{}" });
  render();
}

async function toggleSelectedRun() {
  const stream = selectedStream();
  if (!stream) return;
  await toggleStreamRun(stream.id, stream.running);
}

async function deleteStreamId(id, name) {
  if (!confirm(`Delete stream "${name}"? This stops it and removes its configuration.`)) return;
  state = await request(`/api/streams/${id}`, { method: "DELETE" });
  if (selectedStreamId === id) selectedStreamId = selectedProvider()?.streams[0]?.id || null;
  lastEditorStreamId = null;
  render();
}

async function deleteSelectedStream() {
  const stream = selectedStream();
  if (!stream) return;
  await deleteStreamId(stream.id, stream.name);
}

function newStream() {
  selectedStreamId = null;
  editingNewStream = true;
  lastAutoDetectedUrl = "";
  $("#streamForm").reset();
  // form.reset() doesn't actually clear this: <input type="hidden"> uses
  // "value mode: default", where the .value IDL property *is* the content
  // attribute rather than a separate dirty/default pair like text inputs
  // have — so once renderEditor() has ever done elements.id.value = stream.id
  // for an existing stream, reset() has nothing to restore to and the field
  // is stuck on that id for the rest of the session. Every later "New
  // Stream" would silently reuse it and PUT-overwrite that old stream
  // instead of POSTing a new one — never appearing as an addition.
  $("#streamForm").elements.id.value = "";
  $("#streamForm").elements.kind.value = "mpd";
  $("#streamForm").elements.playlistSegments.value = 6;
  $("#streamForm").elements.keepSegments.value = 10;
  $("#streamForm").elements.downloadAhead.value = 8;
  $("#streamForm").elements.pollInterval.value = 2;
  $("#playLink").value = "";
  setLinkField($("#directLink"), {});
  renderCdnMirrors([]);
  $("#statusBox").textContent = "New stream.";
  clearRepresentationPicker();
  updateKindVisibility();
  updatePipelineFieldVisibility();
}

// MARK: - Stream editor dialog
//
// Adding or editing a stream used to happen inline in a split streams-list/
// editor view. It's now an in-page modal <dialog> instead — a real popup
// window is one more window to manage and gets blocked/hidden by browsers
// too easily, so this stays in the same window/tab.

function openStreamEditorDialog(createNew) {
  if (createNew) newStream();
  $("#streamEditorTitle").textContent = selectedStream()?.name || "New stream";
  switchTab("editor");
  $("#streamEditorDialog").showModal();
  $("#streamForm").elements.name.focus();
  fetchFfmpegStatus();
}

function closeStreamEditorDialog() {
  if ($("#streamEditorDialog").open) $("#streamEditorDialog").close();
}

function openNewStreamDialog() {
  if (!selectedProvider()) return;
  openStreamEditorDialog(true);
}

// Jumps straight to the Player tab in the (already reasonably large) modal
// dialog — a bigger view than the floating Picture-in-Picture quick-play,
// without leaving the current grid/list.
function openBigPlayer(provider, stream) {
  selectedProviderId = provider.id;
  selectedStreamId = stream.id;
  renderEditor();
  $("#streamEditorTitle").textContent = stream.name;
  $("#streamEditorDialog").showModal();
  switchTab("player");
}

// Populates one of the Direct link / Download link boxes from a
// { repId: url } map as real, individually clickable <a> elements — one row
// per representation — instead of a read-only text field the user has to
// select-all and copy out of.
function setLinkField(container, urls, options = {}) {
  const keys = Object.keys(urls);
  container.innerHTML = "";
  container.dataset.full = keys.map((k) => (keys.length === 1 ? urls[k] : `${k}: ${urls[k]}`)).join("\n");
  if (!keys.length) {
    container.classList.add("empty-state");
    container.textContent = options.emptyText || "Start a stream to get a link";
    return;
  }
  container.classList.remove("empty-state");
  for (const key of keys) {
    const row = document.createElement("div");
    row.className = "link-list-row";
    if (keys.length > 1) {
      const label = document.createElement("span");
      label.className = "link-list-label";
      label.textContent = `${key}:`;
      row.appendChild(label);
    }
    const a = document.createElement("a");
    a.href = urls[key];
    a.textContent = urls[key];
    a.target = "_blank";
    a.rel = "noopener";
    if (options.download) a.setAttribute("download", "");
    row.appendChild(a);
    container.appendChild(row);
  }
}

function switchTab(tabName) {
  document.querySelectorAll(".tab").forEach((button) => button.classList.toggle("active", button.dataset.tab === tabName));
  $("#streamForm").classList.toggle("hidden", tabName !== "editor");
  $("#playerPanel").classList.toggle("hidden", tabName !== "player");
  if (tabName === "player") loadPlayer();
}

// Closing the dialog (or navigating away from the player tab) used to leave
// hls.js running and the <video> playing in the background — the <dialog>
// hiding is purely visual, nothing about it pauses media — so the stream
// kept fetching segments and using bandwidth with no player visible at all.
function stopPlayer() {
  if (hls) {
    hls.destroy();
    hls = null;
  }
  const video = $("#video");
  video.pause();
  video.removeAttribute("src");
  video.load();
}

// Shared by the main player and the floating Picture-in-Picture player, so
// both behave identically instead of drifting apart as two inline literals.
//
// The retry policy matters: a stream that was *just* started has a live worker
// per representation that hasn't written its live.m3u8 yet, so the playlists
// 404 for the first few seconds. hls.js's default errorRetry gives up after 2
// attempts (~3s), which lands right inside that window — the player then sits
// there dead until a manual reload, even though the stream comes up fine a
// moment later. Retrying persistently with backoff lets playback start on its
// own as soon as the first segments land, and also rides out a transient blip
// mid-stream rather than tearing the player down.
const HLS_CONFIG = {
  liveSyncDurationCount: 2,
  maxLiveSyncPlaybackRate: 1.3,
  debug: true,
  playlistLoadPolicy: {
    default: {
      maxTimeToFirstByteMs: 10000,
      maxLoadTimeMs: 20000,
      timeoutRetry: { maxNumRetry: 4, retryDelayMs: 500, maxRetryDelayMs: 4000 },
      errorRetry: { maxNumRetry: 8, retryDelayMs: 1000, maxRetryDelayMs: 8000 },
    },
  },
};

function loadPlayer() {
  const stream = selectedStream();
  const video = $("#video");
  stopPlayer();
  $("#playerTrackControls").classList.add("hidden");
  $("#qualitySelect").innerHTML = "";
  $("#audioTrackSelect").innerHTML = "";
  if (!stream?.playUrl) {
    video.removeAttribute("src");
    $("#statusBox").textContent = "Start the stream to get a play link.";
    return;
  }
  // hls.js is preferred whenever it can run: some browsers/embedded webviews
  // report native HLS support via canPlayType but handle multi-track audio
  // (a separate EXT-X-MEDIA playlist, which every multi-quality/multi-audio
  // stream here uses) inconsistently or not at all — video plays, audio
  // silently doesn't. hls.js demuxes both into the same <video> via MSE and
  // gets this right. Native <video src> is only the fallback for engines
  // (iOS Safari, mainly) where hls.js can't run at all (no MSE).
  if (window.Hls && window.Hls.isSupported()) {
    hls = new window.Hls(HLS_CONFIG);
    hls.loadSource(stream.playUrl);
    hls.attachMedia(video);
    hls.on(window.Hls.Events.MANIFEST_PARSED, () => {
      video.play().catch(() => {});
      renderTrackControls(hls);
    });
  } else if (video.canPlayType("application/vnd.apple.mpegurl")) {
    video.src = stream.playUrl;
    video.play().catch(() => {});
  } else {
    $("#statusBox").textContent = "This browser cannot play HLS and hls.js is unavailable.";
  }
}

// Quality/audio-track pickers for the main player — hls.js demuxes every
// #EXT-X-STREAM-INF / #EXT-X-MEDIA:TYPE=AUDIO variant into `hls.levels` /
// `hls.audioTracks`, but doesn't offer any UI for switching between them
// itself. Hidden entirely when there's nothing to pick (single quality,
// single or no audio track).
function renderTrackControls(hlsInstance) {
  const qualitySelect = $("#qualitySelect");
  const audioSelect = $("#audioTrackSelect");
  const levels = hlsInstance.levels || [];
  const audioTracks = hlsInstance.audioTracks || [];

  qualitySelect.innerHTML = "";
  if (levels.length > 1) {
    const autoOption = document.createElement("option");
    autoOption.value = "-1";
    autoOption.textContent = "Auto";
    qualitySelect.appendChild(autoOption);
    levels.forEach((level, index) => {
      const option = document.createElement("option");
      option.value = String(index);
      option.textContent = level.height ? `${level.height}p` : (level.bitrate ? `${Math.round(level.bitrate / 1000)} kbps` : `Level ${index}`);
      qualitySelect.appendChild(option);
    });
    qualitySelect.value = String(hlsInstance.currentLevel);
  }

  audioSelect.innerHTML = "";
  if (audioTracks.length > 1) {
    audioTracks.forEach((track, index) => {
      const option = document.createElement("option");
      option.value = String(index);
      option.textContent = track.name || track.lang || `Track ${index}`;
      audioSelect.appendChild(option);
    });
    audioSelect.value = String(hlsInstance.audioTrack);
  }

  $("#qualitySelectField").classList.toggle("hidden", levels.length <= 1);
  $("#audioTrackSelectField").classList.toggle("hidden", audioTracks.length <= 1);
  $("#playerTrackControls").classList.toggle("hidden", levels.length <= 1 && audioTracks.length <= 1);
}

// Same background-playback leak as stopPlayer() above, but for the PiP
// video — closing the floating PiP window (its own native close button,
// outside our DOM) fires "leavepictureinpicture" with no dialog involved at
// all, so this needed its own listener rather than reusing the dialog's.
function stopPipPlayer() {
  if (pipHls) {
    pipHls.destroy();
    pipHls = null;
  }
  const video = $("#pipVideo");
  video.pause();
  video.removeAttribute("src");
  video.load();
}

// Quick-play from the All Streams grid: loads the stream into a hidden
// <video> and pops it into a floating Picture-in-Picture window, without
// navigating away from the grid. Starts the stream first if it isn't
// running yet.
async function playInPictureInPicture(stream) {
  if (!document.pictureInPictureEnabled) {
    alert("This browser doesn't support Picture-in-Picture.");
    return;
  }
  if (!stream.running) {
    await toggleStreamRun(stream.id, false);
    await new Promise((resolve) => setTimeout(resolve, 1500)); // give the worker a moment to produce the first segment
  }
  const video = $("#pipVideo");
  if (pipHls) {
    pipHls.destroy();
    pipHls = null;
  }
  try {
    if (window.Hls && window.Hls.isSupported()) {
      pipHls = new window.Hls(HLS_CONFIG);
      pipHls.loadSource(stream.playUrl);
      pipHls.attachMedia(video);
    } else if (video.canPlayType("application/vnd.apple.mpegurl")) {
      video.src = stream.playUrl;
    } else {
      alert("This browser cannot play HLS and hls.js is unavailable.");
      return;
    }
    await video.play();
    await video.requestPictureInPicture();
  } catch (error) {
    alert(`Couldn't start playback: ${error.message || error}`);
  }
}

// MARK: - Stream detect / probe

function clearRepresentationPicker() {
  probeResult = null;
  selectedRepIds = new Set();
  $("#representationPicker").classList.add("hidden");
  $("#representationPicker").innerHTML = "";
  $("#detectStatus").textContent = "";
  updateDecryptionKeysVisibility();
}

// The CENC key box only makes sense once we know the manifest is encrypted —
// stay hidden until a probe finds protection, the field already has
// saved/typed key content in it, or the user asks for it via the "Add
// decryption keys" button (detection isn't reliable for every CENC
// signaling shape, so there has to be a manual escape hatch).
function updateDecryptionKeysVisibility() {
  const form = $("#streamForm");
  const group = $("#decryptionKeysGroup");
  const button = $("#showDecryptionKeysBtn");
  if (!form || !group) return;
  const hasProtection = Boolean(probeResult && Object.keys(probeResult.protection || {}).length);
  const hasExistingKeys = form.elements.decryptionKeys.value.trim().length > 0;
  const shouldShow = hasProtection || hasExistingKeys || decryptionKeysManuallyShown;
  group.classList.toggle("hidden", !shouldShow);
  if (button) button.classList.toggle("hidden", shouldShow);
}

// MARK: - Input/output pipeline
//
// Input=internal runs the built-in Swift downloader/mixer. The ffmpeg input
// modes (ffmpegResident / ffmpegTsHls / ffmpegMultiTsHls / ffmpegFmp4Hls)
// spawn a resident ffmpeg that reads the source directly (decrypting CENC
// clearkey itself) and remuxes with -c copy to the selected Output — HLS
// served from the panel, or an SRT/UDP/TCP push (Output target). ffmpeg must
// be installed for any non-internal input or non-hls output.

let ffmpegStatus = null;
let ffmpegStatusPromise = null;

function fetchFfmpegStatus() {
  if (!ffmpegStatusPromise) {
    ffmpegStatusPromise = request("/api/ffmpeg-status")
      .then((result) => { ffmpegStatus = result; updatePipelineFieldVisibility(); return result; })
      .catch(() => { ffmpegStatus = { available: false, installCommand: "", canAutoInstall: false }; });
  }
  return ffmpegStatusPromise;
}

const OUTPUT_TARGET_HINTS = {
  srtServer: "Port to listen on, e.g. 9000 — other tools connect to this panel to pull the stream via SRT.",
  udpSrt: "Destination to push to, e.g. udp://host:1234 or srt://host:1234 — this panel connects out.",
  custom: "Extra ffmpeg output arguments or a pipe command, passed through as-is.",
};

function updatePipelineFieldVisibility() {
  const form = $("#streamForm");
  if (!form) return;
  const available = Boolean(ffmpegStatus?.available);
  document.querySelectorAll('#streamForm option[data-needs-ffmpeg]').forEach((option) => {
    option.disabled = !available;
  });
  const outputMode = form.elements.outputMode.value;
  const targetField = $("#outputTargetField");
  targetField.classList.toggle("hidden", outputMode === "hls");
  $("#outputTargetHint").textContent = OUTPUT_TARGET_HINTS[outputMode] || "";

  const needsFfmpeg = form.elements.inputMode.value !== "internal" || outputMode !== "hls";
  const notice = $("#ffmpegNotice");
  const installRow = $("#ffmpegInstallRow");
  if (!ffmpegStatus || available) {
    notice.classList.add("hidden");
    installRow.classList.add("hidden");
    return;
  }
  notice.classList.remove("hidden");
  notice.textContent = needsFfmpeg
    ? `ffmpeg isn't installed — required for the option${form.elements.inputMode.value !== "internal" && outputMode !== "hls" ? "s" : ""} selected above.`
    : `ffmpeg isn't installed. Only Internal remuxer → HLS/Direct works without it.`;
  installRow.classList.toggle("hidden", !ffmpegStatus.canAutoInstall);
  if (!ffmpegStatus.canAutoInstall && ffmpegStatus.installCommand) {
    notice.textContent += ` Install it yourself: ${ffmpegStatus.installCommand}`;
  }
}

let ffmpegInstallPollTimer = null;

function stopFfmpegInstallPoll() {
  clearInterval(ffmpegInstallPollTimer);
  ffmpegInstallPollTimer = null;
}

async function pollFfmpegInstallOutput() {
  const box = $("#ffmpegInstallOutput");
  try {
    const result = await request(`/api/logs?streamId=${encodeURIComponent("ffmpeg-install")}&limit=300`);
    const lines = (result.entries || []).slice().reverse().map((entry) => entry.message || entry.event).join("\n");
    const text = lines || "(no output yet)";
    if (box.textContent === text) return;
    const wasAtBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 20;
    box.textContent = text;
    if (wasAtBottom) box.scrollTop = box.scrollHeight;
    const latest = (result.entries || [])[0];
    if (latest && latest.event === "installExit") {
      stopFfmpegInstallPoll();
      ffmpegStatusPromise = null;
      await fetchFfmpegStatus();
    }
  } catch (error) {
    box.textContent = `Couldn't load install output: ${error.message || error}`;
  }
}

$("#ffmpegInstallBtn").addEventListener("click", async () => {
  $("#ffmpegInstallOutput").classList.remove("hidden");
  $("#ffmpegInstallOutput").textContent = "Starting…";
  try {
    await request("/api/ffmpeg-install", { method: "POST", body: "{}" });
  } catch (error) {
    $("#ffmpegInstallOutput").textContent = `Couldn't start install: ${error.message || error}`;
    return;
  }
  stopFfmpegInstallPoll();
  pollFfmpegInstallOutput();
  ffmpegInstallPollTimer = setInterval(pollFfmpegInstallOutput, 1000);
});

$("#streamForm").elements.inputMode.addEventListener("change", updatePipelineFieldVisibility);
$("#streamForm").elements.outputMode.addEventListener("change", updatePipelineFieldVisibility);

// Detected KIDs get a zeroed placeholder key line so the user only has to
// paste in the real key rather than also having to copy the KID by hand.
function applyDetectedProtectionPlaceholders(protection) {
  const textarea = $("#streamForm").elements.decryptionKeys;
  const existingLines = textarea.value.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
  const existingKids = new Set(existingLines.map((line) => line.split(":")[0].trim().toLowerCase()));
  const additions = [];
  for (const kids of Object.values(protection || {})) {
    for (const kid of kids) {
      const normalized = kid.toLowerCase();
      if (!existingKids.has(normalized)) {
        existingKids.add(normalized);
        additions.push(`${normalized}:${"0".repeat(32)}`);
      }
    }
  }
  if (additions.length) textarea.value = [...existingLines, ...additions].join("\n");
}

function ensureProbeResult() {
  if (!probeResult) probeResult = { kind: $("#streamForm").elements.kind.value, representations: [], protection: {} };
  return probeResult;
}

function renderRepresentationPicker() {
  const container = $("#representationPicker");
  const result = ensureProbeResult();
  const reps = result.representations || [];
  container.classList.remove("hidden");
  const video = reps.filter((r) => r.type === "video");
  const audio = reps.filter((r) => r.type === "audio");
  const other = reps.filter((r) => r.type !== "video" && r.type !== "audio");
  const protection = result.protection || {};

  const rows = (list, label, groupKey) => {
    if (!list.length) return "";
    const allSelected = list.every((rep) => selectedRepIds.has(rep.id));
    const items = list.map((rep) => {
      const kid = protection[rep.id];
      const details = rep.type === "video"
        ? [rep.width && rep.height ? `${rep.width}x${rep.height}` : null, rep.bandwidth ? `${Math.round(rep.bandwidth / 1000)}kbps` : null, rep.codecs].filter(Boolean).join(" · ")
        : [rep.language, rep.codecs].filter(Boolean).join(" · ");
      return `
        <label class="rep-row">
          <input type="checkbox" data-rep-id="${escapeAttr(rep.id)}" ${selectedRepIds.has(rep.id) ? "checked" : ""}>
          <span>${escapeHtml(rep.id)}${details ? ` — ${escapeHtml(details)}` : ""}</span>
          ${kid ? `<span class="kid-hint">KID ${escapeHtml(kid[0])}</span>` : ""}
        </label>`;
    }).join("");
    return `
      <div class="rep-group-label">
        <span>${label}</span>
        <button type="button" class="ghost mini-select-all" data-group="${groupKey}">${allSelected ? "Unselect all" : "Select all"}</button>
      </div>
      ${items}`;
  };

  container.innerHTML = rows(video, "Video qualities", "video") + rows(audio, "Audio tracks", "audio") + rows(other, "Other", "other") + `
    <div class="manual-rep-row">
      <input id="manualRepId" placeholder="Representation id (not detected? type it)">
      <select id="manualRepType"><option value="video">Video</option><option value="audio">Audio</option></select>
      <button type="button" class="ghost" id="manualRepAdd"><span data-icon="plus"></span>Add</button>
    </div>
  `;
  container.querySelectorAll("input[type=checkbox]").forEach((box) => {
    box.addEventListener("change", () => {
      const id = box.dataset.repId;
      if (box.checked) selectedRepIds.add(id); else selectedRepIds.delete(id);
    });
  });
  const groups = { video, audio, other };
  container.querySelectorAll(".mini-select-all").forEach((button) => {
    button.addEventListener("click", () => {
      const list = groups[button.dataset.group] || [];
      const allSelected = list.every((rep) => selectedRepIds.has(rep.id));
      for (const rep of list) {
        if (allSelected) selectedRepIds.delete(rep.id); else selectedRepIds.add(rep.id);
      }
      renderRepresentationPicker();
    });
  });
  $("#manualRepAdd").addEventListener("click", () => {
    const id = $("#manualRepId").value.trim();
    if (!id) return;
    const type = $("#manualRepType").value;
    const existing = result.representations.find((r) => r.id === id);
    if (existing) {
      existing.type = type;
    } else {
      result.representations.push({ id, type });
    }
    selectedRepIds.add(id);
    renderRepresentationPicker();
  });
  applyIcons(container);
}

let lastAutoDetectedUrl = "";
let autoDetectTimer = null;
let detecting = false;

// The source URL is auto-probed as soon as it looks usable — pasting,
// typing (debounced), or leaving the field all trigger it — so the user
// never has to remember to press Detect themselves.
function autoDetectFromUrlField() {
  const form = $("#streamForm");
  const url = form.elements.url.value.trim();
  if (!url || !/^https?:\/\//i.test(url) || url === lastAutoDetectedUrl || detecting) return;
  lastAutoDetectedUrl = url;
  detectSource();
}

function scheduleAutoDetect() {
  clearTimeout(autoDetectTimer);
  autoDetectTimer = setTimeout(autoDetectFromUrlField, 700);
}

// If the user doesn't pick any qualities, default to the highest-bandwidth
// video representation rather than requiring them to also fill in the
// separate manual "Representation" field just to get something playable.
// Default to restreaming every detected quality and every audio track —
// that gives the master playlist real adaptive-bitrate/multi-audio choice,
// and the player (or the user, via the picker below) picks whichever
// combination it wants. Only kicks in when nothing's been picked yet, so it
// never overrides a deliberate manual selection.
function selectAllRepresentationsByDefault() {
  if (selectedRepIds.size || !probeResult) return false;
  const reps = probeResult.representations || [];
  if (!reps.length) return false;
  for (const rep of reps) selectedRepIds.add(rep.id);
  return true;
}

async function detectSource() {
  const form = $("#streamForm");
  const url = form.elements.url.value.trim();
  if (!url) { $("#detectStatus").textContent = "Enter a source URL first."; return; }
  lastAutoDetectedUrl = url;
  detecting = true;
  $("#detectStatus").textContent = "Detecting…";
  try {
    const result = await request("/api/probe", {
      method: "POST",
      body: JSON.stringify({ url, proxy: form.elements.proxy.value, headers: form.elements.manifestHeaders.value }),
    });
    probeResult = result;
    selectedRepIds = new Set();
    const kind = result.kind === "m3u8" ? "m3u8" : "mpd";
    form.elements.kind.value = kind;
    updateKindVisibility();
    applyDetectedProtectionPlaceholders(result.protection);
    const pickedDefault = selectAllRepresentationsByDefault();
    const count = (result.representations || []).length;
    const kindLabel = kind === "m3u8" ? "M3U8 playlist" : "MPD manifest";
    $("#detectStatus").textContent = count
      ? `Detected ${kindLabel} — found ${count} representation${count === 1 ? "" : "s"}${pickedDefault ? " · all selected by default, uncheck any you don't want" : ""}.`
      : `Detected ${kindLabel} — no representations auto-detected, you can still add one manually below.`;
    renderRepresentationPicker();
    updateDecryptionKeysVisibility();
  } catch (error) {
    ensureProbeResult();
    renderRepresentationPicker();
    updateDecryptionKeysVisibility();
    $("#detectStatus").textContent = `${error.message || error} — you can still add a representation manually below.`;
  } finally {
    detecting = false;
  }
}

// MARK: - Provider settings

function updateSegmentUrlParamsVisibility() {
  const form = $("#providerSettingsForm");
  $("#segmentUrlParamsField").classList.toggle("hidden", form.elements.inheritUrlParams.checked);
}

// MARK: - Script provider (accounts + login/pair)
//
// Accounts are edited as a local draft array (deep-copied from the provider
// on open) rather than native form fields — same reasoning as the
// representation picker's selectedRepIds set: an array-of-objects doesn't
// map onto FormData, so it's assembled into the PUT payload by hand on
// submit instead. New accounts get a client-generated id immediately (rather
// than leaving it blank until the server assigns one) so the "which account
// is active" radio selection has something stable to bind to right away,
// before the account is ever saved.
let scriptAccountsDraft = [];
let activeScriptAccountIdDraft = "";
let scriptOutputPollTimer = null;

function renderScriptAccountsList() {
  const container = $("#scriptAccountsList");
  if (!scriptAccountsDraft.length) {
    container.className = "script-accounts-list empty-state";
    container.textContent = "No accounts yet — add one below before Login/Pair.";
    return;
  }
  container.className = "script-accounts-list";
  container.innerHTML = "";
  for (const account of scriptAccountsDraft) {
    const enabled = account.enabled !== false;
    const row = document.createElement("div");
    row.className = `script-account-row${enabled ? "" : " account-disabled"}`;
    row.innerHTML = `
      <label class="check" title="Use this account for Login/Pair"><input type="radio" name="activeScriptAccount" ${account.id === activeScriptAccountIdDraft ? "checked" : ""}></label>
      <label class="check" title="Enabled — uncheck to keep this account configured but skip it for Login/Pair/Load channels/Load events"><input type="checkbox" class="account-enabled" ${enabled ? "checked" : ""}></label>
      <input class="account-username" value="${escapeAttr(account.username || "")}" placeholder="Username">
      <input class="account-password" type="password" value="${escapeAttr(account.password || "")}" placeholder="Password">
      <button type="button" class="mini-icon-btn ghost" data-action="delete" title="Remove account"><span data-icon="trash"></span></button>
    `;
    row.querySelector('input[type="radio"]').addEventListener("change", () => { activeScriptAccountIdDraft = account.id; });
    row.querySelector(".account-enabled").addEventListener("change", (event) => {
      account.enabled = event.currentTarget.checked;
      renderScriptAccountsList();
    });
    // No separate "name" field anymore — the account's Username doubles as
    // its display label (dropdown, logs, export), same value sent as user=.
    row.querySelector(".account-username").addEventListener("input", (event) => {
      account.username = event.currentTarget.value;
      account.name = event.currentTarget.value;
    });
    row.querySelector(".account-password").addEventListener("input", (event) => { account.password = event.currentTarget.value; });
    row.querySelector('[data-action="delete"]').addEventListener("click", () => {
      scriptAccountsDraft = scriptAccountsDraft.filter((a) => a.id !== account.id);
      if (activeScriptAccountIdDraft === account.id) activeScriptAccountIdDraft = scriptAccountsDraft[0]?.id || "";
      renderScriptAccountsList();
    });
    applyIcons(row);
    container.appendChild(row);
  }
}

function stopScriptOutputPoll() {
  clearInterval(scriptOutputPollTimer);
  scriptOutputPollTimer = null;
}

async function pollScriptOutput(providerId) {
  const box = $("#scriptOutputBox");
  try {
    const result = await request(`/api/logs?streamId=${encodeURIComponent("script:" + providerId)}&limit=300`);
    const lines = (result.entries || []).slice().reverse().map((entry) => entry.message || entry.event).join("\n");
    const text = lines || "(no output yet)";
    // Reassigning textContent on every tick — even to the same text — wipes
    // any active selection and yanks scroll position, making it impossible
    // to select/copy a traceback before the next poll undoes it. Skip the
    // DOM write entirely when nothing changed (the common case once a
    // script has exited and output is just sitting there), and only
    // auto-scroll to the bottom when the user was already there — someone
    // who's scrolled up to read/copy something shouldn't get yanked back
    // down by new output arriving.
    if (box.textContent === text) return;
    const wasAtBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 20;
    box.textContent = text;
    if (wasAtBottom) box.scrollTop = box.scrollHeight;
  } catch (error) {
    box.textContent = `Couldn't load script output: ${error.message || error}`;
  }
}

async function runProviderScript(action) {
  const provider = selectedProvider();
  if (!provider) return;
  if (!$("#providerSettingsForm").elements.scriptPath.value.trim()) { alert("Set a script path first."); return; }
  if (!activeScriptAccountIdDraft) { alert("Add and select an account first."); return; }
  $("#scriptOutputBox").classList.remove("hidden");
  $("#scriptOutputBox").textContent = "Saving…";
  updateScriptOutputToggleLabel();
  try {
    await saveProviderSettings();
    $("#scriptOutputBox").textContent = "Starting…";
    await request(`/api/providers/${provider.id}/script/${action}`, { method: "POST", body: "{}" });
  } catch (error) {
    $("#scriptOutputBox").textContent = `Couldn't start ${action}: ${error.message || error}`;
    return;
  }
  stopScriptOutputPoll();
  pollScriptOutput(provider.id);
  // channels/events write new streams straight to disk in the background
  // (see importScriptEntries) — piggyback a refresh() on the same tick so
  // they show up in the provider/grid views without waiting for the
  // separate 4s background poll or a manual reload.
  const alsoRefreshState = action === "channels" || action === "events";
  scriptOutputPollTimer = setInterval(() => {
    pollScriptOutput(provider.id);
    if (alsoRefreshState) refresh();
  }, 1000);
}

// Import Channels/Events from the All Streams toolbar — same backend
// action as the provider settings dialog's buttons, but there's no dialog
// open here to hold a live-scrolling output box, so this shows just the
// latest status line and stops polling once the script reports it's done
// (rather than running forever until the user navigates away).
let gridImportPollTimer = null;

async function pollGridImportStatus(providerId) {
  const box = $("#streamsGridImportStatus");
  try {
    const result = await request(`/api/logs?streamId=${encodeURIComponent("script:" + providerId)}&limit=5`);
    const latest = (result.entries || [])[0];
    if (!latest) return;
    box.textContent = latest.message || latest.event;
    box.classList.remove("hidden");
    if (latest.event === "scriptExit") {
      clearInterval(gridImportPollTimer);
      gridImportPollTimer = null;
      refresh();
    }
  } catch (error) {
    box.textContent = `Couldn't check import status: ${error.message || error}`;
    box.classList.remove("hidden");
  }
}

async function runProviderScriptForGrid(action) {
  const provider = state.providers.find((p) => p.id === streamsGridProviderId);
  if (!provider) { alert("Filter All Streams to a single provider first."); return; }
  if (!provider.scriptPath) { alert("This provider has no script configured — set one in Provider settings."); return; }
  const box = $("#streamsGridImportStatus");
  box.classList.remove("hidden");
  box.textContent = `Starting ${action}…`;
  try {
    await request(`/api/providers/${provider.id}/script/${action}`, { method: "POST", body: "{}" });
  } catch (error) {
    box.textContent = `Couldn't start ${action}: ${error.message || error}`;
    return;
  }
  clearInterval(gridImportPollTimer);
  pollGridImportStatus(provider.id);
  gridImportPollTimer = setInterval(() => {
    pollGridImportStatus(provider.id);
    refresh();
  }, 1000);
}

function openProviderSettingsDialog() {
  const form = $("#providerSettingsForm");
  const provider = selectedProvider();
  if (!provider) return;
  form.elements.name.value = provider.name || "";
  form.elements.proxy.value = provider.proxy || "";
  form.elements.headers.value = provider.headers || "";
  form.elements.segmentUrlParams.value = provider.segmentUrlParams || "";
  form.elements.inheritUrlParams.checked = Boolean(provider.inheritUrlParams);
  form.elements.scriptPath.value = provider.scriptPath || "";
  form.elements.scriptBind.value = provider.scriptBind || "";
  form.elements.scriptDoh.value = provider.scriptDoh || "";
  form.elements.scriptWorker.value = provider.scriptWorker || "";
  form.elements.accountSelectionMode.value = provider.accountSelectionMode || "fixed";
  scriptAccountsDraft = (provider.scriptAccounts || []).map((a) => ({ ...a }));
  activeScriptAccountIdDraft = provider.activeScriptAccountId || scriptAccountsDraft[0]?.id || "";
  renderScriptAccountsList();
  $("#newScriptAccountUsername").value = "";
  $("#newScriptAccountPassword").value = "";
  stopScriptOutputPoll();
  $("#scriptOutputBox").classList.add("hidden");
  $("#scriptOutputBox").textContent = "";
  updateScriptOutputToggleLabel();
  updateSegmentUrlParamsVisibility();
  $("#providerSettingsDialog").showModal();
}

// The terminal auto-shows whenever Login/Pair/Load channels/Load events is
// clicked (so you see what just happened without an extra click), but can
// also be shown/hidden by hand at any other time — e.g. to peek at the
// previous run's output, or to tuck it away once you're done reading it.
function updateScriptOutputToggleLabel() {
  const hidden = $("#scriptOutputBox").classList.contains("hidden");
  $("#toggleScriptOutputBtn").innerHTML = `<span data-icon="logs"></span>${hidden ? "Show" : "Hide"} terminal`;
  applyIcons($("#toggleScriptOutputBtn"));
}

function closeProviderSettingsDialog() {
  if ($("#providerSettingsDialog").open) $("#providerSettingsDialog").close();
}

async function deleteSelectedProvider() {
  const provider = selectedProvider();
  if (!provider) return;
  const streamCount = provider.streams.length;
  const warning = streamCount
    ? `Delete provider "${provider.name}" and all ${streamCount} of its stream${streamCount === 1 ? "" : "s"}? This stops them and cannot be undone.`
    : `Delete provider "${provider.name}"?`;
  if (!confirm(warning)) return;
  state = await request(`/api/providers/${provider.id}`, { method: "DELETE" });
  selectedProviderId = state.providers[0]?.id || null;
  selectedStreamId = selectedProvider()?.streams[0]?.id || null;
  lastEditorStreamId = null;
  closeProviderSettingsDialog();
  render();
}


// Shared by the form's own submit AND by Login/Pair/Load channels/Load
// events (see runProviderScript) — those all call a backend endpoint that
// reads the *saved* provider from disk, not this open form, so running one
// without saving first used to silently act on stale (often blank) config,
// e.g. "no script configured" right after typing a script path in. One
// save path means that class of bug can't come back by drifting the two
// out of sync again.
async function saveProviderSettings() {
  const provider = selectedProvider();
  if (!provider) throw new Error("No provider selected.");
  const form = $("#providerSettingsForm");
  state = await request(`/api/providers/${provider.id}`, {
    method: "PUT",
    body: JSON.stringify({
      name: form.elements.name.value, logo: provider.logo,
      proxy: form.elements.proxy.value, headers: form.elements.headers.value, segmentUrlParams: form.elements.segmentUrlParams.value,
      inheritUrlParams: form.elements.inheritUrlParams.checked,
      scriptPath: form.elements.scriptPath.value, scriptBind: form.elements.scriptBind.value,
      scriptDoh: form.elements.scriptDoh.value, scriptWorker: form.elements.scriptWorker.value,
      scriptAccounts: scriptAccountsDraft, activeScriptAccountId: activeScriptAccountIdDraft,
      accountSelectionMode: form.elements.accountSelectionMode.value,
    }),
  });
}

$("#providerSettingsForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  try {
    await saveProviderSettings();
  } catch (error) {
    alert(`Couldn't save provider settings: ${error.message || error}`);
    return;
  }
  closeProviderSettingsDialog();
  render();
});

// MARK: - API keys

function renderKeys() {
  const list = $("#keyList");
  const keys = state.apiKeys || [];
  if (!keys.length) {
    list.className = "key-list empty-state";
    list.textContent = "No keys yet — playback is open to anyone on the network.";
    return;
  }
  list.className = "key-list";
  list.innerHTML = "";
  for (const key of keys) {
    const row = document.createElement("div");
    row.className = "key-row";
    row.innerHTML = `
      <div class="key-top">
        <strong>${escapeHtml(key.label)}</strong>
        <button type="button" class="danger" data-key-id="${escapeAttr(key.id)}"><span data-icon="trash"></span>Revoke</button>
      </div>
      <div class="key-value">${escapeHtml(key.key)}</div>
      <div class="key-meta">${key.requests || 0} requests · ${formatBytes(key.bytes || 0)} · last seen ${key.lastSeenAt ? new Date(key.lastSeenAt).toLocaleString() : "never"}</div>
    `;
    row.querySelector("button").addEventListener("click", () => revokeKey(key.id));
    list.appendChild(row);
  }
  applyIcons(list);
}

async function revokeKey(id) {
  state = await request(`/api/keys/${id}`, { method: "DELETE" });
  render();
}

$("#keyForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  state = await request("/api/keys", { method: "POST", body: JSON.stringify({ label: form.elements.label.value }) });
  form.reset();
  render();
});

// MARK: - Logs

let logMode = "normal";
let logLevelFilter = "";
let pendingLogStreamId = null;

// Deep-link from a stream card's Logs button straight into the Logs view,
// pre-filtered to that stream.
function openStreamLogs(streamId) {
  pendingLogStreamId = streamId;
  switchView("logs");
}

// Entries come back newest-first, like every other row's timeline — but
// script output (login/pair/channels/events stdout, one record per line)
// reads as a paragraph, not a timeline. Left as individual rows, a single
// traceback becomes dozens of separately-timestamped, ellipsized lines,
// unreadable and impossible to copy as one block. Bundle consecutive
// scriptOutput entries into one group, restored to chronological order
// (they're reverse order coming in) so the block reads top-to-bottom like
// the terminal output actually printed.
function groupLogEntries(entries) {
  const groups = [];
  let i = 0;
  while (i < entries.length) {
    if (entries[i].event === "scriptOutput") {
      const chunk = [];
      while (i < entries.length && entries[i].event === "scriptOutput") {
        chunk.push(entries[i]);
        i += 1;
      }
      groups.push({ type: "script", entries: chunk.reverse() });
    } else {
      groups.push({ type: "single", entry: entries[i] });
      i += 1;
    }
  }
  return groups;
}

function logGroupHtml(group) {
  if (group.type === "single") return logRowHtml(group.entry);
  const time = new Date(group.entries[0].timestamp).toLocaleTimeString();
  const text = group.entries.map((entry) => entry.raw || entry.message || "").join("\n");
  return `
    <div class="log-row log-row-script">
      <span class="log-time">${time}</span>
      <pre class="log-detail-script">${escapeHtml(text)}</pre>
    </div>
  `;
}

function logRowHtml(entry) {
  const time = new Date(entry.timestamp).toLocaleTimeString();
  if (logMode === "verbose") {
    // entry.raw is JSON for worker log lines but plain text for script
    // output (see logStore.record calls tagged "scriptOutput") — fall back
    // to showing it as-is instead of letting JSON.parse throw and blank out
    // the whole list over one non-JSON line.
    let detail;
    if (entry.raw) {
      try {
        detail = JSON.stringify(JSON.parse(entry.raw), null, 2);
      } catch {
        detail = entry.raw;
      }
    } else {
      detail = JSON.stringify(entry, null, 2);
    }
    return `
      <div class="log-row log-row-verbose ${entry.level === "error" ? "log-error" : ""}">
        <span class="log-time">${time}</span>
        <span class="log-event">${escapeHtml(entry.event)}</span>
        <pre class="log-detail-verbose">${escapeHtml(detail)}</pre>
      </div>
    `;
  }
  return `
    <div class="log-row ${entry.level === "error" ? "log-error" : ""}">
      <span class="log-time">${time}</span>
      <span class="log-event">${escapeHtml(entry.event)}</span>
      <span class="log-detail" title="${escapeAttr([entry.url, entry.message].filter(Boolean).join(" · "))}">${escapeHtml([entry.url, entry.message].filter(Boolean).join(" · "))}</span>
    </div>
  `;
}

async function loadLogs() {
  const list = $("#logList");
  const streamId = pendingLogStreamId !== null ? pendingLogStreamId : $("#logStreamFilter").value;
  pendingLogStreamId = null;
  const options = state.providers.flatMap((p) => p.streams.map((s) => `<option value="${escapeAttr(s.id)}" ${s.id === streamId ? "selected" : ""}>${escapeHtml(s.name)}</option>`));
  // Login/Pair/Load channels/Load events output is recorded under a
  // synthetic "script:<providerId>" id (see PanelServer's script action
  // handlers) rather than a real stream id — list it here too so it's
  // reachable from the main Logs tab, not just the provider dialog's own
  // transient terminal.
  const scriptOptions = state.providers.filter((p) => p.scriptPath).map((p) => `<option value="${escapeAttr("script:" + p.id)}" ${("script:" + p.id) === streamId ? "selected" : ""}>${escapeHtml(p.name)} (script)</option>`);
  $("#logStreamFilter").innerHTML = `<option value="">All streams</option>${options.join("")}${scriptOptions.join("")}`;
  $("#logLevelFilter").value = logLevelFilter;
  try {
    const params = new URLSearchParams({ limit: "150" });
    if (streamId) params.set("streamId", streamId);
    const result = await request(`/api/logs?${params.toString()}`);

    const allEntries = result.entries || [];
    const entries = logLevelFilter ? allEntries.filter((entry) => entry.level === logLevelFilter) : allEntries;
    if (!allEntries.length) {
      list.className = "log-list empty-state";
      list.textContent = "No log entries yet — start a stream to see fetch/download activity here.";
      return;
    }
    if (!entries.length) {
      list.className = "log-list empty-state";
      list.textContent = "No log entries match this filter.";
      return;
    }
    list.className = `log-list ${logMode === "verbose" ? "log-list-verbose" : ""}`;
    list.innerHTML = groupLogEntries(entries).map(logGroupHtml).join("");
  } catch (error) {
    list.className = "log-list empty-state";
    list.textContent = String(error.message || error);
  }
}

$("#refreshLogsBtn").addEventListener("click", loadLogs);
$("#providerSearch").addEventListener("input", (event) => {
  providerSearchQuery = event.currentTarget.value;
  renderProviders();
  applyIcons();
});
$("#streamsGridProviderFilter").addEventListener("change", (event) => {
  streamsGridProviderId = event.currentTarget.value;
  renderStreamsGrid();
});
$("#streamsGridTypeFilter").addEventListener("change", (event) => {
  streamsGridTypeFilter = event.currentTarget.value;
  renderStreamsGrid();
});
$("#streamsGridSearch").addEventListener("input", (event) => {
  streamsGridSearchQuery = event.currentTarget.value;
  renderStreamsGrid();
});
$("#gridRunningOnlyBtn").addEventListener("click", () => {
  streamsGridRunningOnly = !streamsGridRunningOnly;
  renderStreamsGrid();
});
$("#updateBannerDismiss")?.addEventListener("click", () => {
  updateBannerDismissed = true;
  $("#updateBanner").classList.add("hidden");
});
$("#gridStartAllBtn").addEventListener("click", () => bulkStreamAction("start"));
$("#gridStopAllBtn").addEventListener("click", () => bulkStreamAction("stop"));
$("#gridDeleteAllBtn").addEventListener("click", () => bulkStreamAction("delete"));
$("#gridNewStreamBtn").addEventListener("click", () => {
  // Attach to whichever provider the grid is currently filtered to; with no
  // filter (viewing every provider's streams at once) fall back to the
  // first provider rather than blocking the action entirely.
  selectedProviderId = streamsGridProviderId || state.providers[0]?.id || null;
  openNewStreamDialog();
});
$("#gridImportChannelsBtn").addEventListener("click", () => runProviderScriptForGrid("channels"));
$("#gridImportEventsBtn").addEventListener("click", () => runProviderScriptForGrid("events"));
$("#gridProviderSettingsBtn").addEventListener("click", () => {
  if (!streamsGridProviderId) return;
  selectedProviderId = streamsGridProviderId;
  openProviderSettingsDialog();
});
document.querySelectorAll(".find-logo-btn").forEach((button) => {
  button.addEventListener("click", async (event) => {
    const form = event.currentTarget.closest("form");
    const name = form.elements.name.value.trim();
    if (!name) return;
    event.currentTarget.disabled = true;
    try {
      const result = await request(`/api/logo-lookup?name=${encodeURIComponent(name)}`);
      if (result.logo) {
        form.elements.logo.value = result.logo;
      } else {
        alert(`No logo match found for "${name}".`);
      }
    } catch (error) {
      alert(`Logo lookup failed: ${error.message || error}`);
    } finally {
      event.currentTarget.disabled = false;
    }
  });
});
$("#connectionsSearch").addEventListener("input", (event) => {
  connectionsSearchQuery = event.currentTarget.value;
  renderConnectionsTable();
});
$("#logStreamFilter").addEventListener("change", loadLogs);
$("#logLevelFilter").addEventListener("change", (event) => {
  logLevelFilter = event.currentTarget.value;
  loadLogs();
});
document.querySelectorAll(".log-mode-btn").forEach((button) => {
  button.addEventListener("click", () => {
    logMode = button.dataset.mode;
    document.querySelectorAll(".log-mode-btn").forEach((b) => b.classList.toggle("active", b === button));
    loadLogs();
  });
});

// MARK: - Settings (port + users)

async function loadSettingsView() {
  try {
    const settings = await request("/api/settings");
    $("#portForm").elements.port.value = settings.port;
    $("#portForm").elements.bindAddress.value = settings.bindAddress || "";
  } catch (error) { /* ignore */ }
  await refreshUserList();
}

async function refreshUserList() {
  const list = $("#userList");
  try {
    const result = await request("/api/users");
    const users = result.users || [];
    if (!users.length) {
      list.className = "key-list empty-state";
      list.textContent = "No admin accounts found.";
      return;
    }
    list.className = "key-list";
    list.innerHTML = users.map((user) => `
      <div class="key-row">
        <div class="key-top">
          <strong>${escapeHtml(user.username)}</strong>
          ${users.length > 1 ? `<button type="button" class="danger" data-user-id="${escapeAttr(user.id)}"><span data-icon="trash"></span>Remove</button>` : ""}
        </div>
        <div class="key-meta">Created ${new Date(user.createdAt).toLocaleString()}</div>
      </div>
    `).join("");
    list.querySelectorAll("button[data-user-id]").forEach((button) => {
      button.addEventListener("click", async () => {
        await request(`/api/users/${button.dataset.userId}`, { method: "DELETE" });
        refreshUserList();
      });
    });
    applyIcons(list);
  } catch (error) {
    list.className = "key-list empty-state";
    list.textContent = String(error.message || error);
  }
}

$("#portForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  await request("/api/settings", {
    method: "POST",
    body: JSON.stringify({ port: Number(form.elements.port.value), bindAddress: form.elements.bindAddress.value }),
  });
  $("#viewMeta").textContent = "Saved — restart the server for it to take effect.";
});

$("#userForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  await request("/api/users", { method: "POST", body: JSON.stringify({ username: form.elements.username.value, password: form.elements.password.value }) });
  form.reset();
  refreshUserList();
});

// MARK: - Live monitoring (SSE)

function statTile(label, value, sub) {
  return `<div class="stat-tile"><span class="stat-label">${escapeHtml(label)}</span><span class="stat-value">${escapeHtml(value)}</span>${sub ? `<span class="stat-sub">${escapeHtml(sub)}</span>` : ""}</div>`;
}

function formatBytes(bytes) {
  const value = Number(bytes) || 0;
  const units = ["B", "KB", "MB", "GB", "TB"];
  let size = value;
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }
  return `${size.toFixed(size >= 10 || unit === 0 ? 0 : 1)} ${units[unit]}`;
}

function formatBytesPerSecond(bytes) {
  return `${formatBytes(bytes)}/s`;
}

let lastConnections = [];
let connectionsSearchQuery = "";

let lastMetricsPayload = null;

function applyMetrics(payload) {
  // Everything below paints into the monitor view, which is display:none on
  // every other tab. An SSE metrics frame arrives every second regardless of
  // which tab is open, so rebuilding all these tiles' and the connections
  // table's innerHTML while they're hidden is pure wasted DOM work. Cache the
  // frame and skip the repaint unless the monitor view is actually visible;
  // switchView repaints from this cache the instant the user comes back.
  lastMetricsPayload = payload;
  if (currentView !== "monitor") return;
  const global = payload.global || {};
  const globalInput = payload.globalInput || {};
  const system = payload.system || {};
  const cpuPercent = Number(system.cpuPercent || 0);
  const loadAverage = Number(system.loadAverage || 0);
  const memUsed = Number(system.memoryUsedBytes || 0);
  const memTotal = Number(system.totalMemoryBytes || 1);

  $("#headlineTiles").innerHTML = [
    statTile("Input Bandwidth", formatBytesPerSecond(globalInput.bytesPerSecond)),
    statTile("Output Bandwidth", formatBytesPerSecond(global.bytesPerSecond)),
    statTile("CPU Load", `${loadAverage.toFixed(2)} / ${system.coreCount || "?"}`),
    statTile("Memory Usage", formatBytes(memUsed)),
  ].join("");

  $("#serverTiles").innerHTML = [
    statTile("CPU", `${cpuPercent.toFixed(1)}%`, `${system.cpuModel || ""} · ${system.coreCount || "?"} cores · peak ${Number(system.peakCPUPercent || 0).toFixed(1)}%`),
    statTile("Memory", formatBytes(memUsed), `of ${formatBytes(memTotal)} · peak ${formatBytes(system.peakMemoryBytes)}`),
    statTile("Disk", formatBytes((system.diskTotalBytes || 0) - (system.diskAvailableBytes || 0)), `of ${formatBytes(system.diskTotalBytes)} used`),
    statTile("Uptime", formatUptime(system.uptimeSeconds), system.osVersion || ""),
    statTile("Build", appVersionLabel(), appVersionSubtitle()),
    statTile("All-time served", formatBytes(global.allTimeBytes)),
    statTile("All-time downloaded", formatBytes(globalInput.allTimeBytes)),
  ].join("");

  const streams = payload.streams || {};
  const streamIds = Object.keys(streams);
  const container = $("#streamTiles");
  if (!streamIds.length) {
    container.className = "tile-grid empty-state";
    container.textContent = "No streams yet.";
  } else {
    container.className = "tile-grid";
    container.innerHTML = streamIds.map((id) => {
      const info = streams[id];
      const name = findStreamName(id) || id;
      const bw = info.bandwidth || {};
      const inBw = info.inputBandwidth || {};
      return statTile(name, `${info.activeClients || 0} active`, `↓ ${formatBytesPerSecond(inBw.bytesPerSecond)} · ↑ ${formatBytesPerSecond(bw.bytesPerSecond)}`);
    }).join("");
  }

  lastConnections = payload.connections || [];
  renderConnectionsTable();
}

function repaintMonitorIfActive() {
  if (currentView === "monitor" && lastMetricsPayload) applyMetrics(lastMetricsPayload);
}

function renderConnectionsTable() {
  const query = connectionsSearchQuery.trim().toLowerCase();
  const rows = query
    ? lastConnections.filter((c) => [c.streamName, c.providerName, c.clientIP, c.userAgent, c.user].some((v) => String(v || "").toLowerCase().includes(query)))
    : lastConnections;
  $("#connectionsEmpty").classList.toggle("hidden", rows.length > 0);
  $("#connectionsTableBody").innerHTML = rows.map((c) => `
    <tr>
      <td>${escapeHtml(c.streamName || c.streamId || "")}</td>
      <td>${escapeHtml(c.providerName || "")}</td>
      <td>${escapeHtml((c.kind || "").toUpperCase())}</td>
      <td>${escapeHtml(c.user || "")}</td>
      <td>${escapeHtml(c.clientIP || "")}</td>
      <td class="ua-cell" title="${escapeAttr(c.userAgent || "")}">${escapeHtml(c.userAgent || "")}</td>
      <td>${escapeHtml(c.uid || "")}</td>
      <td>${formatUptime(c.uptimeSeconds)}</td>
      <td class="${Number(c.errors) > 0 ? "errors-cell" : ""}">${Number(c.errors) || 0}</td>
      <td>${formatBytesPerSecond(c.bytesPerSecond)}</td>
    </tr>
  `).join("");
}

function findStreamName(streamId) {
  for (const provider of state.providers) {
    const match = provider.streams.find((stream) => stream.id === streamId);
    if (match) return match.name;
  }
  return null;
}

function formatUptime(seconds) {
  const total = Math.floor(Number(seconds) || 0);
  const days = Math.floor(total / 86400);
  const hours = Math.floor((total % 86400) / 3600);
  const minutes = Math.floor((total % 3600) / 60);
  if (days > 0) return `${days}d ${hours}h`;
  if (hours > 0) return `${hours}h ${minutes}m`;
  return `${minutes}m`;
}

function connectEvents() {
  if (eventSource) return;
  eventSource = new EventSource("/api/events");
  eventSource.onopen = () => { $("#monitorStatus").textContent = "Live"; };
  eventSource.onerror = () => { $("#monitorStatus").textContent = "Reconnecting…"; };
  eventSource.onmessage = (event) => {
    try {
      applyMetrics(JSON.parse(event.data));
    } catch (error) {
      // ignore malformed frame
    }
  };
}

// MARK: - Auth

function showAuthScreen() {
  $("#app").classList.add("hidden");
  $("#authScreen").classList.remove("hidden");
  if (eventSource) { eventSource.close(); eventSource = null; }
}

function showApp() {
  $("#authScreen").classList.add("hidden");
  $("#app").classList.remove("hidden");
}

async function checkAuth() {
  const status = await fetch("/api/auth/status").then((r) => r.json()).catch(() => ({ needsSetup: true, authenticated: false }));
  authenticated = Boolean(status.authenticated);
  if (status.needsSetup) {
    $("#authSubtitle").textContent = "Create the admin account";
    $("#authSubmit").textContent = "Create account";
    showAuthScreen();
    return false;
  }
  if (!authenticated) {
    $("#authSubtitle").textContent = "Sign in";
    $("#authSubmit").textContent = "Sign in";
    showAuthScreen();
    return false;
  }
  $("#whoami").textContent = status.username ? `Signed in as ${status.username}` : "";
  showApp();
  return true;
}

$("#authForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  $("#authError").classList.add("hidden");
  const status = await fetch("/api/auth/status").then((r) => r.json()).catch(() => ({ needsSetup: true }));
  const path = status.needsSetup ? "/api/auth/setup" : "/api/auth/login";
  try {
    const response = await fetch(path, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        username: form.elements.username.value,
        password: form.elements.password.value,
        remember: form.elements.remember.checked ? "true" : "false",
      }),
    });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(payload.error || `HTTP ${response.status}`);
    form.reset();
    boot();
  } catch (error) {
    $("#authError").textContent = String(error.message || error);
    $("#authError").classList.remove("hidden");
  }
});

$("#signOutBtn").addEventListener("click", async () => {
  await fetch("/api/auth/logout", { method: "POST" }).catch(() => {});
  authenticated = false;
  showAuthScreen();
});

// MARK: - Theme

function applyThemeIcon() {
  const current = document.documentElement.dataset.theme;
  const prefersDark = window.matchMedia("(prefers-color-scheme: dark)").matches;
  const isDark = current ? current === "dark" : prefersDark;
  $("#themeToggle").dataset.icon = isDark ? "sun" : "moon";
  applyIcons($("#themeToggle"));
}

function applyTheme(theme) {
  if (theme) {
    document.documentElement.dataset.theme = theme;
    localStorage.setItem("restreamair-theme", theme);
  } else {
    delete document.documentElement.dataset.theme;
    localStorage.removeItem("restreamair-theme");
  }
  applyThemeIcon();
}

function toggleTheme() {
  const current = document.documentElement.dataset.theme;
  const prefersDark = window.matchMedia("(prefers-color-scheme: dark)").matches;
  if (!current) {
    applyTheme(prefersDark ? "light" : "dark");
  } else if (current === "dark") {
    applyTheme("light");
  } else {
    applyTheme(null);
  }
}

// MARK: - Misc helpers

function escapeHtml(value) {
  return String(value).replace(/[&<>"']/g, (char) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#039;" }[char]));
}

function escapeAttr(value) {
  return escapeHtml(value);
}

function openNewProviderDialog() {
  const form = $("#providerForm");
  form.reset();
  $("#newProviderDialog").showModal();
  form.elements.name.focus();
}

function closeNewProviderDialog() {
  if ($("#newProviderDialog").open) $("#newProviderDialog").close();
}

$("#newProviderBtn").addEventListener("click", openNewProviderDialog);
$("#importProviderBtn").addEventListener("click", () => $("#importProviderFile").click());
$("#importProviderFile").addEventListener("change", async (event) => {
  const file = event.currentTarget.files[0];
  event.currentTarget.value = "";
  if (file) await importProviderFromFile(file);
});
$("#closeNewProviderBtn").addEventListener("click", closeNewProviderDialog);
$("#newProviderDialog").addEventListener("click", (event) => {
  if (event.target === event.currentTarget) closeNewProviderDialog();
});

$("#providerForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  state = await request("/api/providers", {
    method: "POST",
    body: JSON.stringify({ name: form.elements.name.value, logo: form.elements.logo.value }),
  });
  selectedProviderId = state.providers.at(-1)?.id || selectedProviderId;
  closeNewProviderDialog();
  render();
});

$("#streamForm").addEventListener("submit", saveStream);
$("#toggleRunBtn").addEventListener("click", toggleSelectedRun);
$("#deleteStreamBtn").addEventListener("click", deleteSelectedStream);
$("#deleteProviderBtn").addEventListener("click", deleteSelectedProvider);
$("#closeStreamEditorBtn").addEventListener("click", closeStreamEditorDialog);
$("#streamEditorDialog").addEventListener("click", (event) => {
  if (event.target === event.currentTarget) closeStreamEditorDialog();
});
$("#streamEditorDialog").addEventListener("close", () => { editingNewStream = false; stopPlayer(); });
$("#pipVideo").addEventListener("leavepictureinpicture", stopPipPlayer);
$("#themeToggle").addEventListener("click", toggleTheme);
$("#streamForm").elements.url.addEventListener("input", scheduleAutoDetect);
$("#streamForm").elements.url.addEventListener("blur", () => { clearTimeout(autoDetectTimer); autoDetectFromUrlField(); });
$("#streamForm").elements.url.addEventListener("paste", () => setTimeout(autoDetectFromUrlField, 30));
$("#streamForm").elements.decryptionKeys.addEventListener("input", updateDecryptionKeysVisibility);
$("#showDecryptionKeysBtn").addEventListener("click", () => {
  decryptionKeysManuallyShown = true;
  updateDecryptionKeysVisibility();
  $("#streamForm").elements.decryptionKeys.focus();
});
$("#closeProviderSettingsBtn").addEventListener("click", closeProviderSettingsDialog);
$("#providerSettingsDialog").addEventListener("click", (event) => {
  if (event.target === event.currentTarget) closeProviderSettingsDialog();
});
$("#providerSettingsDialog").addEventListener("close", stopScriptOutputPoll);
$("#providerSettingsForm").elements.inheritUrlParams.addEventListener("change", updateSegmentUrlParamsVisibility);
$("#addScriptAccountBtn").addEventListener("click", () => {
  const username = $("#newScriptAccountUsername").value.trim();
  if (!username) return;
  const account = {
    id: `account_${Date.now()}_${Math.random().toString(36).slice(2, 8)}`,
    name: username, username,
    password: $("#newScriptAccountPassword").value,
    params: "",
    enabled: true,
  };
  scriptAccountsDraft.push(account);
  if (!activeScriptAccountIdDraft) activeScriptAccountIdDraft = account.id;
  $("#newScriptAccountUsername").value = "";
  $("#newScriptAccountPassword").value = "";
  renderScriptAccountsList();
});
$("#scriptLoginBtn").addEventListener("click", () => runProviderScript("login"));
$("#scriptPairBtn").addEventListener("click", () => runProviderScript("pair"));
$("#scriptLoadChannelsBtn").addEventListener("click", () => runProviderScript("channels"));
$("#scriptLoadEventsBtn").addEventListener("click", () => runProviderScript("events"));
$("#toggleScriptOutputBtn").addEventListener("click", () => {
  $("#scriptOutputBox").classList.toggle("hidden");
  updateScriptOutputToggleLabel();
});
$("#copyBtn").addEventListener("click", async () => {
  const link = $("#playLink").value;
  if (link) await navigator.clipboard.writeText(link);
});
$("#copyDirectBtn").addEventListener("click", async () => {
  const link = $("#directLink").dataset.full;
  if (link) await navigator.clipboard.writeText(link);
});
$("#qualitySelect").addEventListener("change", (event) => {
  if (hls) hls.currentLevel = Number(event.currentTarget.value);
});
$("#audioTrackSelect").addEventListener("change", (event) => {
  if (hls) hls.audioTrack = Number(event.currentTarget.value);
});
document.querySelectorAll(".tab").forEach((button) => button.addEventListener("click", () => switchTab(button.dataset.tab)));
document.querySelectorAll(".nav-btn").forEach((button) => button.addEventListener("click", () => switchView(button.dataset.view)));

applyIcons();
const savedTheme = localStorage.getItem("restreamair-theme");
if (savedTheme) applyTheme(savedTheme);
else applyThemeIcon();
updateKindVisibility();

if ("serviceWorker" in navigator) {
  navigator.serviceWorker.register("/sw.js").catch(() => {});
}

async function boot() {
  if (bootStarted) return;
  const ok = await checkAuth();
  if (!ok) return;
  bootStarted = true;
  document.querySelectorAll(".stream-form label, #providerChips").forEach((el) => el.classList.add("skeleton"));
  refresh()
    .then(() => {
      document.querySelectorAll(".skeleton").forEach((el) => el.classList.remove("skeleton"));
    })
    .catch((error) => {
      document.querySelectorAll(".skeleton").forEach((el) => el.classList.remove("skeleton"));
      $("#statusBox").textContent = String(error.message || error);
    });
  setInterval(() => refresh().catch(() => {}), 4000);
  connectEvents();
}

boot();
