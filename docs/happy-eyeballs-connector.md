# Happy Eyeballs TCP connector

`fiber::net::TcpConnector` implements the connection-establishment part of
[RFC 8305](https://www.rfc-editor.org/rfc/rfc8305.html) for an already-resolved address set. DNS
lookup, destination sorting, and address caching remain separate. The existing single-address
`TcpStream::connect` API and its timeout semantics are unchanged.

## Input and ordering

The input is copied into fixed storage before suspension and is limited to
`kHappyEyeballsMaxAddresses` (16) entries. `HappyEyeballsAddressPolicy` selects IPv6-first,
IPv4-first, or one family only. Order within each family is stable.

For a dual-family policy, the connector emits `first_address_family_count` preferred-family
entries first (one by default), then alternates the other and preferred families. For example,
IPv6-first transforms `v6-a, v6-b, v4-a, v4-b` into `v6-a, v4-a, v6-b, v4-b`.

## Attempt timing

- The first candidate is attempted immediately.
- A pending attempt remains active while the next candidate starts after
  `connection_attempt_delay` (250 ms by default). Delays below 10 ms are rejected.
- An attempt that fails before the delay expires advances to the next candidate immediately.
- At most `max_concurrent_attempts` sockets are active (two by default, four maximum).
- There is no per-attempt timeout. Every active and future attempt shares `total_timeout`; an
  infinite total timeout is supported. A zero timeout still permits the first nonblocking
  `connect(2)` call, but does not wait after it returns `EINPROGRESS`.

The first TCP success wins. Before returning its `StreamInfant`, the connector removes the winner
from the poller and closes every losing socket on the owner EventLoop.

## Cancellation and errors

The awaiter exposes `cancel()` for owner-loop cancellation. Destroying a suspended awaiter closes
its sockets without resuming its caller. EventLoop shutdown invokes an intrusive stop callback,
returns `IoErr::Canceled`, cancels the timer, and closes all connector-owned descriptors before the
loop thread exits.

`HappyEyeballsConnectError` records candidate-to-input mapping, attempted and failed masks, and one
error per normalized candidate without allocation. When every address fails, `code` is the error
of the last normalized candidate, so the primary error and summary are independent of completion
order. Cancellation and the shared deadline use `Canceled` and `TimedOut` respectively while
retaining failures already observed.

## HTTP/1 and TLS integration

`Http1ClientConnection` has a multi-address overload that races only the TCP phase. The winner then
uses the same TCP socket options and the same optional TLS context, SNI, certificate verification,
ALPN checks, and TLS handshake timeout as the single-address path. TLS timeout remains separate from
the TCP total timeout. The connection state and selected peer are published only after transport
creation and any TLS handshake succeed.

Once a TCP winner is selected, its losing TCP attempts are already closed. A later TLS failure is
returned to the caller and does not restart the address race.

On a lite_nginx pool miss, one lease and one `Http1ClientConnection` cover the complete TCP race and
optional TLS setup. A failed connection is never exposed as reusable, and the lease is reset once.
