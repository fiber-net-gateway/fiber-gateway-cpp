// Example: a JSON health endpoint.
// Demonstrates resp.sendJson and req.getMethod.
// GET /health -> { "status": "ok", "method": "GET", "ts": 1 }
resp.sendJson(200, {
    status: "ok",
    method: req.getMethod(),
    ts: 1
});
