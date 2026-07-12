// Example: issue upstream HTTP requests from a script.
// Demonstrates the `directive svc = http "<target>";` binding and its svc.request / svc.proxyPass
// calls (the host is bound once at compile time; the call options only carry path/query/headers).
//
// Requires an upstream block named `backend` (or a reachable ad-hoc URL) and a
// `connection_pool { keepalive_size N; ... }` block under `http` for keepalive reuse.

// directive binds the name `svc` to a fixed upstream target (a named upstream, or an
// http(s)://host[:port] URL). The target is resolved once at compile time; svc.request /
// svc.proxyPass then use it without re-specifying the target each call.
directive svc = http "@backend";

// POST /proxy  ->  proxy the inbound request body to the `backend` upstream and stream its
// response back to the client. Returns the upstream status code.
if (req.getMethod() == "POST") {
    return svc.proxyPass({responseHeaders: {"X-Proxied-By": "script"}});
}

// Otherwise: issue a fresh upstream GET and surface {status, headers, body} to the caller.
let r = svc.request({path: "/items", includeHeaders: true});
resp.sendJson(200, {
    upstreamStatus: r.status,
    contentType: r.headers["Content-Type"],
    bodyLength: r.body
});
