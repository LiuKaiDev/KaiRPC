# Known Regressions

This inventory records behavior that is intentionally preserved by the Stage 2
baseline. The CTest suite detects the currently supported paths; it does not
turn these observations into production fixes.

## Transport and ownership

### BUG-TCP-001: `onWrite` does not advance the output read index

- **Current observation:** `TcpConnection::onWrite()` writes from the output
  buffer but does not move its read index by the number of bytes accepted.
- **Risk:** A later writable event can resend bytes, duplicate application
  data, or report completion while bytes remain queued.
- **Future test idea:** Use a socketpair with a forced partial write and assert
  that each byte is emitted once and the completion callback waits for the
  entire range.
- **Planned stage/category:** Stage 4, transport write correctness.

### BUG-TCPBUFFER-001: exact-capacity `moveWriteIndex` fails

- **Current observation:** `TcpBuffer::moveWriteIndex()` rejects an index that
  lands exactly at the vector capacity (`>=` instead of `>`).
- **Risk:** A read that fills the available buffer can leave the write index
  stale and corrupt subsequent TinyPB framing.
- **Future test idea:** Fill a buffer exactly, move the write index to the end,
  and verify the readable byte count and decoded packet.
- **Planned stage/category:** Stage 4, TCP buffer boundary correctness.

### BUG-FD-001: close ownership is incomplete

- **Current observation:** `TcpConnection::clear()` removes event
  registration/state but does not close the socket; client and server paths
  retain separate fd ownership assumptions.
- **Risk:** Descriptor leaks and stale events can target a reused fd.
- **Future test idea:** Repeatedly connect, close, and inspect `/proc/self/fd`
  while forcing descriptor reuse.
- **Planned stage/category:** Stage 5, lifecycle and shutdown ownership.

### BUG-EPOLL-001: HUP/RDHUP handling is incomplete

- **Current observation:** Event dispatch checks `EPOLLERR` but does not
  consistently handle `EPOLLHUP` or `EPOLLRDHUP`.
- **Risk:** Peer half-close or hangup can leave connections and pending RPCs
  alive indefinitely.
- **Future test idea:** Close a socket in each half-close direction and assert a
  bounded transition to the closed state.
- **Planned stage/category:** Stage 4, event/error handling.

### BUG-EPOLL-002: error events can invoke the wrong callback

- **Current observation:** The event loop queues the writable callback for an
  `EPOLLERR` path instead of the registered error callback.
- **Risk:** Errors are treated as successful writes and cleanup/error reporting
  is skipped.
- **Future test idea:** Inject a socket error and assert that only the error
  callback runs.
- **Planned stage/category:** Stage 4, event/error handling.

### BUG-LIFETIME-001: raw callbacks can outlive owners

- **Current observation:** Event, timer, RPC, and heartbeat callbacks capture
  raw object pointers without a cancellation barrier.
- **Risk:** Shutdown/reconnect races can dereference freed objects.
- **Future test idea:** Destroy a connection/channel while callbacks are queued
  and run under ASan/TSan with a deterministic synchronization barrier.
- **Planned stage/category:** Stage 5, callback lifetime and cancellation.

### BUG-IOTHREAD-001: shutdown ownership is incomplete

- **Current observation:** `IOThreadGroup` has no coordinated destructor/join
  path, and server teardown order does not drain worker event loops.
- **Risk:** Threads, eventfds, timers, and queued tasks can outlive the server.
- **Future test idea:** Start and stop a server repeatedly, join every worker,
  and assert no thread or fd remains after destruction.
- **Planned stage/category:** Stage 5, lifecycle and shutdown ownership.

## RPC behavior

### BUG-RPC-001: timeout and cancellation are incomplete

- **Current observation:** Timeout marks the controller but does not cancel all
  outstanding socket/timer callbacks; cancellation notification is empty.
- **Risk:** Late responses can race timeout state or retain resources longer
  than the call contract allows.
- **Future test idea:** Delay a server response past the deadline and assert one
  callback, one terminal controller state, and no late callback.
- **Planned stage/category:** Stage 6, RPC semantics.

### BUG-RPC-002: disconnect callback exactly-once behavior is incomplete

- **Current observation:** Several disconnect and discovery-failure paths return
  without invoking the user closure, while other paths guard completion with a
  finished flag.
- **Risk:** Callers can hang forever or observe inconsistent completion counts.
- **Future test idea:** Drop the peer at connect, write, and read phases and
  count callback invocations under a short deadline.
- **Planned stage/category:** Stage 6, RPC semantics and failure handling.

## TinyPB and service discovery

### BUG-TINYPB-001: checksum is not validated

- **Current observation:** The encoder writes a constant checksum and the
  decoder stores it without comparing it to packet contents.
- **Risk:** Corrupted payloads can be accepted as valid RPC messages.
- **Future test idea:** Flip one body byte and assert decode rejects the packet.
- **Planned stage/category:** Stage 3, TinyPB hardening.

### BUG-TINYPB-002: malformed and bounds validation is incomplete

- **Current observation:** Some package/field bounds are checked, but empty
  identifiers, malformed combinations, and all integer/capacity edges are not
  rejected by one explicit contract.
- **Risk:** Malformed input can be accepted, allocate unexpectedly, or desync
  the stream.
- **Future test idea:** Table-drive truncated, oversized, negative, and invalid
  marker packets through the decoder and run them under fuzzing.
- **Planned stage/category:** Stage 3, TinyPB hardening.

### BUG-SD-001: service-discovery framing and failure handling are incomplete

- **Current observation:** Query/control exchanges use an unframed single read
  and write with no bounded I/O deadline, authentication, or lifecycle stop.
- **Risk:** Partial commands, stalled peers, and ambiguous failures can block or
  corrupt registry state.
- **Future test idea:** Fragment commands, coalesce commands, delay responses,
  and exercise TTL expiry/restart behavior with a fake client.
- **Planned stage/category:** Stage 7, service-discovery robustness.

## Logging and high availability

### BUG-LOG-001: dynamic format strings are passed to printf-family APIs

- **Current observation:** Logger paths pass constructed strings directly as
  format strings.
- **Risk:** Percent sequences in log content can trigger incorrect formatting
  or memory reads.
- **Future test idea:** Log messages containing `%s`, `%n`, and UTF-8 bytes and
  verify literal output under sanitizers.
- **Planned stage/category:** Stage 8, observability and safety.

### BUG-LOG-002: `sprintf` can overflow the restart counter buffer

- **Current observation:** High-availability restart formatting uses a fixed
  ten-byte array with `sprintf`.
- **Risk:** A large restart count can overwrite adjacent stack memory.
- **Future test idea:** Exercise the restart path with boundary and malformed
  counters under ASan and assert bounded formatting.
- **Planned stage/category:** Stage 8, observability and safety.
