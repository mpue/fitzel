// fitzel-coi.js -- makes the page of an exported fitzel game cross-origin
// isolated on hosts that cannot be told to send the headers for it.
//
// The engine runs on several threads (WebAssembly with SharedArrayBuffer), and
// browsers allow that only on a page served with
//   Cross-Origin-Opener-Policy: same-origin
//   Cross-Origin-Embedder-Policy: require-corp
// A server you control should send those itself (serve.py does). On static
// hosting that cannot -- GitHub Pages, a plain web space -- index.html installs
// this service worker, which adds them to every response of the page, and
// reloads once under it.
self.addEventListener("install", function () { self.skipWaiting(); });
self.addEventListener("activate", function (e) { e.waitUntil(self.clients.claim()); });

self.addEventListener("fetch", function (e) {
  var req = e.request;
  // Chrome rejects this combination outside same-origin requests.
  if (req.cache === "only-if-cached" && req.mode !== "same-origin") return;
  e.respondWith(fetch(req).then(function (res) {
    if (res.status === 0) return res;   // opaque: nothing to add headers to
    var h = new Headers(res.headers);
    h.set("Cross-Origin-Opener-Policy", "same-origin");
    h.set("Cross-Origin-Embedder-Policy", "require-corp");
    h.set("Cross-Origin-Resource-Policy", "cross-origin");
    return new Response(res.body, { status: res.status, statusText: res.statusText, headers: h });
  }));
});
