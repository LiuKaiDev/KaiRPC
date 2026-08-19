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

- **Status:** FIXED in Stage 5 (`fix(runtime): harden callback lifetime and
  thread shutdown`).
- **Root cause:** `TcpConnection` read/write/error handlers and client connect
  handlers captured raw owners in callbacks that could remain queued after
  destruction. The server listener and cleanup timer had the same stale-owner
  risk.
- **Regression tests:** `unit.lifetime.deferred_callback` queues a real
  connection read callback, destroys the shared connection, and runs the loop;
  the Stage 4 baseline produced an ASan heap-use-after-free in
  `TcpConnection::onRead()`. `unit.lifetime.timer_cancel` verifies cancelled
  timer work does not run, and `unit.shutdown.server` exercises server teardown.
- **Production fix:** Connection and client event callbacks use weak ownership
  guards; server callbacks use a lifetime token; timer cancellation is exposed
  through `Eventloop::deleteTimerEvent()` and its flag is atomic.
- **Invariant:** A queued callback becomes a no-op once its owner is destroyed,
  and cancelled timer work cannot re-arm or invoke owner state.

### BUG-IOTHREAD-001: shutdown ownership is incomplete

- **Status:** FIXED in Stage 5 (`fix(runtime): harden callback lifetime and
  thread shutdown`).
- **Root cause:** An unstarted worker blocked forever on its start semaphore,
  `IOThreadGroup::~IOThreadGroup()` leaked children, and semaphore destruction
  raced worker access. EventLoop-owned eventfds/timerfds were also not closed.
- **Regression tests:** `unit.iothread.start_stop`,
  `unit.iothread.destructor`, `unit.iothread.idempotent_stop`,
  `unit.iothreadgroup.shutdown`, `unit.iothreadgroup.repeat`, and
  `unit.shutdown.server` cover bounded stop/join and repeated `/proc/self/task`
  and `/proc/self/fd` counts.
- **Production fix:** `IOThread::stop()` is idempotent and releases an
  unstarted worker; destruction stops, joins exactly once, then destroys
  semaphores. `IOThreadGroup` owns, stops, joins, and deletes every child.
  EventLoop, timer, wakeup, acceptor, and server teardown follow that order.
- **Invariant:** No owned worker or shutdown synchronization object outlives
  its owner, and repeated lifecycle does not accumulate threads or descriptors.

## RPC behavior

### BUG-RPC-001: timeout and cancellation are incomplete

- **Status:** FIXED in Stage 6 (`fix(rpc): enforce timeout and exactly-once completion`).
- **Root cause:** Timeout set `Finished` before `RpcChannel::callBack()`, so the
  user closure was skipped; timers and response callbacks were not invalidated,
  and `NotifyOnCancel()` was empty.
- **Pre-fix evidence:** `timeout 5s /tmp/kairpc-stage6-red/bin/rpc_completion_test timeout`
  returned `124` after logging `call rpc timeout arrive`; no user completion ran.
- **Regression tests:** `unit.rpc.timeout`, `unit.rpc.cancel`,
  `unit.rpc.response`, and `unit.rpc.timeout_wins`; ASan completion tests passed
  6/6 for 50 rounds.
- **Production fix:** A per-call request state owns the timer, transport,
  response, and closure. Timeout, cancellation, response, connect error, and
  disconnect all compete through one atomic terminal gate, remove pending read
  state, invalidate the timer, and invoke the closure once.
- **Invariant:** A terminal request cannot be completed again; late responses and
  timer events are no-ops, and terminal resources are released.
- **Stage 6 commit:** `fix(rpc): enforce timeout and exactly-once completion`.

### BUG-RPC-002: disconnect callback exactly-once behavior is incomplete

- **Status:** FIXED in Stage 6 (`fix(rpc): enforce timeout and exactly-once completion`).
- **Root cause:** Connect refusal and peer terminal events had no shared
  completion path; pending callbacks could remain unresolved or race timeout.
- **Pre-fix evidence:** The old timeout path reproduced missing completion and
  hung until the outer watchdog. Connect-failure and disconnect cases are
  retained as post-fix regression coverage.
- **Regression tests:** `unit.rpc.connect_failure` and
  `unit.rpc.disconnect`, plus response/timeout race coverage.
- **Production fix:** TcpConnection exposes disconnect notification and pending
  read removal; RpcChannel routes connect and transport errors through the same
  request state gate as success and timeout.
- **Invariant:** Every locally detected terminal failure completes the user
  callback at most once and leaves no pending request entry.
- **Stage 6 commit:** `fix(rpc): enforce timeout and exactly-once completion`.

Exactly-once completion means at-most-once user callback execution for one
logical RPC attempt. It does not guarantee exactly-once remote business
execution.

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
