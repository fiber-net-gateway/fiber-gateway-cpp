// Example: echo the request body as JSON.
// Demonstrates req.readJson (async), object property access, and resp.sendJson.
// POST /echo -> the parsed JSON body, augmented with the request path.
let body = req.readJson();
resp.sendJson(200, {
    echoed: body,
    path: req.getPath()
});
