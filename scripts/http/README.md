# HTTP header-line parser code generation

The complete incremental HTTP/1 header-line parser is described as an LLParse
graph in `generate_header_line_parser.ts`. It covers the header name, colon,
leading and trailing spaces, value, line terminator, header-block terminator,
invalid input, and every buffer boundary. Name and value spans are delivered to
C++ callbacks for lowercase hashing and pointer bookkeeping.

LLParse generates the state persistence, lookup tables, and target-specific
SIMD scans. Its generated C implementation is compiled as C++ and committed
under `src/http/generated/`.

Regenerate and verify it with:

```bash
cd scripts/http
npm ci
npm run generate
npm run check
```

The dependency versions are locked to the same `llparse` release used by the
analyzed llhttp 9.4.2 source.
