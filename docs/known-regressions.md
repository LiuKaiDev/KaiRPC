# Known Regressions

This inventory records known behavior and the regression stage responsible for
each item. Fixed entries retain their root cause and regression evidence;
remaining entries stay open for their planned stages.

## Transport and ownership

### BUG-TCP-001: `onWrite` does not advance the output read index

- **Status:** FIXED in Stage 3 (`fix(tcp): correct buffer and partial-write
  handling`).
- **Root cause:** `TcpConnection::onWrite()` did not advance the output read
  index after a successful `write()`, so a partial or repeated writable
  callback restarted at the already-sent prefix.
- **Regression tests:** `unit.tcp_write.complete`, `unit.tcp_write.partial`,
  and `unit.tcp_write.eagain` use a nonblocking socketpair to cover complete
  writes, deterministic partial writes, repeated callbacks, ordering, and
  EAGAIN retry behavior.
- **Production fix:** Advance the output read index by exactly the positive
  syscall return value, retain the unwritten suffix, and remove writable
  interest or run client completion callbacks only after the buffer drains.
- **Invariant:** If `write(fd, data, N)` returns `K > 0`, exactly `K` readable
  bytes are consumed; EAGAIN consumes none; completed output cannot be resent.

### BUG-TCPBUFFER-001: exact-capacity `moveWriteIndex` fails

- **Status:** FIXED in Stage 3 (`fix(tcp): correct buffer and partial-write
  handling`).
- **Root cause:** `TcpBuffer::moveWriteIndex()` rejected a write index equal to
  the vector capacity because its boundary check used `>=`.
- **Regression test:** `unit.tcp_buffer` advances by exactly `writeAble()` and
  verifies the resulting indexes and readable byte count.
- **Production fix:** Validate the requested advancement against
  `writeAble()` and allow equality before advancing the write index.
- **Invariant:** `0 <= read_index <= write_index <= capacity`; advancing by
  the full writable count is legal, while advancing beyond it is rejected.

### BUG-FD-001: close ownership is incomplete

- **Status:** FIXED in Stage 4 (`fix(net): harden fd lifecycle and epoll error
  handling`).
- **Root cause:** `TcpConnection::clear()` removed epoll interest and marked
  the connection closed, but never released the socket; `TcpClient` also kept
  a second close path.
- **Regression tests:** `unit.fd_lifecycle.close`,
  `unit.fd_lifecycle.idempotent`, and `unit.fd_lifecycle.repeat` verify
  `EBADF`, peer EOF, repeated cleanup safety, and a stable `/proc/self/fd`
  count across 100 bounded lifecycles.
- **Production fix:** `TcpConnection::clear()` is the single final release
  path: it marks the connection closed, invalidates `m_fd`, unregisters and
  resets the cached event, and closes the owned descriptor exactly once.
  `TcpClient` delegates teardown to its connection.
- **Invariant:** A connection-owned descriptor is invalid after final
  teardown, and subsequent cleanup cannot close a reused descriptor number.

### BUG-EPOLL-001: HUP/RDHUP handling is incomplete

- **Status:** FIXED in Stage 4 (`fix(net): harden fd lifecycle and epoll error
  handling`).
- **Root cause:** `Fd_Event` did not register `EPOLLRDHUP`, and `Eventloop`
  ignored HUP/RDHUP, allowing terminal descriptors to remain active.
- **Regression tests:** `unit.epoll.hup`, `unit.epoll.rdhup`, and
  `unit.epoll.rdhup_with_data` cover full close, half-close, terminal
  deregistration, and readable data arriving with RDHUP.
- **Production fix:** Read interest includes `EPOLLRDHUP`; HUP/RDHUP are
  terminal events that unregister the descriptor and invoke the error callback
  after any readable callback from the same epoll result.
- **Invariant:** Terminal events do not busy-loop, do not dispatch writes, and
  do not discard data reported with `EPOLLIN`.

### BUG-EPOLL-002: error events can invoke the wrong callback

- **Status:** FIXED in Stage 4 (`fix(net): harden fd lifecycle and epoll error
  handling`).
- **Root cause:** The `EPOLLERR` branch tested the error handler but queued the
  writable handler.
- **Regression test:** `unit.epoll.error_dispatch` uses a real pipe error and
  distinct read/write/error counters.
- **Production fix:** Terminal dispatch snapshots and queues the registered
  `ERROR_EVENT` callback, while suppressing `EPOLLOUT` for the same terminal
  result.
- **Invariant:** `EPOLLERR` reaches only error handling; it is never treated as
  successful writable progress.

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
