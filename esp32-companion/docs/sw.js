// FR34 Cooler PWA – service worker
// Caches app shell for offline use after first visit.
// Bump CACHE version string to force clients to fetch fresh assets.
const CACHE = 'fr34-ble-v1';

// Resources to pre-cache on install.
// Vue CDN is attempted opportunistically — if offline at install time
// it will be cached lazily on first fetch.
const APP_SHELL = [
  './',
  './manifest.json',
  './icon.svg'
];
const VUE_CDN = 'https://unpkg.com/vue@3/dist/vue.global.prod.js';

self.addEventListener('install', event => {
  event.waitUntil(
    caches.open(CACHE).then(cache =>
      cache.addAll(APP_SHELL)
        .then(() => cache.add(VUE_CDN).catch(() => { /* offline at install time – cached lazily later */ }))
    )
  );
  self.skipWaiting();
});

self.addEventListener('activate', event => {
  // Remove stale caches from previous versions
  event.waitUntil(
    caches.keys()
      .then(keys => Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k))))
  );
  self.clients.claim();
});

self.addEventListener('fetch', event => {
  // Only handle GET; let POST/PUT pass through
  if (event.request.method !== 'GET') return;

  event.respondWith(
    caches.match(event.request).then(cached => {
      if (cached) return cached;
      return fetch(event.request).then(response => {
        // Cache successful responses for app shell and Vue CDN
        if (response.ok) {
          const url = event.request.url;
          const isAppShell = url.startsWith(self.location.origin);
          const isVue      = url.includes('unpkg.com');
          if (isAppShell || isVue) {
            const clone = response.clone();
            caches.open(CACHE).then(cache => cache.put(event.request, clone));
          }
        }
        return response;
      });
    })
  );
});
