// Example: route-variable constants ($path/$query/$header/$cookie/$req).
//
// Unlike req.getHeader(name) etc. (function calls), the $namespace constants are resolved
// at script compile time and read directly from the request at runtime:
//   $path.<name>   - a path variable captured by the location's route pattern (e.g. /api/:id
//                    makes $path.id available). Referencing a name the route does not capture
//                    is a *compile-time* error: the script fails to load.
//   $query.<key>   - a query parameter (case-sensitive).
//   $header.<key>  - a request header (case-insensitive, '-' matches '_'); e.g.
//                    $header.x_forwarded_for reads X-Forwarded-For.
//   $cookie.<key>  - a request cookie (same normalization as $header).
//   $req.<field>   - one of uri / method / path / query (fixed set; unknown = compile error).
//
// Absent values resolve to null (not an error).
//
// Mount under a route with a path variable, e.g.:
//   location /api/:id { script_file conf/scripts/vars.js; }
// and GET /api/42?src=web  with  X-Forwarded-For: 1.2.3.4  and  Cookie: session=abc

resp.sendJson(200, {
    id: $path.id,
    src: $query.src,
    clientIp: $header.x_forwarded_for,
    session: $cookie.session,
    uri: $req.uri,
    method: $req.method,
    path: $req.path,
    queryStr: $req.query
});
