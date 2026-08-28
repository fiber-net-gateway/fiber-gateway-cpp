# QUIC Key Update (RFC 9001 §6) — Implementation Plan

## Status: Design / Pre-Implementation

## Problem Summary

The codebase has **zero key update support**: `kPacketFlagKeyPhase = 0x04` is defined in
`QuicProtocol.h:24` but never used anywhere. This means:

- Short-header application packets always set key phase `0` — a peer that initiates a key
  update sends packets with a flipped key phase bit that we cannot decrypt.
- We never initiate a key update ourselves, exceeding [`max_key_usage` / integrity
  limits](https://www.rfc-editor.org/rfc/rfc9001#section-6.6) on a long-running connection.
- On receive, we always select `application_read` keys from `QuicCryptoState` — there is no
  `next_key` slot, no trial-decryption path, and no key-phase tracking in the connection.

Handshake key discard exists (`discard_packet_number_space`), but that's a different RFC
requirement (§4.9) and is already handled.

## Goals

1.  **Receive path**: Detect flipped `key_phase` in short-header packets, trial-decrypt
    with pre-derived next-read keys, switch `current` ← `next` on success.
2.  **Send path**: Set the `key_phase` bit on outbound short-header packets at
    `EVP_AEAD` integrity-limit intervals (or when the peer has acknowledged our phase
    change).
3.  **Key derivation**: Pre-derive the "next" read/write keys using HKDF-Expand-Label
    with label `"tls13 quic ku"` (RFC 9001 §6.1, Algorithm 8).
4.  **Grace period**: Retain discarded keys for `3× PTO` after switching, so reordered
    packets with the old phase can still be decrypted (RFC 9001 §6.5).
5.  **Error handling**: `KEY_UPDATE_ERROR` (`0x0E`) — already in `QuicConnection.h:81`
    as `KeyUpdateError`.

## Implementation Steps

### Step 1. Add key update state to `QuicConnection` (and `QuicCryptoState`)

**Files**: `include/fiber/quic/QuicConnection.h`, `src/quic/QuicCrypto.cpp`, `src/quic/QuicCrypto.h`

**Add two new `QuicPacketProtectionKeys` slots in `QuicCryptoState`**:

```cpp
struct QuicCryptoState : ... {
    // existing fields ...
    QuicPacketProtectionKeys key_update_read{};
    QuicPacketProtectionKeys key_update_write{};
};
```

These hold the *pre-derived next-generation keys* (nginx's `next_key`). When `key_update_*`
has `ready == true`, they are available for trial decryption / outbound use.

**Add per-connection state in `QuicConnection` private members**:

```cpp
// Existing private members (line ~409-419)
QuicPacketProtectionKeys key_update_read_{};
QuicPacketProtectionKeys key_update_write_{};
event::EventLoop::TimerEntry key_update_timer_entry_{};  // for grace-period discards
bool key_phase_ = false;           // current outbound key phase
bool next_keys_ready_ = false;     // true when key_update_{read,write} are derived
```

The `key_phase_` bit controls the `kPacketFlagKeyPhase` in outbound short headers and is
compared against the bit decoded from incoming short headers.

**Add accessors**:

```cpp
bool key_phase() const noexcept { return key_phase_; }
void flip_key_phase() noexcept { key_phase_ = !key_phase_; }
bool next_keys_ready() const noexcept { return next_keys_ready_; }
```

---

### Step 2. Derive next keys via HKDF-Expand-Label (RFC 9001 §6.1 Algorithm 8)

**File**: `src/quic/QuicCrypto.cpp`

**New function**:

```cpp
[[nodiscard]] common::IoResult<void>
quic_derive_next_key_pair(
    QuicCryptoState &state,
    const QuicPacketProtectionKeys &current_read,
    const QuicPacketProtectionKeys &current_write) noexcept;
```

Implementation mirrors `ngx_quic_keys_update()` (nginx
`ngx_event_quic_protection.c:786-875`):

1. For each direction (read, write):
   - Verify `current.secret_len > 0`
   - HKDF-Expand-Label with label `"tls13 quic ku"` → new secret
   - HKDF-Expand-Label with label `"tls13 quic key"` from new secret → new key material
   - HKDF-Expand-Label with label `"tls13 quic iv"` from new secret → new IV
   - Copy HP key and HP context from current (header protection does not change on key
     update — RFC 9001 §5.4.3: HP uses the same key across updates).
2. Initialize AEAD context with new key and IV via `EVP_AEAD_CTX_init`.
3. Zero out current secrets (security — forward secrecy for old keys).
4. Move results into `state.key_update_read` / `state.key_update_write`.

Constants already exist in `QuicCrypto.cpp`:
- `kTls13LabelPrefix` = `"tls13 "` → concatenated with label to make `"tls13 quic ku"` etc.
- `hkdf_expand_label()` already handles the TLS 1.3 HKDF-Expand-Label construction.

**Called from**:

- After handshake confirmation: `handle_crypto_frame` in `QuicPacketProcessor.cpp` (line
  172-183) — after `discard_packet_number_space(Handshake)`, derive the first next-key set.
- After switching keys (on receive or send): immediately derive the *next-next* set.

---

### Step 3. Send path — set key_phase bit in short headers

**File**: `src/quic/QuicTransportCodec.cpp` (function `quic_init_packet_header`)

At line 574, the short-header flags are currently:

```cpp
case QuicEncryptionLevel::Application:
    packet.long_header = false;
    packet.type = QuicPacketType::Short;
    packet.flags = kPacketFlagFixed | pn_bits;  // ← no key phase bit
    break;
```

This becomes:

```cpp
case QuicEncryptionLevel::Application:
    packet.long_header = false;
    packet.type = QuicPacketType::Short;
    packet.flags = kPacketFlagFixed | pn_bits;
    if (connection.key_phase()) {
        packet.flags |= kPacketFlagKeyPhase;
    }
    break;
```

**Challenge**: `quic_init_packet_header` currently takes `(QuicPacketHeader&, const QuicPacketNumberSpace&)` — it lacks access to the connection. We need to either:

- **(Preferred)** Add a `key_phase` parameter to `quic_init_packet_header` (or pass the
  phase bit directly).
- Or pass the full `QuicConnection &` reference.

The callers in `QuicPacketCodec.cpp:235` and `QuicUdpEndpoint.cpp` will need to forward the
key phase from the connection. Since `quic_init_packet_header` is already in
`QuicTransportCodec.cpp` and `QuicPacketCodec.cpp:235` has the connection reference, this
is a clean change:

```cpp
void quic_init_packet_header(QuicPacketHeader &packet,
                              const QuicPacketNumberSpace &space,
                              bool key_phase) noexcept;
```

Or more generally, we can pass the connection:

```cpp
void quic_init_packet_header(QuicPacketHeader &packet,
                              const QuicConnection &connection,
                              const QuicPacketNumberSpace &space) noexcept;
```

---

### Step 4. Receive path — detect and handle key phase toggle

**Key insight from nginx**: The decryption path is staged:

1. **Read current key phase** from connection (`pkt->key_phase = qc->key_phase`).
2. **Remove header protection** using *current* HP keys (same HP key across updates).
3. **Extract the on-wire key_phase** from the now-revealed `flags`.
4. If `wire_key_phase != current_key_phase`:
   - If `next_keys_ready_`, try decrypting with `key_update_read` keys instead. Set
     `key_update = true`.
   - If `!next_keys_ready_`, log and return error (KEY_UPDATE_ERROR — peer updated before
     we had next keys ready).
5. After successful decryption and frame processing, if `key_update`:
   - Flip connection's `key_phase_`.
   - Swap `application_read` ← `key_update_read` (destroy old, install new).
   - Derive the *next-next* read keys.
   - Arm `key_update_timer_entry_` for `3× PTO` to discard the old keys (grace period).

**File**: `src/quic/QuicPacketCodec.cpp` — modify `quic_decode_packet` / `keys_for_packet`.

**Approach A (simpler, mirrors nginx)**: Change `quic_decode_packet` signature to pass both
the connection state and out-param for key_update. The function already takes
`QuicConnection &connection`. Inside:

```cpp
auto *keys = keys_for_packet(connection, packet->level, false);
// For short-header Application-level packets:
if (!packet->long_header && packet->level == QuicEncryptionLevel::Application) {
    // Extract key_phase from flags after header protection removal
    // (header protection removal happens inside quic_decrypt_packet_payload)
    bool wire_key_phase = (packet->flags & kPacketFlagKeyPhase) != 0;
    if (wire_key_phase != connection.key_phase()) {
        if (!connection.next_keys_ready()) {
            return std::unexpected(common::IoErr::Invalid); // KEY_UPDATE_ERROR
        }
        keys = &connection.key_update_read();
        result.key_update = true;
        result.new_key_phase = wire_key_phase;
    }
}
```

Actually, looking at the existing flow more carefully:

```
quic_decode_packet()
  → quic_parse_packet_header()          [header parsing, sets packet->level etc.]
  → keys_for_packet(connection, level, false)
  → quic_decrypt_packet_payload(packet, space, *keys, datagram, ...)
       [inside: remove_header_protection, then AEAD-decrypt]
  → After decrypt: check reserved bits, parse frames
```

The problem: **header protection removal happens inside `quic_decrypt_packet_payload`** (at
line 383 of `QuicCrypto.cpp`). The key_phase bit is unrevealed until after header
protection removal. So we must:

1. Remove header protection **first**, before choosing the decrypt key.
2. Read the key_phase bit from the now-clear flags.
3. Optionally switch to next keys for the AEAD decrypt step.

**Restructured decode pipeline**:

```
quic_decode_packet()
  → quic_parse_packet_header()          [header parsing]
  → For short application packets:
       Remove header protection with current HP keys
         (inline or via quic_remove_header_protection)
       Read key_phase from flags
       If wire_key_phase != connection.key_phase():
           next_keys_ready → use key_update_read, set key_update flag
           !next_keys_ready → abort
       Otherwise → use application_read
  → quic_decrypt_packet_payload() with the chosen keys
  → Verify reserved bits, parse frames
```

This means `quic_decode_packet` can no longer delegate all crypto work to
`quic_decrypt_packet_payload()` for short packets — it must do HP removal first, or we
refactor the decrypt function to accept an optional next-keys parameter.

**Refactor suggestion**: Split `quic_decrypt_packet_payload` into a two-phase:

```cpp
// Phase 1: Remove header protection (separate from AEAD)
auto hp_removed = quic_remove_header_protection(*packet, *keys, datagram, datagram_len);
// Phase 2: Now read key_phase, possibly switch keys
// Phase 3: AEAD decrypt with (possibly switched) keys
auto opened = quic_decrypt_payload(*packet, chosen_keys, ...);
```

This is a safe refactor — `quic_remove_header_protection` already exists as a public
function in `QuicCrypto.h`.

---

### Step 5. Post-processing on key update (in the processor)

**File**: `src/quic/QuicPacketProcessor.cpp` — in `process_decoded_packet()` or
`quic_process_datagram()`.

After `quic_decode_packet` returns with `key_update == true`:

```cpp
// Flip phase
conn.flip_key_phase();

// Swap: application_read ← key_update_read
swap_keys(conn.crypto().application_read, conn.key_update_read);
conn.key_update_read.reset();

// Derive next-next keys from the (now-current) application_read/write
quic_derive_next_key_pair(conn.crypto(), conn.crypto().application_read,
                           conn.crypto().application_write);
conn.next_keys_ready_ = true;

// Arm grace-period timer: after 3× PTO, discard old keys
// (The OLD application_read keys — which we saved before the swap — are kept
//  until the timer fires, then their EVP_AEAD_CTX is cleaned up.)
```

**Timer for key discard**: Use `EventLoop::post_at()` with `key_update_timer_entry_` and a
deadline `now + 3 * computed_pto`. The timer handler zeroizes the saved old keys.

---

### Step 6. Initiate outbound key updates

**File**: `src/quic/QuicPacketProcessor.cpp` — after handshake confirmation (the first
trigger), and later when integrity limits approach.

**When to initiate**: RFC 9001 §6.6 — endpoints SHOULD update before
`max_key_usage` (≈2^23 packets for AES-GCM). Track `packets_sent_with_current_phase`:

```cpp
// New field in QuicConnection:
std::uint64_t phase_send_packet_count_ = 0;
std::uint64_t phase_recv_packet_count_ = 0;
// In QuicPacketNumberSpace or QuicConnection
```

Increment on each sent application-level packet. When exceeding
`kKeyUpdateInitiateThreshold` (e.g., 2^22 — half the limit), initiate:

1. Derive next write keys from current `application_write`.
2. Flip internal `key_phase_` state.
3. Flip on-wire `kPacketFlagKeyPhase` on subsequent short headers.
4. On receiving peer ACK for a packet with the new phase → origin side confirms peer
   received the update. After confirmation, derive next-next write keys and set discard
   timer for the old write keys.

**Important**: The sender MUST NOT flip until the previous key update is complete (i.e.,
`next_keys_ready_ == false` — no in-flight update pending).

---

### Step 7. Grace-period old-key retention (RFC 9001 §6.5)

**New private field in `QuicConnection`**:

```cpp
QuicPacketProtectionKeys previous_read_keys_{};
QuicPacketProtectionKeys previous_write_keys_{};
event::EventLoop::TimerEntry key_update_discard_timer_{};
```

On key switch:
1. Move the old `application_read` → `previous_read_keys_` (retain context).
2. Swap `application_read` ← `key_update_read`.
3. Arm a timer at `now + 3 × current_PTO` (where PTO = `smoothed_rtt + max(4*rttvar, kTimerGranularity) + max_ack_delay`).
4. When the timer fires: `previous_read_keys_.reset()` (cleanup AEAD ctx, zero secrets).

On the send side, when we initiate:
- Keep old write keys until peer's ACK arrives with the new key phase.

**Receive trial decryption during grace**: Before returning `NotFound` for a failed
decrypt, if `previous_read_keys_.ready`, try decrypting with `previous_read_keys_` (for
reordered packets from before the update). This is optional for v1 but important for
robustness.

---

### Step 8. Special Cases and Error Recovery

- **Unsolicited key update** (peer updates without next keys ready): Return
  `KEY_UPDATE_ERROR` and close connection. Nginx treats this as a fatal connection error.
- **Concurrent key update** (both sides update at once): If a packet arrives with a new
  phase while we also just flipped our own phase, the keys should still match (both sides
  derived with the same input). The implementation must handle the case where
  `wire_key_phase == our_new_key_phase` → use `application_read` (already swapped).
- **Key update during closing/draining**: Ignore. The connection is tear-down.
- **TLS handshake not confirmed**: No key update before handshake is confirmed. The first
  update must happen after `mark_established()`.

---

## File Change Summary

| File | Change |
|------|--------|
| `QuicConnection.h` | Add `key_update_read_`, `key_update_write_`, `previous_read_`, `key_phase_`, `next_keys_ready_`, `phase_send_packet_count_`, `key_update_discard_timer_`, accessors |
| `QuicConnection.cpp` | Initialize new members, implement `swap_keys`, reset in `reset()` or close |
| `QuicCrypto.h` | Declare `quic_derive_next_key_pair()` |
| `QuicCrypto.cpp` | Implement `quic_derive_next_key_pair()` (HKDF with "quic ku" label) |
| `QuicTransportCodec.cpp` | Modify `quic_init_packet_header` → accept key_phase, set `kPacketFlagKeyPhase` |
| `QuicTransportCodec.h` | Update signature of `quic_init_packet_header` |
| `QuicPacketCodec.cpp` | Add key-phase trial-decrypt logic in `quic_decode_packet`, restructure HP removal + AEAD decrypt selection |
| `QuicPacketProcessor.cpp` | After handshake confirm: derive first next keys + after key_update: swap, derive next-next, arm discard timer |
| `QuicProtocol.h` | (Optional) add key update constants (thresholds) |

## Testing Strategy

- **Unit test**: `QuicCryptoTest` — verify `quic_derive_next_key_pair` produces known
  vectors from RFC 9001 §A.5 (Key Update).
- **Integration test**: Two-connection test where one side initiates a key update, verifies
  the other can decrypt, and vice versa.
- **Fuzz test**: Reordered packets across a key update boundary that should be decryptable
  within the grace period.

## Compatibility

- **Protocol version**: QUIC v1 only (RFC 9000 / 9001). No change to version negotiation.
- **TLS**: BoringSSL's `SSL_QUIC_METHOD` callbacks don't need modification — we derive
  keys ourselves via HKDF, not through SSL callbacks. The `set_encryption_secret` callback
  only fires during the initial handshake, not during key update.
- **Cipher suites**: All three supported suites (AES-128-GCM, AES-256-GCM,
  ChaCha20-Poly1305) use the same `"tls13 quic ku"` label, with key/secret sizes
  determined by `suite_spec()`.

## Reference

- nginx: `ngx_event_quic_protection.{c,h}` — `ngx_quic_keys_update()`,
  `ngx_quic_keys_switch()`, `ngx_quic_keys_discard()`
- nginx: `ngx_event_quic.c` — key_update event, key_phase toggle, receive path
- nginx: `ngx_event_quic_output.c` — outbound key phase bit
- RFC 9001 §6 — Key Update
- RFC 9001 §6.1 — Algorithm 8 / HKDF-Expand-Label with "quic ku" label
- RFC 9001 §A.5 — Key Update test vectors
- RFC 9001 §6.6 — Integrity limits (≈2^23 for AES-GCM)
- BoringSSL: `EVP_AEAD_CTX_{init,cleanup,seal,open}` remain the API even after update