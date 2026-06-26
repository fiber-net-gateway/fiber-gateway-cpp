# QUIC 0-RTT — Server-Side Reception

**Date:** 2026-06-25
**Scope:** QUIC transport layer only (no HTTP/3 behavior). Server-side 0-RTT
reception, nginx-aligned.
**Author:** Design via brainstorming; ratified against
`temp/nginx/src/event/quic/`.

---

## 1. Goal & Non-Goals

### Goal

Turn 0-RTT from "parses but can't drive" into working server-side reception: a
resumed client may send 0-RTT STREAM data; the server decrypts and delivers it,
accepts or rejects it after the handshake verdict, and rolls back stream
buffers on rejection so the client's 1-RTT retransmission is accepted cleanly.

### Non-Goals

- **No QUIC client.** The stack is server-only (`QuicUdpEndpoint` creates
  connections only from incoming Initials; `QuicTlsSession` has `init_server`
  only). 0-RTT is sent by the client; we do not build one. End-to-end testing
  uses an external client.
- **No HTTP/3 behavior.** H3 integration (425 Too Early, request idempotency)
  is deferred. The transport exposes a per-stream `received_early_data()` flag
  so an application *can* gate, but no H3 code changes.
- **No anti-replay database.** Per the chosen scope, replay protection is
  deferred to the application via the early-data flag (nginx model).
- **No new transport parameters.** `max_early_data` is a TLS-layer setting, not
  a QUIC TP (confirmed: nginx and project TP IDs both stop at `0x10`). The
  existing `zero_rtt_len` split in the transport-params codec is the only TP
  mechanism involved, and it already exists.

---

## 2. Current State (verified)

| Capability | Status | Location |
|---|---|---|
| 0-RTT long-header parse → `ZeroRtt`/`EarlyData` | ✅ done | `QuicTransportCodec.cpp:207` |
| 0-RTT long-header build (`kLongPacketTypeZeroRtt`) | ✅ done | `QuicTransportCodec.cpp:568` (send-side; unused on server) |
| `EarlyData` encryption level + `early_read`/`early_write` key slots | ✅ done | `QuicConnection.h:195`, `QuicCrypto.cpp:385` |
| `early_read` keys installed from `set_read_secret(ssl_encryption_early_data)` | ✅ done | `QuicTlsSession.cpp:70-89` |
| `EarlyData` aliases Application pn space | ✅ done | `QuicConnection.cpp:1686` |
| Frame permissions: STREAM/RESET_STREAM/STOP_SENDING allowed at `EarlyData`; ACK/CRYPTO/HANDSHAKE_DONE/NEW_TOKEN rejected | ✅ done | `QuicTransportCodec.cpp:23-55, 622` |
| `zero_rtt_len` TP split for `SSL_set_quic_early_data_context` | ✅ done | `QuicTransportParamsCodec.cpp:340` |
| `SSL_CTX_set_early_data_enabled` / `SSL_set_early_data_context` / `SSL_set_early_data_enabled` | ❌ not wired | `QuicTlsSession.cpp`, `TlsContext.cpp` |
| Per-connection accept/reject verdict + stream rollback | ❌ absent | `QuicConnection`, `QuicStreamRecvQueue` |
| 0-RTT ACK suppression | ❌ absent | `QuicPacketProcessor.cpp:476,633` |
| Post-resolution 0-RTT packet drop | ❌ absent | `QuicPacketProcessor.cpp` |

BoringSSL reference APIs available (all confirmed present in
`temp/_deps/boringssl-src/include/openssl/ssl.h`):
`SSL_CTX_set_early_data_enabled`, `SSL_CTX_set_max_early_data`,
`SSL_set_early_data_enabled`, `SSL_set_quic_early_data_context`,
`SSL_in_early_data`, `SSL_early_data_accepted`.
**No** `SSL_write_early_data`/`SSL_read_early_data` for QUIC — 0-RTT flows as
STREAM frames, which the project already routes through `early_read` keys.
**No** BoringSSL replay detection: `SSL_set_quic_early_data_context` does only
byte-for-byte transport-param matching (`ssl.h:4147` warns replays may be
processed).

---

## 3. Design

### 3.1 TLS & Resumption (server-side)

The server **issues** session tickets (automatic in TLS 1.3; the project already
emits post-handshake `NewSessionTicket` CRYPTO data via `add_handshake_data`).
It **accepts** resumption and 0-RTT. No save/restore logic is needed on the
server.

Changes:

1. **`TlsServerContext::init_base_context`** (`src/net/TlsContext.cpp`): when a
   new `enable_0rtt` option is on, call `SSL_CTX_set_early_data_enabled(ctx, 1)`
   and `SSL_CTX_set_max_early_data(ctx, max_early_data)`. Session cache is on by
   default in BoringSSL; ticket issuance is automatic. This is the gateway-wide
   switch.

2. **`QuicTlsSession::init_server`** (`src/quic/QuicTlsSession.cpp`): after
   `quic_create_transport_params` returns `*transport_params_len`, capture the
   `zero_rtt_len` (call the codec with the out-param) and call
   `SSL_set_quic_early_data_context(ssl, transport_params, zero_rtt_len)`. The
   `zero_rtt_len` bytes are exactly the "remembered" TP prefix BoringSSL
   compares against what the ticket carried. Then `SSL_set_early_data_enabled
   (ssl, 1)` so early data is allowed per-connection (consistent with CTX).

3. **No change** to `set_read_secret(ssl_encryption_early_data)` — it already
   installs `early_read` keys and works.

### 3.2 Receive Path & Accept/Reject Gate (core)

0-RTT packets arrive coalesced with the Initial in one datagram (Initial first).
The processor processes coalesced packets in order: Initial → `init_server` →
`drive_handshake` → BoringSSL consumes the ClientHello/PSK →
`set_read_secret(early_data)` installs `early_read`. Subsequent 0-RTT packets
decrypt with those keys. `quic_decode_packet` and
`quic_frame_allowed_for_receiver` already handle `EarlyData`. **No decrypt /
frame-permission changes.**

New per-connection state on `QuicConnection`:

```cpp
enum class EarlyDataStatus : std::uint8_t {
    None,      // no early data seen, or 0-RTT disabled
    Pending,   // early data received, handshake not yet confirmed
    Accepted,  // handshake confirmed and SSL_early_data_accepted() == 1
    Rejected,  // handshake confirmed and SSL_early_data_accepted() == 0
};
EarlyDataStatus early_data_status_ = EarlyDataStatus::None;
```

**Delivery (Approach 1 — tag and deliver, retroactive discard).** While status
is `Pending`, 0-RTT STREAM frames are delivered to stream recv queues normally
via `recv_stream_frame`, exactly as 1-RTT frames. Streams that receive early
data set a `received_early_data_` flag (per-stream, on `QuicStream`). The
connection records, per early-data stream, the high-water offset delivered while
`Pending` so it can roll back on rejection.

**Gate at handshake confirmation.** In `handle_crypto_frame` /
`mark_established` (`QuicPacketProcessor.cpp:217`), once
`tls().handshake_done()`, query `SSL_early_data_accepted(ssl)` exactly once:

- **Accepted:** `early_data_status_ = Accepted`. Streams and their early data
  are left intact; clear the per-stream early-delivery accounting. The
  `received_early_data_` flags stay set (apps may read them). Discard
  `early_read` keys (`crypto_.early_read.reset()`).
- **Rejected:** `early_data_status_ = Rejected`. For each stream that received
  early data, call `QuicStreamRecvQueue::rollback_early_data(high_water)` to
  discard the early-delivered bytes and rewind the recv offset to 0, so the
  client's 1-RTT retransmission of the same data (offsets 0..N) is accepted as
  fresh data rather than rejected as duplicate. Notify the app via the existing
  stream lifecycle. Discard `early_read` keys.
- If no early data was ever received (`early_data_status_ == None`): nothing to
  do.

**Post-resolution drop.** Once `early_data_status_` is `Accepted` or
`Rejected`, the processor drops any further `QuicPacketType::ZeroRtt` packets
without decryption (RFC 9001: 0-RTT ignored after handshake completion). This
prevents late/replayed 0-RTT from perturbing stream state.

### 3.3 0-RTT ACK Suppression (correctness fix)

**Problem:** `EarlyData` aliases the Application pn space. Today
`process_decoded_packet` calls `space.on_packet_received(pn, now,
result.ack_eliciting)` unconditionally (`QuicPacketProcessor.cpp:476`), and the
non-coalesced path at `:633` does the same. A 0-RTT packet's number would be
recorded into the Application space's ACK ranges and could schedule an ACK.

**Rule (RFC 9001 §5.8):** the server MUST NOT ack 0-RTT packets.

**Fix:** in `process_decoded_packet` (and the `:633` path), when
`packet.level == QuicEncryptionLevel::EarlyData`:
- Do not call `on_packet_received` with `ack_eliciting=true`. Record the packet
  number only for duplicate-detection if needed (or skip pn-space recording
  entirely for 0-RTT — see Open Question §6.1), and never set
  `result.ack_eliciting` for a 0-RTT-only packet.
- Frame-level `ack_eliciting` from parsed 0-RTT frames must not propagate to
  the pn-space ACK machinery.

The simplest correct implementation: guard the `on_packet_received` /
`handle_ack_eliciting_packet` calls with `level != EarlyData`, and force
`result.ack_eliciting = false` when the packet is 0-RTT. (Coalesced datagrams
are processed per-packet, so a 1-RTT packet coalesced after a 0-RTT still gets
acked normally via its own `process_decoded_packet` call.)

### 3.4 Stream Rollback Semantics

`QuicStreamRecvQueue::rollback_early_data(std::uint64_t early_high_water)`:

- Discards all buffered receive bytes at offsets `< early_high_water` that
  arrived via early data.
- Resets the stream's consumed/reassembled offset so a 1-RTT retransmission of
  offset `0..N` is accepted as new (not duplicate) data. Specifically the
  reassembler must forget it has seen those offsets.
- Must handle: partial streams, out-of-order early data, and the case where
  early data set a final size / FIN — the FIN and final-size from early data
  are discarded too (the client retransmits them in 1-RTT or resets).

The connection tracks `early_high_water` per stream only while status is
`Pending`; once resolved the accounting is discarded. Streams created by early
data that carry no 1-RTT continuation are subject to the app's normal
idle/close handling (no special teardown beyond rollback).

**Open question §6.1** addresses whether a rejected 0-RTT stream should instead
be RESET by the server (simpler) vs. rolled back (lower complexity for the
client). See §6.

### 3.5 Configuration

New options, plumbed `TlsOptions → TlsServerContext → QuicUdpEndpoint::Options`:

| Option | Type | Default | Purpose |
|---|---|---|---|
| `enable_0rtt` | bool | `false` | Master switch. Off by default for replay safety. |
| `max_early_data` | uint32 | `0` (= unlimited) | Passed to `SSL_CTX_set_max_early_data`. Caps early-data bytes per resumed connection at the TLS layer. |

When `enable_0rtt` is false, none of §3.1's BoringSSL calls are made; the
server behaves exactly as today (early-data ClientHello extensions ignored,
0-RTT packets would fail to decrypt since no `early_read` keys are installed —
which is already the status quo).

### 3.6 Application Hook (transport-only)

Exposed for future H3 use; no behavior in this scope:

- `QuicConnection::early_data_status() const -> EarlyDataStatus`
- `QuicConnection::early_data_accepted() const -> bool` (true iff `Accepted`)
- `QuicStream::received_early_data() const -> bool`

An application reading streams may consult these to gate non-idempotent
operations. This scope wires the accessors only.

---

## 4. Lifecycle & Key Discard

- `early_read` keys stay live from `set_read_secret(early_data)` until the
  accept/reject verdict is taken at handshake confirmation. They must survive
  that window because 0-RTT packets may arrive before the handshake completes.
- On verdict (either), `crypto_.early_read.reset()`.
- `early_write` is never used (server sends no 0-RTT; half-RTT uses Application
  keys). No change.
- Post-verdict 0-RTT packets are dropped (§3.2), so `early_read` need not be
  retained after reset.

---

## 5. Testing

### 5.1 Unit tests (in-tree, `tests/Quic*Test.cpp`)

1. **`zero_rtt_len` split correctness** — `quic_create_transport_params` with
   `zero_rtt_len` out-param: the prefix length covers exactly the "remembered"
   TPs and excludes server-only ones (`original_dcid`, `retry_scid`,
   `stateless_reset_token`). Assert the byte range matches what
   `SSL_set_quic_early_data_context` would compare.
2. **Frame permissions at `EarlyData`** — STREAM/RESET_STREAM/STOP_SENDING
   accepted; ACK/CRYPTO/HANDSHAKE_DONE/NEW_TOKEN rejected via
   `quic_frame_allowed_for_receiver(Server, EarlyData, ...)`.
3. **0-RTT ACK suppression** — a 0-RTT packet does not set `ack_eliciting` and
   does not record a pn into the Application ACK ranges; a coalesced 1-RTT
   packet in the same datagram still generates its ACK.
4. **Early-data state machine:**
   - `Pending → Accepted`: stream early data preserved, readable; flags set.
   - `Pending → Rejected`: `rollback_early_data` discards early bytes; a
     subsequent 1-RTT retransmission at offset 0 is accepted (not duplicate).
   - Post-verdict 0-RTT packet dropped without decryption.
5. **`enable_0rtt=false`** — no `early_read` keys installed; 0-RTT packets fail
   decrypt (status quo preserved).

### 5.2 End-to-end (manual, external client)

No in-tree client (server-only scope). Acceptance plan with concrete commands:

1. Run the server with `enable_0rtt=true`.
2. Drive an external QUIC client that supports 0-RTT (`quiche-client`,
   `quinn`, or `openssl s_client -earlydata`) against it.
3. First connection: full handshake, obtain a session ticket.
4. Second connection: client sends 0-RTT data. Verify:
   - Server decrypts and delivers early data (accepted case).
   - For a transport-level loopback, verify the resumed connection completes
     in 1 RTT.
5. Force rejection (mismatch the early-data context, e.g. change server TP
   limits between connections): verify server rejects 0-RTT, client retransmits
   in 1-RTT, data still delivered.

Record the exact client/version and commands in the test plan when
implementing.

---

## 6. Open Questions

### 6.1 Rejected-0-RTT stream disposition: rollback vs. reset

**Rollback** (proposed in §3.2/§3.4): discard early bytes, rewind offset, accept
1-RTT retransmission. Pro: transparent to the client, lowest client complexity.
Con: rollback logic in `QuicStreamRecvQueue` must handle partial/out-of-order/
FIN early data — most intricate part of the design.

**Alternative — server RESET:** on rejection, the server sends RESET_STREAM for
each early-data stream and STOP_SENDING; the client opens a fresh stream and
retransmits there. Pro: simpler server (no rollback). Con: higher client
complexity, more round trips, not what nginx does (nginx delivers, app decides).

**Recommendation:** rollback, matching nginx and the chosen "deliver early,
gate at app" model. Decision deferred to user ratification — see §6 in the
brainstorm; if the user prefers reset, §3.4 is replaced.

---

## 7. Component Summary

| Area | File | Change |
|------|------|--------|
| TLS CTX | `src/net/TlsContext.cpp`, `TlsOptions.h` | `SSL_CTX_set_early_data_enabled`, `SSL_CTX_set_max_early_data`; `enable_0rtt`/`max_early_data` options |
| Per-conn TLS | `src/quic/QuicTlsSession.cpp/.h` | `SSL_set_quic_early_data_context` (using `zero_rtt_len`), `SSL_set_early_data_enabled`; expose `early_data_accepted()` |
| Receive/gate | `src/quic/QuicConnection.{h,cpp}`, `QuicPacketProcessor.cpp` | `EarlyDataStatus` tracker; accept/reject gate at handshake-confirm; post-verdict 0-RTT drop; 0-RTT ACK suppression |
| Stream rollback | `src/quic/QuicStreamRecvQueue.{h,cpp}`, `QuicStream.h` | `rollback_early_data()`; `received_early_data_` flag |
| App accessors | `QuicConnection.h`, `QuicStream.h` | `early_data_status()`, `early_data_accepted()`, `received_early_data()` (no H3 behavior) |
| Config | `src/quic/QuicUdpEndpoint.h` | forward `enable_0rtt`/`max_early_data` |
| Tests | `tests/Quic*Test.cpp` | unit tests §5.1 + manual e2e plan §5.2 |

---

## 8. Out-of-Scope Follow-Ups

- H3 425 Too Early + request-method idempotency gate.
- In-tree QUIC client (would unlock loopback 0-RTT testing and outbound 0-RTT).
- Anti-replay window (if the gateway fronts non-idempotent services that must
  never see a replay).
