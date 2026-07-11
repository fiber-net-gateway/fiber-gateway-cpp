// Example: inspect the request and build a custom response.
// Demonstrates req.getQuery (with and without a name), req.getHeader, req.getCookie,
// req.getUri, and resp.setHeader / resp.addHeader / resp.addCookie / resp.sendJson.
//
// GET /inspect?a=1&b=2  with headers  X-Test: hi   and  Cookie: session=xyz
// -> 200 application/json, plus response headers and a Set-Cookie.

let q = req.getQuery();
let headers = req.getHeader();
let cookies = req.getCookie();

resp.setHeader("X-Served-By", "script");
resp.addHeader("X-Debug-Path", req.getPath());
resp.addCookie({
    name: "trace",
    value: "abc",
    path: "/",
    httpOnly: true,
    sameSite: "Lax"
});

resp.sendJson(200, {
    uri: req.getUri(),
    queryStr: req.getQueryStr(),
    qa: q.a,
    qb: q.b,
    xTest: req.getHeader("x-test"),
    session: cookies.session,
    host: headers.host
});
