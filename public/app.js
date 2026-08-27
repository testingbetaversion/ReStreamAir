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
  help: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="9"/><path d="M9.3 9.2a2.8 2.8 0 0 1 5.4 1c0 1.8-2.7 2.3-2.7 4"/><circle cx="12" cy="17" r=".7" fill="currentColor" stroke="none"/></svg>`,
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
let currentView = "server";
let eventSource = null;
let authenticated = false;
let probeResult = null;
let selectedRepIds = new Set();
let bootStarted = false;
let bootPromise = null;
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

let refreshPromise = null;
let stateMutationEpoch = 0;

async function refreshOnce() {
  const epoch = stateMutationEpoch;
  const fresh = await request("/api/state");
  // A GET issued just before Start can arrive after the POST response and used
  // to paint the old `stopped` snapshot over the new running state. That made
  // a successful broadcast visibly "drop" until another refresh.
  if (epoch !== stateMutationEpoch) return;
  state = fresh;
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

function refresh() {
  // setInterval does not wait for the previous tick. Under load, overlapping
  // state requests and full DOM renders made the panel look frozen precisely
  // when several channels were starting. Coalesce every caller onto one refresh.
  if (!refreshPromise) {
    refreshPromise = refreshOnce().finally(() => { refreshPromise = null; });
  }
  return refreshPromise;
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
  if (currentView === "server") {
    $("#viewTitle").textContent = "Server information";
    $("#viewMeta").textContent = "Host health, capacity, and build information.";
  } else if (currentView === "monitor") {
    $("#viewTitle").textContent = "Monitoring";
    $("#viewMeta").textContent = "Live bandwidth by stream and by connected client.";
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
  } else if (currentView === "help") {
    $("#viewTitle").textContent = "Help";
    $("#viewMeta").textContent = "How the panel, scripts, and DRM work.";
  } else {
    $("#viewTitle").textContent = "Providers";
    $("#viewMeta").textContent = "Click a provider to see and manage its streams.";
  }
}

let logsPollTimer = null;

// MARK: - URL routing
//
// Every view has its own address, so the browser's back/forward buttons work,
// a page can be reloaded where you left it, and a view can be linked to or
// bookmarked. The two views that carry context put it in the query string
// (?provider= on All Streams, ?stream= on Logs) so a deep link lands
// pre-filtered exactly the way the in-app shortcuts do.
//
// Real paths rather than a hash, so the URLs read like pages; both servers
// serve index.html for this fixed list (an unknown path still 404s, so a typo
// is not silently swallowed by the SPA). A "#/logs" hash is still honoured on
// the way in, which keeps deep links working if the panel is ever opened from
// a file:// copy or behind a proxy that cannot rewrite paths.
const VIEW_ROUTES = {
  providers: "/providers",
  grid: "/streams",
  server: "/server",
  monitor: "/monitoring",
  logs: "/logs",
  keys: "/keys",
  settings: "/settings",
  help: "/help",
};
const ROUTE_VIEWS = Object.fromEntries(Object.entries(VIEW_ROUTES).map(([view, path]) => [path, view]));
const DEFAULT_VIEW = "server";

// The address that represents the app's current state, including the filter
// context of whichever view is showing.
function urlForView(view) {
  const path = VIEW_ROUTES[view] || VIEW_ROUTES[DEFAULT_VIEW];
  const params = new URLSearchParams();
  if (view === "grid" && streamsGridProviderId) params.set("provider", streamsGridProviderId);
  if (view === "logs") {
    const streamId = pendingLogStreamId !== null
      ? pendingLogStreamId
      : ($("#logStreamFilter")?.value || "");
    if (streamId) params.set("stream", streamId);
  }
  const query = params.toString();
  return path + (query ? `?${query}` : "");
}

// Which view the current address names, plus any filter it carries.
function routeFromLocation() {
  const hash = location.hash.replace(/^#/, "");
  const usingHash = hash.startsWith("/");
  const raw = usingHash ? hash : location.pathname;
  const [rawPath, rawQuery] = raw.split("?");
  const path = rawPath.replace(/\/+$/, "") || "/";
  const params = new URLSearchParams(usingHash ? (rawQuery || "") : location.search);
  return {
    view: ROUTE_VIEWS[path] || DEFAULT_VIEW,
    provider: params.get("provider") || "",
    stream: params.get("stream") || "",
  };
}

// Point the address bar at `view` without navigating. Replace rather than push
// when the entry is only being corrected (boot, or a filter change inside the
// view already showing), so the back button does not collect duplicates.
function syncUrl(view, replace) {
  const url = urlForView(view);
  const current = location.pathname + location.search;
  if (url === current) return;
  if (replace) history.replaceState({ view }, "", url);
  else history.pushState({ view }, "", url);
}

// Adopt whatever view the address names — used at boot and on back/forward.
function applyRoute() {
  const route = routeFromLocation();
  if (route.view === "grid") streamsGridProviderId = route.provider;
  if (route.view === "logs" && route.stream) pendingLogStreamId = route.stream;
  // "replace", not "none": adopting the route should also canonicalise the
  // address, so "/" and the "#/logs" hash form land on the same URL every other
  // navigation produces — without pushing a duplicate history entry.
  switchView(route.view, { history: "replace" });
}

window.addEventListener("popstate", applyRoute);

// `history` is "push" (a normal navigation), "replace" (correcting the current
// entry), or "none" (the address already says this — we are following it).
function switchView(view, { history: historyMode = "push" } = {}) {
  // Before the view transition, not inside it: startViewTransition defers the
  // callback, so anything that read currentView straight after a switchView()
  // call got the previous view.
  currentView = view;
  if (historyMode !== "none") syncUrl(view, historyMode === "replace");
  const apply = () => {
    document.querySelectorAll(".nav-btn").forEach((button) => button.classList.toggle("active", button.dataset.view === view));
    $("#providersView").classList.toggle("hidden", view !== "providers");
    $("#streamsGridView").classList.toggle("hidden", view !== "grid");
    $("#serverView").classList.toggle("hidden", view !== "server");
    $("#monitorView").classList.toggle("hidden", view !== "monitor");
    $("#logsView").classList.toggle("hidden", view !== "logs");
    $("#keysView").classList.toggle("hidden", view !== "keys");
    $("#settingsView").classList.toggle("hidden", view !== "settings");
    $("#helpView").classList.toggle("hidden", view !== "help");
    renderViewHeader();
    if (view === "grid") renderStreamsGrid();
    if (view === "server" || view === "monitor") repaintMonitorIfActive();
    if (view === "settings") loadSettingsView();
    // Logs auto-refresh only while the tab is actually open, same reasoning
    // as the existing "no overhead when closed" comment on loadLogs itself.
    clearInterval(logsPollTimer);
    logsPollTimer = null;
    if (view === "logs") {
      loadLogs();
      if (!logsPaused) logsPollTimer = setInterval(loadLogs, 2000);
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
let streamsGridOrderKey = "";

// Update just the live/changing parts of an already-rendered stream card, so
// the auto-refresh doesn't rebuild (and visually jump) the whole grid.
function updateStreamCardDynamic(card, stream) {
  if (!card) return;
  const badge = card.querySelector(".badge");
  if (badge) {
    badge.textContent = stream.status || "stopped";
    badge.classList.toggle("running", Boolean(stream.running));
  }
  const stats = card.querySelectorAll(".stream-stats span");
  if (stats.length >= 3) {
    stats[0].textContent = `${stream.activeClients ?? 0} active`;
    stats[1].textContent = `↓ ${formatBytesPerSecond((stream.inputBandwidth || {}).bytesPerSecond)}`;
    stats[2].textContent = `↑ ${formatBytesPerSecond((stream.bandwidth || {}).bytesPerSecond)}`;
  }
  const toggle = card.querySelector('[data-action="toggle"]');
  if (toggle) {
    toggle.className = stream.running ? "danger" : "success";
    toggle.innerHTML = `<span data-icon="${stream.running ? "stop" : "play"}"></span>${stream.running ? "Stop" : "Start"}`;
    applyIcons(toggle);
  }
}

// Look a stream up in the current state by id. Click handlers must resolve the
// stream at click time rather than closing over the object they were rendered
// with — see the toggle handler in renderStreamsGrid.
function findStreamById(id) {
  for (const provider of state.providers) {
    const found = provider.streams.find((s) => s.id === id);
    if (found) return found;
  }
  return null;
}

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
  stateMutationEpoch++;
  // Starting many DASH engines simultaneously creates every director, poller,
  // writer and download pool in one spike. Serialize bulk operations; the UI
  // already promised this behavior and it keeps the panel event loop responsive.
  for (const { stream } of rows) {
    let path = null;
    let method = "POST";
    if (kind === "start" && !stream.running) path = `/api/streams/${stream.id}/start`;
    else if (kind === "stop" && stream.running) path = `/api/streams/${stream.id}/stop`;
    else if (kind === "delete") { path = `/api/streams/${stream.id}`; method = "DELETE"; }
    if (!path) continue;
    try {
      await request(path, { method, body: method === "POST" ? "{}" : undefined });
      count++;
    } catch (error) {
      $("#streamsGridImportStatus").classList.remove("hidden");
      $("#streamsGridImportStatus").textContent = `${kind} failed on "${stream.name}": ${error.message || error}`;
    }
  }
  if (count > 0) state = await request("/api/state");
  stateMutationEpoch++;
  
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
  // Import needs a specific script provider, not "whichever's first". The
  // buttons live in the bulk bar next to Start all so they're always visible
  // (discoverable), but only enable once the grid is filtered down to a single
  // provider that actually has a script configured — the tooltip says so.
  const filteredProvider = state.providers.find((p) => p.id === streamsGridProviderId);
  const showImport = Boolean(filteredProvider?.scriptPath);
  $("#gridImportChannelsBtn").disabled = !showImport;
  $("#gridImportEventsBtn").disabled = !showImport;
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
  // Fast path: when the same streams are shown in the same order (the common
  // case on the 4s auto-refresh), update just the live bits in place instead of
  // rebuilding every card. Rebuilding reloaded each logo <img> and reflowed the
  // grid, so cards visibly flashed and jumped every few seconds.
  // Key on the static card content (not status/bandwidth — those are the live
  // bits the fast path updates), so editing a name/logo/url still rebuilds.
  const orderKey = rows.map(({ provider, stream }) => [stream.id, stream.name, stream.logo, stream.kind, stream.url, provider.name, stream.sourceType].join("|")).join(",");
  if (orderKey === streamsGridOrderKey && container.children.length === rows.length && !container.classList.contains("empty-state")) {
    rows.forEach(({ stream }, index) => updateStreamCardDynamic(container.children[index], stream));
    return;
  }
  streamsGridOrderKey = orderKey;
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
        <span title="Downloaded from the origin">↓ ${formatBytesPerSecond(inputBandwidth.bytesPerSecond)}</span>
        <span title="Served to viewers">↑ ${formatBytesPerSecond(bandwidth.bytesPerSecond)}</span>
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
      // Re-read the stream from the live state instead of the object this card
      // was built from. The fast-path refresh above (updateStreamCardDynamic)
      // relabels an existing card without rebuilding it — orderKey deliberately
      // ignores `running` so the grid does not churn — so the captured object
      // goes stale the moment the stream starts or stops. Acting on it sent the
      // opposite request to what the button said: press Stop, then press the
      // button again (now reading "Start") and it POSTed /stop a second time.
      const live = findStreamById(stream.id) || stream;
      await toggleStreamRun(live.id, live.running);
    });
    card.querySelector('[data-action="delete"]').addEventListener("click", async (event) => {
      event.stopPropagation();
      const live = findStreamById(stream.id) || stream;
      await deleteStreamId(live.id, live.name);
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
  if (await copyToClipboard(url)) {
    if (button) flashButtonCopied(button);
  } else {
    window.prompt("Copy the output URL:", url);
  }
}

// Copy text to the clipboard, working on plain-HTTP origins too. The modern
// navigator.clipboard API only exists in a "secure context" (https or
// localhost) — the panel is almost always reached over http://<lan-ip>:port,
// where it's undefined and every copy fell through to a manual prompt() popup.
// Fall back to a hidden <textarea> + execCommand("copy"), which works on http.
async function copyToClipboard(text) {
  try {
    if (window.isSecureContext && navigator.clipboard) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch (_) { /* fall through to the legacy path */ }
  try {
    const area = document.createElement("textarea");
    area.value = text;
    area.setAttribute("readonly", "");
    area.style.position = "fixed";
    area.style.top = "-1000px";
    area.style.opacity = "0";
    document.body.appendChild(area);
    area.focus();
    area.select();
    area.setSelectionRange(0, text.length);
    const ok = document.execCommand("copy");
    document.body.removeChild(area);
    return ok;
  } catch (_) {
    return false;
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
  fetchNM3U8DLREStatus();
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
      form.elements.hlsSegmentSeconds.value = 10;
      form.elements.playbackDelaySeconds.value = 0;
      form.elements.keepSegments.value = 60;
      form.elements.downloadAhead.value = 20;
      form.elements.parallelDownloads.value = 6;
      form.elements.prioritizeOldest.checked = false;
      form.elements.pollInterval.value = 0;
      form.elements.reducedManifestPolling.checked = false;
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
    directSource: Boolean(stream.directSource),
    sourceUrl: stream.sourceUrl,
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
  form.elements.downloader.value = stream.downloader || "";
  form.elements.downloaderParams.value = stream.downloaderParams || "";
  form.elements.playlistSegments.value = stream.playlistSegments || 6;
  form.elements.hlsSegmentSeconds.value = stream.hlsSegmentSeconds || 10;
  form.elements.playbackDelaySeconds.value = stream.playbackDelaySeconds || 0;
  form.elements.keepSegments.value = stream.keepSegments || 60;
  form.elements.downloadAhead.value = stream.downloadAhead ?? 20;
  form.elements.parallelDownloads.value = stream.parallelDownloads ?? 6;
  form.elements.prioritizeOldest.checked = stream.prioritizeOldest ?? false;
  form.elements.pollInterval.value = stream.pollInterval || 2;
  form.elements.audioDelayMs.value = stream.audioDelayMs || 0;
  form.elements.tvgId.value = stream.tvgId || "";
  form.elements.forceOffline.checked = Boolean(stream.forceOffline);
  form.elements.reducedManifestPolling.checked = Boolean(stream.reducedManifestPolling);
  form.elements.directSource.checked = Boolean(stream.directSource);
  form.elements.manifestHeaders.value = stream.manifestHeaders || "";
  form.elements.mediaHeaders.value = stream.mediaHeaders || "";
  form.elements.hlsKeyHeaders.value = stream.hlsKeyHeaders || "";
  form.elements.decryptionKeys.value = stream.decryptionKeys || "";
  form.elements.hlsKey.value = stream.hlsKey || "";
  form.elements.hlsIV.value = stream.hlsIV || "";
  form.elements.inputMode.value = stream.inputMode || "internal";
  form.elements.outputMode.value = stream.outputMode || "hls";
  form.elements.outputTarget.value = stream.outputTarget || "";
  form.elements.pipeCommand.value = stream.pipeCommand || "";
  if (form.elements.nm3u8dlreParams) {
    form.elements.nm3u8dlreParams.value = stream.nm3u8dlreParams || "";
  }
  form.elements.useCdm.checked = Boolean(stream.useCdm);
  form.elements.sessionManifest.checked = Boolean(stream.sessionManifest);
  // null/absent override means "inherit the provider"; an array (even an empty
  // one) means this stream decides for itself.
  const providerActions = selectedProvider()?.scriptActions || [];
  const override = stream.scriptActionsOverride;
  const hasOverride = Array.isArray(override);
  $("#overrideScriptActions").checked = hasOverride;
  $("#streamScriptActions").classList.toggle("hidden", !hasOverride);
  renderScriptActions($("#streamScriptActions"), hasOverride ? override : providerActions);
  form.elements.scriptOverride.value = stream.scriptOverride || "";
  form.elements.heartbeatSeconds.value = stream.heartbeatSeconds || 0;
  form.elements.scriptParams.value = stream.scriptParams || "";
  // Proxy scope defaults to all-on when unset (older streams).
  form.elements.proxyScript.checked = stream.proxyScript !== false;
  form.elements.proxyManifest.checked = stream.proxyManifest !== false;
  form.elements.proxyMedia.checked = stream.proxyMedia !== false;
  // Auto-reveal the Scripting & DRM section when it holds saved values.
  const isImported = stream.sourceType === "channel" || stream.sourceType === "event";
  $("#scriptingGroup").classList.toggle("hidden", !(stream.useCdm || stream.sessionManifest || (stream.scriptOverride || "").trim() || stream.heartbeatSeconds || (stream.scriptParams || "").trim() || isImported));
  const importedHint = $("#importedScriptHint");
  if (importedHint) importedHint.classList.toggle("hidden", !isImported);
  updatePipelineFieldVisibility();

  selectedRepIds = new Set(stream.representations || []);
  const savedMeta = stream.representationMeta || {};
  const savedOrder = Array.isArray(stream.representationOrder) && stream.representationOrder.length
    ? stream.representationOrder
    : Object.keys(savedMeta);
  probeResult = {
    kind: stream.kind,
    representations: savedOrder.filter((id) => savedMeta[id]).map((id) => ({ id, ...savedMeta[id] })),
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
      body: JSON.stringify({ url, proxy: stream.proxy || selectedProvider()?.proxy || "", headers: stream.manifestHeaders || "", forceIpv6: Boolean(selectedProvider()?.forceIpv6), rotateProxies: Boolean(selectedProvider()?.rotateProxies) }),
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
    representationOrder: (probeResult?.representations || []).map((rep) => rep.id),
    representationMeta,
    period: form.elements.period.value,
    proxy: form.elements.proxy.value,
    downloader: form.elements.downloader.value,
    downloaderParams: form.elements.downloaderParams.value,
    playlistSegments: Number(form.elements.playlistSegments.value),
    hlsSegmentSeconds: Number(form.elements.hlsSegmentSeconds.value) || 10,
    playbackDelaySeconds: Number(form.elements.playbackDelaySeconds.value),
    keepSegments: Number(form.elements.keepSegments.value),
    downloadAhead: Number(form.elements.downloadAhead.value),
    parallelDownloads: Number(form.elements.parallelDownloads.value),
    prioritizeOldest: form.elements.prioritizeOldest.checked,
    pollInterval: Number(form.elements.pollInterval.value),
    audioDelayMs: Number(form.elements.audioDelayMs.value) || 0,
    tvgId: form.elements.tvgId.value.trim(),
    forceOffline: form.elements.forceOffline.checked,
    reducedManifestPolling: form.elements.reducedManifestPolling.checked,
    directSource: form.elements.directSource.checked,
    manifestHeaders: form.elements.manifestHeaders.value,
    mediaHeaders: form.elements.mediaHeaders.value,
    hlsKeyHeaders: form.elements.hlsKeyHeaders.value,
    decryptionKeys: form.elements.decryptionKeys.value,
    hlsKey: form.elements.hlsKey.value,
    hlsIV: form.elements.hlsIV.value,
    inputMode: form.elements.inputMode.value,
    outputMode: form.elements.outputMode.value,
    outputTarget: form.elements.outputTarget.value,
    pipeCommand: form.elements.pipeCommand.value,
    nm3u8dlreParams: form.elements.nm3u8dlreParams?.value || "",
    useCdm: form.elements.useCdm.checked,
    sessionManifest: form.elements.sessionManifest.checked,
    scriptOverride: form.elements.scriptOverride.value,
    scriptActionsOverride: $("#overrideScriptActions").checked
      ? readScriptActions($("#streamScriptActions"))
      : null,
    heartbeatSeconds: Number(form.elements.heartbeatSeconds.value) || 0,
    scriptParams: form.elements.scriptParams.value,
    proxyScript: form.elements.proxyScript.checked,
    proxyManifest: form.elements.proxyManifest.checked,
    proxyMedia: form.elements.proxyMedia.checked,
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
  try {
    stateMutationEpoch++;
    state = await request(`/api/streams/${id}/${running ? "stop" : "start"}`, { method: "POST", body: "{}" });
    stateMutationEpoch++;
    render();
  } catch (error) {
    alert(`Failed to ${running ? "stop" : "start"} stream: ${error.message || error}`);
  }
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
  $("#streamForm").elements.hlsSegmentSeconds.value = 10;
  $("#streamForm").elements.playbackDelaySeconds.value = 0;
  $("#streamForm").elements.keepSegments.value = 10;
  $("#streamForm").elements.downloadAhead.value = 20;
  $("#streamForm").elements.parallelDownloads.value = 6;
  $("#streamForm").elements.prioritizeOldest.checked = false;
  $("#streamForm").elements.pollInterval.value = 0;
  $("#streamForm").elements.reducedManifestPolling.checked = false;
  $("#playLink").value = "";
  setLinkField($("#directLink"), {});
  renderCdnMirrors([]);
  $("#statusBox").textContent = "New stream.";
  $("#scriptingGroup").classList.add("hidden");
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

// Stream settings the server persists but does not act on yet.
//
// The panel and the engine move at different speeds: the panel stores every
// field the editor has ever offered, while the engine reads the subset it has
// implemented.
// A control that silently does nothing is worse than a missing one — the
// rendition picker looked like it worked for months while the engine ignored it
// and auto-selected the top 1080p rendition, so "I selected 720p" and "it plays
// 1080p" were both true. Marking them is the cheap half of the fix; the values
// are still stored and start working the moment the engine reads them.
const INACTIVE_FIELDS = {
  period: "the engine reads every DASH period; selection isn't implemented",
  forceOffline: "not implemented in this build",
  onDemand: "not implemented in this build",
  autostart: "streams marked running resume on restart regardless",
  speedUp: "not implemented in this build",
  recordEvent: "not implemented in this build",
  nm3u8dlreParams: "the N_m3u8DL-RE input mode isn't in this build",
  useCdm: "CDM/script key resolution isn't in this build",
  cdmType: "CDM/script key resolution isn't in this build",
  heartbeatEnabled: "provider scripts aren't in this build",
  heartbeatSeconds: "provider scripts aren't in this build",
  scriptParams: "provider scripts aren't in this build",
  scriptAudioSelector: "provider scripts aren't in this build",
  scriptVideoSelector: "provider scripts aren't in this build",
};

// Dims each unimplemented control and appends the reason to its hint, so the
// operator can tell "this does nothing yet" from "this isn't working".
function markInactiveFields(form) {
  for (const [name, why] of Object.entries(INACTIVE_FIELDS)) {
    for (const el of form.querySelectorAll(`[name="${name}"]`)) {
      const label = el.closest("label") || el.parentElement;
      if (!label || label.dataset.inactiveMarked) continue;
      label.dataset.inactiveMarked = "1";
      label.classList.add("field-inactive");
      label.title = `Not active in this build — ${why}`;
      const badge = document.createElement("span");
      badge.className = "inactive-badge";
      badge.textContent = "inactive";
      badge.title = label.title;
      label.appendChild(badge);
    }
  }
}

function openStreamEditorDialog(createNew) {
  if (createNew) newStream();
  $("#streamEditorTitle").textContent = selectedStream()?.name || "New stream";
  switchTab("editor");
  $("#streamEditorDialog").showModal();
  markInactiveFields($("#streamForm"));
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
// Use seconds rather than segment counts: output segment duration is
// configurable, and ten-second segments must not turn an 8-segment target
// into 80 seconds of extra latency. Twenty seconds gives the player two full
// default segments while the
// sources this app restreams get fetched through slow/rate-limited proxies,
// so the server's own poll cycle can take 10-20s+ per batch and the live edge
// jumps forward in bursts rather than one segment at a time. hls.js kept
// recomputing its target to stay ~4s behind a point that had already moved by
// the time a fragment finished loading, aborting the in-flight load and
// retargeting forever — buffered stayed empty no matter how long it waited,
// with no error to recover from because nothing was actually failing. Trading
// ~16s more live latency for a target that survives one burst gives it
// something to actually catch up to.
const HLS_CONFIG = {
  liveSyncDuration: 20,
  liveMaxLatencyDuration: 60,
  maxLiveSyncPlaybackRate: 1,
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

// hls.js surfaces every failure as an ERROR event rather than throwing, so
// with nothing listening a fatal one — a network exhausted retries, or (the
// common case on the flaky live sources this app restreams) an MSE
// buffer-append failure right after an EXT-X-DISCONTINUITY — just left the
// player sitting on a dead <video> forever with no indication why. This is
// hls.js's own documented recovery: network errors resume with startLoad(),
// media errors resume with recoverMediaError(). recoverMediaError() alone
// isn't enough for a source that keeps re-erroring — mediaErrorCount only
// resets on an actual buffered frame, so repeated errors with no progress
// between them escalate to `onUnrecoverable` instead of retrying forever.
function attachHlsErrorRecovery(hlsInstance, onUnrecoverable) {
  let mediaErrorCount = 0;
  hlsInstance.on(window.Hls.Events.FRAG_BUFFERED, () => { mediaErrorCount = 0; });
  hlsInstance.on(window.Hls.Events.ERROR, (_event, data) => {
    if (!data.fatal) return;
    switch (data.type) {
      case window.Hls.ErrorTypes.NETWORK_ERROR:
        hlsInstance.startLoad();
        break;
      case window.Hls.ErrorTypes.MEDIA_ERROR:
        mediaErrorCount++;
        if (mediaErrorCount <= 3) hlsInstance.recoverMediaError();
        else onUnrecoverable?.(data);
        break;
      default:
        onUnrecoverable?.(data);
        break;
    }
  });
}

// Reload attempts are capped and reset whenever the player is (re)loaded for
// a stream the user picked, so a permanently broken source gives up instead
// of hammering the server in a tight loop, while a transient run of errors
// right after opening the player still gets a few full retries.
let playerReloadAttempts = 0;

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
  playerReloadAttempts = 0;
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
    hls.on(window.Hls.Events.LEVEL_SWITCHED, (_event, data) => {
      if (hls && hls.manualLevel !== -1) $("#qualitySelect").value = String(data.level);
    });
    attachHlsErrorRecovery(hls, (data) => {
      console.error("hls.js gave up on", stream.name, data);
      if (playerReloadAttempts++ < 5 && selectedStream()?.id === stream.id) {
        setTimeout(loadPlayer, 1000);
      } else {
        $("#statusBox").textContent = `Playback stopped: ${data.details || data.type}. Reopen the player to retry.`;
      }
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
      // No reload loop here like the main player's — requestPictureInPicture()
      // below only runs once, on this user gesture, and reopening the floating
      // window on its own later wouldn't be allowed without another one. Once
      // hls.js truly gives up, just tear it down so the window doesn't sit
      // there frozen on a dead frame.
      attachHlsErrorRecovery(pipHls, (data) => {
        console.error("hls.js gave up on PiP playback for", stream.name, data);
        stopPipPlayer();
      });
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
// Input=internal runs the built-in engine. The ffmpeg modes spawn a supervised
// resident process that reads the URL directly; pipe instead spawns the entered
// producer argv and connects its stdout to ffmpeg's stdin. Both copy-remux to
// HLS or the selected SRT/UDP/custom output without transcoding.

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

let nm3u8dlreStatus = null;
let nm3u8dlreStatusPromise = null;

function fetchNM3U8DLREStatus() {
  if (!nm3u8dlreStatusPromise) {
    nm3u8dlreStatusPromise = request("/api/nm3u8dlre-status")
      .then((result) => { nm3u8dlreStatus = result; updatePipelineFieldVisibility(); return result; })
      .catch(() => { nm3u8dlreStatus = { available: false, installCommand: "", canAutoInstall: false }; });
  }
  return nm3u8dlreStatusPromise;
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
  const nmAvailable = Boolean(nm3u8dlreStatus?.available);
  document.querySelectorAll('#streamForm option[data-needs-nm3u8dlre]').forEach((option) => {
    option.disabled = !nmAvailable;
  });

  const inputMode = form.elements.inputMode.value;
  const outputMode = form.elements.outputMode.value;
  const targetField = $("#outputTargetField");
  targetField.classList.toggle("hidden", outputMode === "hls");
  $("#outputTargetHint").textContent = OUTPUT_TARGET_HINTS[outputMode] || "";

  const nmParamsField = $("#nm3u8dlreParamsField");
  if (nmParamsField) nmParamsField.classList.toggle("hidden", inputMode !== "nm3u8dlre");
  const pipeCommandField = $("#pipeCommandField");
  if (pipeCommandField) pipeCommandField.classList.toggle("hidden", inputMode !== "pipe");
  if (form.elements.pipeCommand) form.elements.pipeCommand.required = inputMode === "pipe";
  if (form.elements.url) form.elements.url.required = inputMode !== "pipe";

  const needsFfmpeg = (inputMode !== "internal" && inputMode !== "nm3u8dlre") || outputMode !== "hls";
  const notice = $("#ffmpegNotice");
  const installRow = $("#ffmpegInstallRow");
  if (!ffmpegStatus || available) {
    notice.classList.add("hidden");
    installRow.classList.add("hidden");
  } else {
    notice.classList.remove("hidden");
    notice.textContent = needsFfmpeg
      ? `ffmpeg isn't installed — required for the option${(inputMode !== "internal" && inputMode !== "nm3u8dlre") && outputMode !== "hls" ? "s" : ""} selected above.`
      : `ffmpeg isn't installed. Only Internal remuxer → HLS/Direct works without it.`;
    installRow.classList.toggle("hidden", !ffmpegStatus.canAutoInstall);
    if (!ffmpegStatus.canAutoInstall && ffmpegStatus.installCommand) {
      notice.textContent += ` Install it yourself: ${ffmpegStatus.installCommand}`;
    }
  }

  // Installed is not the same as capable. libavformat only builds the DASH
  // demuxer when configured with --enable-libxml2, and a good number of stock
  // builds leave it out — on those, an .mpd source cannot be opened at all and
  // ffmpeg fails in a way that reads as a broken stream rather than a missing
  // feature. Warn about the specific capability rather than a package name,
  // which is Homebrew-only trivia and wrong everywhere else.
  const capNotice = $("#ffmpegCapsNotice");
  if (capNotice) {
    const missing = [];
    const isDash = (form.elements.kind?.value || "mpd") === "mpd";
    if (available && ffmpegStatus.hasDash === false && isDash && needsFfmpeg) {
      missing.push("no DASH demuxer — this build was compiled without --enable-libxml2, so an .mpd input cannot be opened (and -cenc_decryption_key has nothing to decrypt). On Homebrew use ffmpeg-full; on Debian/Ubuntu a distribution ffmpeg normally has it");
    }
    if (available && ffmpegStatus.hasSrt === false && (outputMode === "srtServer" || outputMode === "udpSrt")) {
      missing.push("no srt:// protocol — this build lacks libsrt, so the SRT output modes cannot connect");
    }
    capNotice.classList.toggle("hidden", missing.length === 0);
    capNotice.textContent = missing.length
      ? `ffmpeg is installed${ffmpegStatus.version ? ` (${ffmpegStatus.version})` : ""} but ${missing.join("; ")}.`
      : "";
  }

  const nmNotice = $("#nm3u8dlreNotice");
  const nmInstallRow = $("#nm3u8dlreInstallRow");
  if (nmNotice && nmInstallRow) {
    if (!nm3u8dlreStatus || nmAvailable || inputMode !== "nm3u8dlre") {
      nmNotice.classList.add("hidden");
      nmInstallRow.classList.add("hidden");
    } else {
      nmNotice.classList.remove("hidden");
      nmNotice.textContent = `N_m3u8DL-RE isn't installed — required for the selected input mode.`;
      nmInstallRow.classList.toggle("hidden", !nm3u8dlreStatus.canAutoInstall);
      if (!nm3u8dlreStatus.canAutoInstall && nm3u8dlreStatus.installCommand) {
        nmNotice.textContent += ` Install it yourself: ${nm3u8dlreStatus.installCommand}`;
      }
    }
  }
}

let ffmpegInstallPollTimer = null;

function stopFfmpegInstallPoll() {
  clearInterval(ffmpegInstallPollTimer);
  ffmpegInstallPollTimer = null;
}

async function pollFfmpegInstallOutput(box = $("#ffmpegInstallOutput")) {
  try {
    const result = await request(`/api/logs?streamId=${encodeURIComponent("ffmpeg-install")}&limit=500`);
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
      if (currentView === "settings") await refreshSettingsFfmpegStatus();
    }
  } catch (error) {
    box.textContent = `Couldn't load install output: ${error.message || error}`;
  }
}

async function startFfmpegInstall(output) {
  output.classList.remove("hidden");
  output.textContent = "Starting installer…";
  try {
    await request("/api/ffmpeg-install", { method: "POST", body: "{}" });
    stopFfmpegInstallPoll();
    pollFfmpegInstallOutput(output);
    ffmpegInstallPollTimer = setInterval(() => pollFfmpegInstallOutput(output), 1000);
  } catch (error) {
    output.textContent = `Couldn't start install: ${error.message || error}`;
  }
}

$("#ffmpegInstallBtn").addEventListener("click", () => {
  startFfmpegInstall($("#ffmpegInstallOutput"));
});

let nm3u8dlreInstallPollTimer = null;

function stopNm3u8dlreInstallPoll() {
  clearInterval(nm3u8dlreInstallPollTimer);
  nm3u8dlreInstallPollTimer = null;
}

async function pollNm3u8dlreInstallOutput() {
  const box = $("#nm3u8dlreInstallOutput");
  try {
    const result = await request(`/api/logs?streamId=${encodeURIComponent("nm3u8dlre-install")}&limit=500`);
    const lines = (result.entries || []).slice().reverse().map((entry) => entry.message || entry.event).join("\n");
    const text = lines || "(no output yet)";
    if (box.textContent === text) return;
    const wasAtBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 20;
    box.textContent = text;
    if (wasAtBottom) box.scrollTop = box.scrollHeight;
    const latest = (result.entries || [])[0];
    if (latest && latest.event === "installExit") {
      stopNm3u8dlreInstallPoll();
      nm3u8dlreStatusPromise = null;
      await fetchNM3U8DLREStatus();
    }
  } catch (error) {
    box.textContent = `Couldn't load install output: ${error.message || error}`;
  }
}

$("#nm3u8dlreInstallBtn")?.addEventListener("click", async () => {
  $("#nm3u8dlreInstallOutput").classList.remove("hidden");
  $("#nm3u8dlreInstallOutput").textContent = "Starting…";
  try {
    await request("/api/nm3u8dlre-install", { method: "POST", body: "{}" });
  } catch (error) {
    $("#nm3u8dlreInstallOutput").textContent = `Couldn't start install: ${error.message || error}`;
    return;
  }
  stopNm3u8dlreInstallPoll();
  pollNm3u8dlreInstallOutput();
  nm3u8dlreInstallPollTimer = setInterval(pollNm3u8dlreInstallOutput, 1000);
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
  $("#forceProbeBtn").disabled = true;
  $("#detectStatus").textContent = "Detecting…";
  try {
    const proxy = form.elements.proxy.value || selectedProvider()?.proxy || "";
    const result = await request("/api/probe", {
      method: "POST",
      body: JSON.stringify({ url, proxy, headers: form.elements.manifestHeaders.value, forceIpv6: Boolean(selectedProvider()?.forceIpv6), rotateProxies: Boolean(selectedProvider()?.rotateProxies) }),
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
    $("#forceProbeBtn").disabled = false;
  }
}

$("#forceProbeBtn").addEventListener("click", () => {
  clearTimeout(autoDetectTimer);
  lastAutoDetectedUrl = "";
  detectSource();
});

// MARK: - Script actions
//
// One catalogue, rendered into both the provider grid (which actions the
// script implements) and the stream grid (this stream's override). Order and
// grouping follow the pipeline rather than the alphabet, so the list reads like
// the sequence it actually runs in. `wired: false` entries are stored and
// respected but never invoked yet — see ScriptAction.isWired in PanelServer.
const SCRIPT_ACTIONS = [
  { id: "login", label: "Login", hint: "Authenticate and persist a session." },
  { id: "pair", label: "Pair", hint: "Device-pairing flow — prints a code and waits." },
  { id: "channels", label: "Channels", hint: "Bulk-import channels as streams." },
  { id: "events", label: "Events", hint: "Bulk-import events as streams." },
  { id: "epg", label: "EPG", hint: "Fetch guide data (XMLTV or JSON), stored per provider." },
  { id: "start", label: "Start", hint: "Called when a stream starts." },
  { id: "stop", label: "Stop", hint: "Called when a stream stops." },
  { id: "manifest", label: "Session manifest", hint: "Fresh source URL, CDNs and headers on every start." },
  { id: "url", label: "URL processing", hint: "Rewrite a URL before it's fetched." },
  { id: "downloadmanifest", label: "Manifest download", hint: "The script fetches the manifest itself." },
  { id: "pssh", label: "PSSH parsing", hint: "Post-process the PSSH boxes before the licence call." },
  { id: "initparse", label: "Init parsing", hint: "Inspect the init segment for extra KIDs/PSSH." },
  { id: "cdm", label: "CDM keys", hint: "Return clear KID:KEY pairs." },
  { id: "heartbeat", label: "Heartbeat", hint: "Periodic ping to keep the session alive." },
  { id: "downloadinit", label: "Init download", hint: "Not wired yet — a subprocess per fetch needs a persistent worker.", wired: false },
  { id: "downloadmedia", label: "Segment download", hint: "Not wired yet — a subprocess per segment needs a persistent worker.", wired: false },
];

// Renders the grid into `container`, ticking whatever's in `selected`.
function renderScriptActions(container, selected) {
  const chosen = new Set(selected || []);
  container.innerHTML = "";
  for (const action of SCRIPT_ACTIONS) {
    const label = document.createElement("label");
    label.className = "check script-action" + (action.wired === false ? " unwired" : "");
    const box = document.createElement("input");
    box.type = "checkbox";
    box.value = action.id;
    box.checked = chosen.has(action.id);
    label.appendChild(box);
    label.appendChild(document.createTextNode(` ${action.label}`));
    const hint = document.createElement("span");
    hint.className = "field-hint sub";
    hint.textContent = action.hint;
    label.appendChild(hint);
    container.appendChild(label);
  }
}

// The ticked ids, in catalogue order so the stored array is stable.
function readScriptActions(container) {
  return Array.from(container.querySelectorAll("input[type=checkbox]"))
    .filter((box) => box.checked)
    .map((box) => box.value);
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
    const result = await request(`/api/logs?streamId=${encodeURIComponent("script:" + providerId)}&limit=500`);
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

// Import Channels/Events from the All Streams bulk bar — same backend action
// as the provider settings dialog's buttons. There's no dialog open here, so
// it renders the script's full output into its own live-scrolling terminal
// (#streamsGridImportOutput) below the bar, so when channels don't load you can
// read the script's actual output/error instead of guessing. A one-line status
// (#streamsGridImportStatus) summarises above it. Polling stops once the script
// reports it exited.
let gridImportPollTimer = null;

async function pollGridImportStatus(providerId) {
  const status = $("#streamsGridImportStatus");
  const term = $("#streamsGridImportOutput");
  try {
    const result = await request(`/api/logs?streamId=${encodeURIComponent("script:" + providerId)}&limit=500`);
    const entries = result.entries || [];
    // Full transcript, oldest→newest, into the terminal — same rendering as
    // the provider dialog's terminal (don't rewrite the DOM when unchanged, so
    // a selection/scroll survives; only auto-scroll if already at the bottom).
    const text = entries.slice().reverse().map((e) => e.message || e.event).join("\n") || "(no output yet)";
    if (term.textContent !== text) {
      const wasAtBottom = term.scrollHeight - term.scrollTop - term.clientHeight < 20;
      term.textContent = text;
      if (wasAtBottom) term.scrollTop = term.scrollHeight;
    }
    term.classList.remove("hidden");
    const latest = entries[0];
    if (latest) {
      status.textContent = latest.message || latest.event;
      status.classList.remove("hidden");
    }
    if (latest && latest.event === "scriptExit") {
      clearInterval(gridImportPollTimer);
      gridImportPollTimer = null;
      refresh();
    }
  } catch (error) {
    status.textContent = `Couldn't check import status: ${error.message || error}`;
    status.classList.remove("hidden");
  }
}

async function runProviderScriptForGrid(action) {
  const provider = state.providers.find((p) => p.id === streamsGridProviderId);
  if (!provider) { alert("Filter All Streams to a single provider first."); return; }
  if (!provider.scriptPath) { alert("This provider has no script configured — set one in Provider settings."); return; }
  const status = $("#streamsGridImportStatus");
  const term = $("#streamsGridImportOutput");
  status.classList.remove("hidden");
  status.textContent = `Starting ${action}…`;
  term.classList.remove("hidden");
  term.textContent = "Starting…";
  try {
    await request(`/api/providers/${provider.id}/script/${action}`, { method: "POST", body: "{}" });
  } catch (error) {
    status.textContent = `Couldn't start ${action}: ${error.message || error}`;
    term.textContent = `Couldn't start ${action}: ${error.message || error}`;
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
  form.elements.errorWebhookUrl.value = provider.errorWebhookUrl || "";
  form.elements.headers.value = provider.headers || "";
  form.elements.downloader.value = provider.downloader || "native";
  form.elements.downloaderParams.value = provider.downloaderParams || "";
  form.elements.forceIpv6.checked = Boolean(provider.forceIpv6);
  form.elements.rotateProxies.checked = Boolean(provider.rotateProxies);
  form.elements.segmentUrlParams.value = provider.segmentUrlParams || "";
  form.elements.inheritUrlParams.checked = Boolean(provider.inheritUrlParams);
  form.elements.scriptPath.value = provider.scriptPath || "";
  form.elements.scriptBind.value = provider.scriptBind || "";
  form.elements.scriptDoh.value = provider.scriptDoh || "";
  form.elements.scriptWorker.value = provider.scriptWorker || "";
  form.elements.accountSelectionMode.value = provider.accountSelectionMode || "fixed";
  renderScriptActions($("#providerScriptActions"), provider.scriptActions || []);
  $("#providerSessionDir").textContent = provider.scriptSessionDir || "—";
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
      proxy: form.elements.proxy.value, errorWebhookUrl: form.elements.errorWebhookUrl.value,
      headers: form.elements.headers.value, segmentUrlParams: form.elements.segmentUrlParams.value,
      downloader: form.elements.downloader.value, downloaderParams: form.elements.downloaderParams.value,
      forceIpv6: form.elements.forceIpv6.checked,
      rotateProxies: form.elements.rotateProxies.checked,
      inheritUrlParams: form.elements.inheritUrlParams.checked,
      scriptPath: form.elements.scriptPath.value, scriptBind: form.elements.scriptBind.value,
      scriptDoh: form.elements.scriptDoh.value, scriptWorker: form.elements.scriptWorker.value,
      scriptAccounts: scriptAccountsDraft, activeScriptAccountId: activeScriptAccountIdDraft,
      accountSelectionMode: form.elements.accountSelectionMode.value,
      scriptActions: readScriptActions($("#providerScriptActions")),
    }),
  });
}

$("#testProviderWebhookBtn").addEventListener("click", async () => {
  const button = $("#testProviderWebhookBtn");
  try {
    button.disabled = true;
    await saveProviderSettings();
    const provider = selectedProvider();
    if (!provider?.errorWebhookUrl) throw new Error("Enter a webhook URL first.");
    await request(`/api/providers/${provider.id}/webhook/test`, { method: "POST", body: "{}" });
    button.textContent = "Test queued";
    setTimeout(() => { button.innerHTML = '<span data-icon="play"></span>Send test notification'; applyIcons(button); }, 1800);
  } catch (error) {
    alert(`Couldn't send webhook test: ${error.message || error}`);
  } finally {
    button.disabled = false;
  }
});

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
let logsPaused = false;
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
  const detailText = [entry.url, entry.message].filter(Boolean).join(" · ");
  // Multi-line messages (a script's Python traceback, say) render as one
  // unreadable jumble in a plain span because HTML collapses newlines. Detect
  // them and lay them out in a pre-wrap block so each line stays on its own.
  const isMultiline = /\n/.test(detailText);
  const detail = isMultiline
    ? `<pre class="log-detail-multiline">${escapeHtml(detailText)}</pre>`
    : `<span class="log-detail" title="${escapeAttr(detailText)}">${escapeHtml(detailText)}</span>`;
  return `
    <div class="log-row ${entry.level === "error" ? "log-error" : ""} ${isMultiline ? "log-row-multiline" : ""}">
      <span class="log-time">${time}</span>
      <span class="log-event">${escapeHtml(entry.event)}</span>
      ${detail}
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
  // Panel-level events (server start, auth, stream start/stop, supervisor) are
  // recorded under a synthetic "__panel__" id — surface them here too.
  const panelOption = `<option value="__panel__" ${streamId === "__panel__" ? "selected" : ""}>Panel (server events)</option>`;
  $("#logStreamFilter").innerHTML = `<option value="">All streams</option>${panelOption}${options.join("")}${scriptOptions.join("")}`;
  $("#logLevelFilter").value = logLevelFilter;
  try {
    const params = new URLSearchParams({ limit: "500" });
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
  updateLogStreamToggleBtn();
}

function updateLogStreamToggleBtn() {
  const streamId = $("#logStreamFilter").value;
  const btn = $("#toggleLogStreamRunBtn");
  if (!btn) return;
  if (!streamId || streamId === "__panel__" || streamId.startsWith("script:")) {
    btn.disabled = true;
    btn.innerHTML = `<span data-icon="play"></span>Start Stream`;
    btn.className = "ghost";
    applyIcons(btn);
    return;
  }
  const stream = state.providers.flatMap(p => p.streams).find(s => s.id === streamId);
  if (!stream) {
    btn.disabled = true;
    return;
  }
  btn.disabled = false;
  const running = Boolean(stream.running);
  btn.className = running ? "ghost danger-ghost" : "ghost success-ghost";
  btn.innerHTML = `<span data-icon="${running ? "stop" : "play"}"></span>${running ? "Stop Stream" : "Start Stream"}`;
  applyIcons(btn);
}

$("#refreshLogsBtn").addEventListener("click", loadLogs);
$("#toggleLogStreamRunBtn")?.addEventListener("click", async () => {
  const streamId = $("#logStreamFilter").value;
  const stream = state.providers.flatMap(p => p.streams).find(s => s.id === streamId);
  if (stream) {
    await toggleStreamRun(stream.id, stream.running);
    updateLogStreamToggleBtn();
  }
});
$("#pauseLogsBtn").addEventListener("click", (event) => {
  logsPaused = !logsPaused;
  const button = event.currentTarget;
  button.innerHTML = logsPaused ? '<span data-icon="play"></span>Resume' : '<span data-icon="stop"></span>Pause';
  button.classList.toggle("active", logsPaused);
  applyIcons(button);
  // Restart or stop the poll to match.
  clearInterval(logsPollTimer);
  logsPollTimer = null;
  if (!logsPaused && currentView === "logs") {
    loadLogs();
    logsPollTimer = setInterval(loadLogs, 2000);
  }
});
$("#clearLogsBtn").addEventListener("click", async () => {
  const streamId = $("#logStreamFilter").value;
  const label = streamId ? "this stream's" : "all";
  if (!confirm(`Clear ${label} logs? This removes the in-memory log lines shown here (saved daily files are kept).`)) return;
  const params = new URLSearchParams();
  if (streamId) params.set("streamId", streamId);
  try {
    await request(`/api/logs?${params.toString()}`, { method: "DELETE" });
    loadLogs();
  } catch (error) {
    alert(`Couldn't clear logs: ${error.message || error}`);
  }
});
$("#providerSearch").addEventListener("input", (event) => {
  providerSearchQuery = event.currentTarget.value;
  renderProviders();
  applyIcons();
});
$("#streamsGridProviderFilter").addEventListener("change", (event) => {
  streamsGridProviderId = event.currentTarget.value;
  renderStreamsGrid();
  // The provider filter is part of this view's address; replace rather than
  // push so flicking through providers does not fill up the back button.
  syncUrl("grid", true);
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
$("#logStreamFilter").addEventListener("change", () => {
  syncUrl("logs", true);
  loadLogs();
});
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
    // `port` is the port actually bound; `storedPort` is the saved preference.
    // Show the saved value (it is what this form edits) but say so when the
    // running server is on a different one, which is exactly the state that
    // used to make this field look wrong.
    const stored = settings.storedPort ?? settings.port;
    $("#portForm").elements.port.value = stored;
    const note = $("#portRunningNote");
    if (note) {
      const differs = settings.port && Number(stored) !== Number(settings.port);
      note.textContent = differs ? `currently listening on ${settings.port} — restart to apply ${stored}` : "";
      note.classList.toggle("hidden", !differs);
    }
    $("#portForm").elements.bindAddress.value = settings.bindAddress || "";
    $("#portForm").elements.trustedProxies.value = settings.trustedProxies || "";
  } catch (error) { /* ignore */ }
  await refreshServiceStatus();
  await refreshSettingsFfmpegStatus();
  await refreshUserList();
}

async function refreshSettingsFfmpegStatus() {
  const status = $("#settingsFfmpegStatus");
  const button = $("#settingsFfmpegInstallBtn");
  if (!status || !button) return;
  const result = await fetchFfmpegStatus();
  if (result.available) {
    status.textContent = `Installed${result.version ? ` — ${result.version}` : ""}${result.hasDash === false ? " · this build has no DASH demuxer" : ""}`;
    button.classList.add("hidden");
  } else {
    status.textContent = "FFmpeg is not installed. The internal remuxer still works without it.";
    button.classList.toggle("hidden", !result.installCommand);
  }
}

$("#settingsFfmpegInstallBtn")?.addEventListener("click", () => {
  startFfmpegInstall($("#settingsFfmpegInstallOutput"));
});

// Service panel. The port setting says "takes effect after restart", so the
// restart has to be reachable from the same page; and a fresh install often has
// no unit at all — just a binary someone started by hand, which does not survive
// a reboot — so offer to create one from the paths already in use.
async function refreshServiceStatus() {
  const panel = $("#servicePanel");
  if (!panel) return;
  let s;
  try { s = await request("/api/service"); } catch { panel.classList.add("hidden"); return; }
  if (!s.systemdAvailable) { panel.classList.add("hidden"); return; }
  panel.classList.remove("hidden");

  const bits = [];
  bits.push(s.unitInstalled ? `${s.unitName} installed` : "no unit installed");
  if (s.unitInstalled) {
    bits.push(s.active ? "running" : "not running");
    bits.push(s.enabled ? "starts on boot" : "does not start on boot");
  }
  $("#serviceStatus").textContent = bits.join(" · ");

  const note = $("#serviceNote");
  if (!s.canManage) {
    note.textContent = "Managing the service needs root; these buttons are disabled.";
  } else if (s.unitInstalled && s.execStart) {
    note.textContent = `ExecStart: ${s.execStart}`;
  } else if (!s.unitInstalled) {
    note.textContent = `Would install a unit running ${s.selfPath || "this binary"} from ${s.workingDir || "the current directory"}.`;
  } else {
    note.textContent = "";
  }

  $("#serviceRestartBtn").disabled = !s.canManage || !s.unitInstalled;
  $("#serviceInstallBtn").disabled = !s.canManage;
  $("#serviceInstallBtn").textContent = s.unitInstalled ? "Reinstall systemd service" : "Install systemd service";
}

$("#serviceRestartBtn")?.addEventListener("click", async (event) => {
  if (!confirm("Restart the panel service now? The panel will be briefly unreachable and any running stream is interrupted.")) return;
  const button = event.currentTarget;
  button.disabled = true;
  try {
    await request("/api/service/restart", { method: "POST" });
    $("#serviceNote").textContent = "Restarting — reconnecting…";
    // The server goes away mid-restart, so poll /ping until it answers again
    // rather than leaving the page looking hung.
    for (let i = 0; i < 30; i++) {
      await new Promise((r) => setTimeout(r, 1000));
      try {
        const res = await fetch("/ping", { cache: "no-store" });
        if (res.ok) { $("#serviceNote").textContent = "Back up."; await refreshServiceStatus(); return; }
      } catch { /* still down */ }
    }
    $("#serviceNote").textContent = "Still not answering — check `systemctl status` on the host.";
  } catch (error) {
    $("#serviceNote").textContent = `Restart failed: ${error.message || error}`;
    button.disabled = false;
  }
});

$("#serviceInstallBtn")?.addEventListener("click", async () => {
  const form = $("#portForm");
  const port = Number(form.elements.port.value) || undefined;
  const bindAddress = form.elements.bindAddress.value || "";
  if (!confirm("Write a systemd unit for this install and enable it at boot?")) return;
  try {
    await request("/api/service/install", { method: "POST", body: JSON.stringify({ port, bindAddress }) });
    $("#serviceNote").textContent = "Unit installed and enabled.";
    await refreshServiceStatus();
  } catch (error) {
    $("#serviceNote").textContent = `Install failed: ${error.message || error}`;
  }
});

async function refreshUserList() {
  const list = $("#userList");
  try {
    const result = await request("/api/users");
    const users = result.users || [];
    if (!users.length) {
      list.className = "key-list empty-state";
      list.textContent = "No accounts found.";
      return;
    }
    // The last admin can't be removed (the server refuses too), but viewers
    // alongside it can — so the button depends on the admin count, not the
    // account count.
    const admins = users.filter((user) => (user.role || "admin") === "admin").length;
    list.className = "key-list";
    list.innerHTML = users.map((user) => {
      const role = user.role || "admin";
      const removable = role !== "admin" || admins > 1;
      return `
      <div class="key-row">
        <div class="key-top">
          <strong>${escapeHtml(user.username)}</strong>
          <span class="pill">${role === "viewer" ? "Read-only" : "Admin"}</span>
          ${removable ? `<button type="button" class="danger" data-user-id="${escapeAttr(user.id)}"><span data-icon="trash"></span>Remove</button>` : ""}
        </div>
        <div class="key-meta">Created ${new Date(user.createdAt).toLocaleString()}</div>
      </div>
    `;
    }).join("");
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
    body: JSON.stringify({
      port: Number(form.elements.port.value),
      bindAddress: form.elements.bindAddress.value,
      trustedProxies: form.elements.trustedProxies.value,
    }),
  });
  $("#viewMeta").textContent = "Saved — restart the server for it to take effect.";
});

$("#userForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  await request("/api/users", {
    method: "POST",
    body: JSON.stringify({
      username: form.elements.username.value,
      password: form.elements.password.value,
      role: form.elements.role.value,
    }),
  });
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
  if (currentView !== "monitor" && currentView !== "server") return;
  const global = payload.global || {};
  const globalInput = payload.globalInput || {};
  const system = payload.system || {};
  const cpuPercent = Number(system.cpuPercent || 0);
  const loadAverage = Number(system.loadAverage || 0);
  const memUsed = Number(system.memoryUsedBytes || 0);
  const memTotal = Number(system.totalMemoryBytes || 1);

  $("#serverTiles").innerHTML = [
    statTile("CPU", `${cpuPercent.toFixed(1)}%`, `${system.cpuModel || ""} · ${system.coreCount || "?"} cores · peak ${Number(system.peakCPUPercent || 0).toFixed(1)}%`),
    statTile("Memory", formatBytes(memUsed), `of ${formatBytes(memTotal)} · peak ${formatBytes(system.peakMemoryBytes)}`),
    statTile("Disk", formatBytes((system.diskTotalBytes || 0) - (system.diskAvailableBytes || 0)), `of ${formatBytes(system.diskTotalBytes)} used`),
    statTile("Uptime", formatUptime(system.uptimeSeconds), system.osVersion || ""),
    statTile("Build", appVersionLabel(), appVersionSubtitle()),
    statTile("All-time served", formatBytes(global.allTimeBytes), `${formatBytes(globalInput.allTimeBytes)} pulled from origins`),
  ].join("");

  if (currentView === "server") return;

  // Out (↑) is what viewers are taking; in (↓) is what the engines are pulling
  // from the origins, which keeps running with nobody connected.
  $("#headlineTiles").innerHTML = [
    statTile("Live bandwidth", `↑ ${formatBytesPerSecond(global.bytesPerSecond)}`, `↓ ${formatBytesPerSecond(globalInput.bytesPerSecond)} from origins`),
    statTile("Active clients", String(global.activeClients || 0)),
    statTile("All-time served", formatBytes(global.allTimeBytes), `${formatBytes(globalInput.allTimeBytes)} pulled`),
    statTile("Connected IPs", String(new Set((payload.connections || []).map((c) => c.clientIP).filter(Boolean)).size)),
  ].join("");

  const streams = payload.streams || {};
  const streamIds = Object.keys(streams).sort((a, b) => {
    const nameA = findStreamName(a) || a;
    const nameB = findStreamName(b) || b;
    return nameA.localeCompare(nameB);
  });
  const container = $("#streamTiles");
  if (!streamIds.length) {
    container.className = "tile-grid empty-state";
    container.textContent = "No streams yet.";
  } else {
    container.className = "tile-grid";
    container.innerHTML = streamIds.map((id) => {
      const info = streams[id];
      const name = findStreamName(id) || id;
      const bw = info.bandwidth || info;
      const inBps = info.inputBytesPerSecond ?? (info.inputBandwidth || {}).bytesPerSecond;
      return statTile(name, `${info.activeClients || 0} active`,
                      `↓ ${formatBytesPerSecond(inBps)} · ↑ ${formatBytesPerSecond(bw.bytesPerSecond)}`);
    }).join("");
  }

  lastConnections = payload.connections || [];
  renderConnectionsTable();
}

function repaintMonitorIfActive() {
  if ((currentView === "server" || currentView === "monitor") && lastMetricsPayload) applyMetrics(lastMetricsPayload);
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
  let status;
  try {
    const response = await fetch("/api/auth/status", { cache: "no-store" });
    status = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(status.error || `HTTP ${response.status}`);
    $("#authError").classList.add("hidden");
  } catch (error) {
    authenticated = false;
    $("#authSubtitle").textContent = "Can't reach the server";
    $("#authForm").dataset.mode = "retry";
    $("#authSubmit").textContent = "Try again";
    $("#authError").textContent = String(error.message || error);
    $("#authError").classList.remove("hidden");
    showAuthScreen();
    return false;
  }
  authenticated = Boolean(status.authenticated);
  if (status.needsSetup) {
    $("#authSubtitle").textContent = "Create the admin account";
    $("#authForm").dataset.mode = "setup";
    $("#authSubmit").textContent = "Create account";
    showAuthScreen();
    return false;
  }
  $("#authForm").dataset.mode = "login";
  if (!authenticated) {
    $("#authSubtitle").textContent = "Sign in";
    $("#authSubmit").textContent = "Sign in";
    showAuthScreen();
    return false;
  }
  const role = status.role || "admin";
  // A viewer's writes are refused by the server with a 403; say so up front
  // rather than letting them find out one failed save at a time.
  $("#whoami").textContent = status.username
    ? `Signed in as ${status.username}${role === "viewer" ? " (read-only)" : ""}`
    : "";
  document.body.dataset.role = role;
  showApp();
  return true;
}

$("#authForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  const button = $("#authSubmit");
  if (button.disabled) return;
  $("#authError").classList.add("hidden");
  button.disabled = true;
  try {
    const statusResponse = await fetch("/api/auth/status", { cache: "no-store" });
    const status = await statusResponse.json().catch(() => ({}));
    if (!statusResponse.ok) throw new Error(status.error || `HTTP ${statusResponse.status}`);
    const settingUp = Boolean(status.needsSetup);
    const path = settingUp ? "/api/auth/setup" : "/api/auth/login";
    form.dataset.mode = settingUp ? "setup" : "login";
    button.textContent = settingUp ? "Creating…" : "Signing in…";
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
    await boot();
  } catch (error) {
    $("#authError").textContent = String(error.message || error);
    $("#authError").classList.remove("hidden");
  } finally {
    button.disabled = false;
    button.textContent = form.dataset.mode === "setup" ? "Create account"
      : form.dataset.mode === "retry" ? "Try again" : "Sign in";
  }
});

$("#signOutBtn").addEventListener("click", async (event) => {
  const button = event.currentTarget;
  if (button.disabled) return;
  button.disabled = true;
  try {
    await fetch("/api/auth/logout", { method: "POST" });
  } catch {
    // Still return to the sign-in screen when the server disappears mid-request.
  } finally {
    authenticated = false;
    showAuthScreen();
    button.disabled = false;
    // Refresh the auth mode and labels. The polling/event infrastructure stays
    // alive and boot() will reconnect it after the next successful sign-in.
    await checkAuth();
  }
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
$("#showScriptingBtn").addEventListener("click", () => {
  const group = $("#scriptingGroup");
  group.classList.toggle("hidden");
  if (!group.classList.contains("hidden")) $("#streamForm").elements.useCdm.focus();
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
$("#scriptLoadEpgBtn").addEventListener("click", () => runProviderScript("epg"));
$("#overrideScriptActions").addEventListener("change", (event) => {
  const grid = $("#streamScriptActions");
  grid.classList.toggle("hidden", !event.target.checked);
  if (event.target.checked && !readScriptActions(grid).length) {
    renderScriptActions(grid, selectedProvider()?.scriptActions || []);
  }
});
$("#clearScriptSessionBtn").addEventListener("click", async () => {
  const provider = selectedProvider();
  if (!provider) return;
  if (!confirm("Delete this provider's stored session and cookies? The script will have to log in again.")) return;
  try {
    await request(`/api/providers/${provider.id}/script/clear-session`, { method: "POST" });
    alert("Session store cleared.");
  } catch (error) {
    alert(`Couldn't clear the session store: ${error.message || error}`);
  }
});
$("#toggleScriptOutputBtn").addEventListener("click", () => {
  $("#scriptOutputBox").classList.toggle("hidden");
  updateScriptOutputToggleLabel();
});
$("#copyBtn").addEventListener("click", async (event) => {
  const link = $("#playLink").value;
  if (!link) return;
  if (await copyToClipboard(link)) flashButtonCopied(event.currentTarget);
  else window.prompt("Copy the output URL:", link);
});
$("#copyDirectBtn").addEventListener("click", async (event) => {
  const link = $("#directLink").dataset.full;
  if (!link) return;
  if (await copyToClipboard(link)) flashButtonCopied(event.currentTarget);
  else window.prompt("Copy the URL:", link);
});
$("#qualitySelect").addEventListener("change", (event) => {
  if (hls) {
    const level = Number(event.currentTarget.value);
    hls.currentLevel = level;
    if (level >= 0) hls.nextLevel = level;
  }
});
$("#audioTrackSelect").addEventListener("change", (event) => {
  if (hls) hls.audioTrack = Number(event.currentTarget.value);
});
document.querySelectorAll(".tab").forEach((button) => button.addEventListener("click", () => switchTab(button.dataset.tab)));
document.querySelectorAll(".nav-btn").forEach((button) => button.addEventListener("click", () => switchView(button.dataset.view)));

async function runStreamScriptAction(action, button) {
  const stream = selectedStream();
  if (!stream) return;
  const provider = selectedProvider();
  if (!provider) return;
  const box = $("#streamScriptOutputBox");
  if (!box) return;
  box.classList.remove("hidden");
  box.textContent = "Running...";
  button.disabled = true;
  try {
    const result = await request(`/api/providers/${provider.id}/script/run`, {
      method: "POST",
      body: JSON.stringify({ action, streamId: stream.id })
    });
    box.textContent = result.output || "No output";
  } catch (err) {
    box.textContent = `Error: ${err.message}`;
  } finally {
    button.disabled = false;
  }
}

$("#scriptRunPsshBtn")?.addEventListener("click", (e) => runStreamScriptAction("pssh", e.currentTarget));
$("#scriptRunManifestBtn")?.addEventListener("click", (e) => runStreamScriptAction("downloadmanifest", e.currentTarget));
$("#scriptRunMediaBtn")?.addEventListener("click", (e) => runStreamScriptAction("downloadmedia", e.currentTarget));

applyIcons();
const savedTheme = localStorage.getItem("restreamair-theme");
if (savedTheme) applyTheme(savedTheme);
else applyThemeIcon();
updateKindVisibility();

if ("serviceWorker" in navigator) {
  navigator.serviceWorker.register("/sw.js").catch(() => {});
}

async function boot() {
  // Coalesce simultaneous boot attempts (for example Enter plus a button
  // click), but do not permanently short-circuit here. A signed-out or expired
  // session must be able to refresh state and reconnect SSE after signing in
  // again without requiring a page reload.
  if (bootPromise) return bootPromise;
  bootPromise = (async () => {
    const ok = await checkAuth();
    if (!ok) return false;

    if (!bootStarted) {
      bootStarted = true;
      document.querySelectorAll(".stream-form label, #providerChips").forEach((el) => el.classList.add("skeleton"));
      setInterval(() => refresh().catch(() => {}), 4000);
    }

    try {
      // A poll sent with the expired cookie may still be in flight while the
      // login request succeeds. Let that stale request finish before starting
      // the authenticated refresh; otherwise refresh() would coalesce onto its
      // 401 and put the sign-in screen straight back.
      if (refreshPromise) await refreshPromise.catch(() => {});
      await refresh();
      authenticated = true;
      showApp();
      document.querySelectorAll(".skeleton").forEach((el) => el.classList.remove("skeleton"));
      // Once state exists, open whichever view the address names. Deferred to
      // here because the per-view loaders (logs, settings) need state and an
      // authenticated session, and because the log/provider filters are only
      // populated after the first render.
      applyRoute();
      connectEvents();
      return true;
    } catch (error) {
      document.querySelectorAll(".skeleton").forEach((el) => el.classList.remove("skeleton"));
      $("#statusBox").textContent = String(error.message || error);
      return false;
    }
  })();
  try {
    return await bootPromise;
  } finally {
    bootPromise = null;
  }
}

boot();
