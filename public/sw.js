const CACHE_NAME = "restreamair-shell-v56";
const SHELL_FILES = [
  "/",
  "/app.js",
  "/styles.css",
  "/hls.min.js",
  "/manifest.webmanifest",
  "/icons/icon-192.png",
  "/icons/icon-512.png",
];

// Everything the server serves that is not the panel shell. These must reach
// the network untouched: the navigate branch below answers ANY navigation from
// the cached "/" entry, so a media path missing from this list is handed
// index.html instead of the stream — clicking a /direct/… or /download/… link
// downloaded the app shell — and the generic handler underneath it would try to
// put an endless byte tail into Cache Storage.
const PASSTHROUGH_PREFIXES = [
  "/api/",
  "/play/",
  "/restream/",
  "/direct/",
  "/download/",
  "/source/",
  "/ping",
  // The Xtream-compatible client API and its live paths.
  "/live/",
  "/player_api.php",
  "/get.php",
  "/xmltv.php",
];

self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => cache.addAll(SHELL_FILES)).then(() => self.skipWaiting())
  );
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((key) => key !== CACHE_NAME).map((key) => caches.delete(key)))
    ).then(() => self.clients.claim())
  );
});

self.addEventListener("fetch", (event) => {
  const url = new URL(event.request.url);
  if (event.request.method !== "GET" || url.origin !== self.location.origin) return;
  if (PASSTHROUGH_PREFIXES.some((prefix) => url.pathname.startsWith(prefix))) return;

  // Every panel view has its own address now (/logs, /streams?provider=…), and
  // all of them are the same index.html. Answer navigations from the one cached
  // "/" entry rather than letting the generic handler below store a separate
  // copy of the shell per path — and per query string, since caches.match keys
  // on the full URL.
  //
  // Network first, cache as the offline fallback. Cache-first meant a browser
  // kept the app.js it had until someone remembered to bump CACHE_NAME — the
  // server could pull a new front-end and nobody would see it.
  if (event.request.mode === "navigate") {
    event.respondWith(networkFirst(new Request("/"), event.request));
    return;
  }
  event.respondWith(networkFirst(event.request, event.request));
});

// Fetch `request` and refresh the cached copy under `key`; when the network is
// unreachable, answer from the cache instead.
function networkFirst(key, request) {
  return fetch(request).then((response) => {
    if (response.ok) {
      const copy = response.clone();
      caches.open(CACHE_NAME).then((cache) => cache.put(key, copy));
    }
    return response;
  }).catch(() => caches.match(key).then((cached) => cached || Response.error()));
}
