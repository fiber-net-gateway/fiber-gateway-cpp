# TLS and QUIC fuzzing

libFuzzer harnesses for the in-tree TLS stack (`src/tls/`) and QUIC transport
(`src/quic/`), built with ASan + UBSan. Off by default; a fuzz build is its
own build tree.

## TLS

| Harness | Input | Reaches |
|---|---|---|
| `tls_handshake_codec_fuzzer` | `[selector][message body]` | every handshake message decoder, plus the key-share / PSK identity / binder / extension helpers applied to decoded views |
| `tls_server_engine_fuzzer` | `[control][client byte stream]` | record framing and reassembly, ClientHello decode, version / suite / group / ALPN negotiation, HRR, ticket lookup and PSK binder verification, 0-RTT windows, mTLS, the 1.2 flight |
| `tls_client_engine_fuzzer` | `[control][server byte stream]` | ServerHello / HRR processing, PSK acceptance, and the plaintext 1.2 server flight: certificate chain parsing and verification, ServerKeyExchange, CertificateRequest |
| `tls_connection_fuzzer` | `[config][records...]` | the post-handshake connection with records **sealed under the peer's real keys** (the harness does the sealing): inner content types, 1.3 padding, alerts, NewSessionTicket / KeyUpdate reassembly, record_overflow, the owed-KeyUpdate write path |

## QUIC

| Harness | Input | Reaches |
|---|---|---|
| `quic_codec_fuzzer` | `[selector][bytes]` | every frame decoder per encryption level and receiver role, transport parameters, long/short packet headers, Version Negotiation, Retry integrity tags, address tokens, packet-number decoding. Parsed frames and parameters are **re-encoded and re-parsed**: encoder/decoder drift fails |
| `quic_reassembly_fuzzer` | `[config][ops...]` | CRYPTO reassembly (`QuicDataReassembler`) and stream receive (`QuicStreamRecvQueue`) checked **against a reference model**: delivered bytes, first-writer-wins overlaps, RFC 9000 4.5 final-size and flow-control rules, storage-budget leaks |
| `quic_connection_fuzzer` | `[config][records...]` | an established client or server connection fed 1-RTT packets **sealed under the peer's real keys**: streams and flow control, ACK/loss/PTO against packets our side really sent (the real build/commit send path, minus the socket), NEW/RETIRE_CONNECTION_ID, path validation and migration, peer key updates, HANDSHAKE_DONE/NEW_TOKEN, shutdown and close; plus local actions (stream reads/writes/resets, timers) between packets |
| `quic_endpoint_fuzzer` | `[config][records...]` | a server endpoint from the first datagram: DCID routing, Version Negotiation, Retry and token validation, INVALID_TOKEN closes, stateless resets, admission, Initial decryption, coalesced packets, and the server TLS 1.3 handshake via CRYPTO frames. Initial packets are sealed under the DCID-derived keys; Handshake and 1-RTT packets under the read keys the server itself installed |
| `quic_client_fuzzer` | `[config][records...]` | the client side of the handshake after `connect()`: Retry (integrity tag, Initial key re-derivation), Version Negotiation, server Initial / ServerHello, Handshake flight with the server's transport parameters, server CID adoption, coalesced datagrams |
| `quic_network_fuzzer` | `[config][ops...]` | a real in-tree client and server **completing real handshakes**, with the fuzzer as the network: datagrams captured from both sides' send paths are delivered, dropped, duplicated, reordered, corrupted or truncated; 1-RTT streams both ways, Retry + NEW_TOKEN, session resumption with accepted 0-RTT, NAT rebinding, loss/PTO timers, closes, and frames injected under either side's real keys at any level (including a key-phase flip) |

The stateful QUIC harnesses narrate each record with `QUIC_FUZZ_TRACE=1`
(for checking seeds and triage). A connection that stays charged against the
endpoint's receive-storage budget after teardown is reported as a crash.

The input layouts (control bits, record encoding) are documented at the top
of each harness.

## Build

```bash
cmake -S . -B build-fuzz -DFIBER_BUILD_FUZZERS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DFIBER_USE_LIBCXX=OFF -DFIBER_BUILD_APPS=OFF -DFIBER_BUILD_EXAMPLES=OFF -DFIBER_BUILD_TESTS=OFF
cmake --build build-fuzz -j
```

- Clang only. LTO is forced off. Fiber targets get
  `-fsanitize=address,undefined,fuzzer-no-link`; third-party deps are left
  uninstrumented so coverage feedback stays on our code.
- `FIBER_USE_LIBCXX=OFF`: distribution libFuzzer runtimes are commonly built
  against libstdc++ and fail to link into a libc++ build.
- Use a separate `FETCHCONTENT_BASE_DIR` (or a fresh checkout) if another
  build tree shares `temp/_deps`: the dependency *build* directories live
  there and differently-flagged trees overwrite each other.

## Run

```bash
# regression: replay the committed corpus once (also registered in ctest)
./build-fuzz/fuzz/tls_server_engine_fuzzer -runs=0 fuzz/corpus/tls_server_engine_fuzzer

# fuzz: new inputs go to the first directory, the committed seeds are read-only
mkdir -p work
./build-fuzz/fuzz/tls_server_engine_fuzzer -fork=4 -max_total_time=900 work fuzz/corpus/tls_server_engine_fuzzer
```

A crash writes `crash-<sha1>`; replay it with `./<fuzzer> crash-<sha1>`
(`-minimize_crash=1 -runs=20000` shrinks it). A fixed crash comes back as a
unit test, and its reproducer goes to `fuzz/regressions/<fuzzer>/` — replayed
by ctest with the seeds, and never touched by seed regeneration.

`ctest --test-dir build-fuzz` replays `corpus/` + `regressions/` for every
harness.

## Seed corpus

`fuzz/corpus/<fuzzer>/` is generated by `tls_fuzz_seedgen`, which drives the
engines against BoringSSL peers: full client and server handshake streams
(1.3, 1.2, GREASE, HRR, P-384, mTLS), a real 1.3 PSK resumption with 0-RTT and
a 1.2 ticket resumption against the harness's own ticket service (fixed key
and clock, so the tickets open during fuzzing), message bodies extracted for
the codec harness, and hand-built record sequences for the connection
harness. It prints how far each drive got. Regenerate after protocol changes:

```bash
rm -rf fuzz/corpus/tls_* && ./build-fuzz/fuzz/tls_fuzz_seedgen fuzz/corpus
```

The QUIC seeds come from `quic_fuzz_seedgen`, which builds them with the
in-tree encoders (every frame type, both transport-parameter owners, every
packet header form, Retry packets and address tokens under the harnesses'
fixed key) and lifts a real ClientHello out of the in-tree client's Initial
queue for the endpoint harness. The ClientHello is random per run (key
shares); everything else is deterministic:

```bash
rm -rf fuzz/corpus/quic_* && ./build-fuzz/fuzz/quic_fuzz_seedgen fuzz/corpus
```
