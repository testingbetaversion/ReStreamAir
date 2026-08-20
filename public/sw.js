const CACHE_NAME = "restreamair-shell-v32";
const SHELL_FILES = [
  "/",
  "/app.js",
  "/styles.css",
  "/hls.min.js",
  "/manifest.webmanifest",
  "/icons/icon-192.png",
  "/icons/icon-512.png",
];

const PASSTHROUGH_PREFIXES = ["/api/", "/play/", "/proxy/", "/restream/"];

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
  if (event.request.mode === "navigate") {
    event.respondWith(
      caches.match("/").then((cached) => cached || fetch(event.request))
    );
    return;
  }

  event.respondWith(
    caches.match(event.request).then((cached) => {
      if (cached) return cached;
      return fetch(event.request).then((response) => {
        const copy = response.clone();
        caches.open(CACHE_NAME).then((cache) => cache.put(event.request, copy));
        return response;
      }).catch(() => cached);
    })
  );
});
