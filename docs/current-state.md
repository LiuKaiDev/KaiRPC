# KaiRPC Current State

Stage 0 repository audit, performed 2026-08-18 on Ubuntu 24.04.4 LTS (WSL), starting at commit `09c8a12e1694d2a15f64928095a8c4e8717bd6d1`.

## Project identity

KaiRPC is an asynchronous C++17 RPC framework for Linux. It uses an epoll-based event loop, per-connection TCP buffers, a custom TinyPB wire format, Google Protobuf generic services, and a small TCP service-discovery center with registration, heartbeat, and TTL filtering. The checked-out repository does **not** contain a Raft implementation, consensus module, replicated log, or distributed database. Raft work is therefore out of scope for this repository and this stage.

The public README describes a working RPC quick start. Stage 1 verification now confirms that path builds and completes a bounded local loopback on the audited environment. Claims below are verified against source and the commands recorded in this document, not inferred from the README.

## Dependencies

- The source uses classic TinyXML 2.x: `#include <tinyxml.h>`, `TiXmlDocument`, `TiXmlElement`, and the `tinyxml` library. TinyXML2 is not compatible with these APIs.
- Protobuf is consumed through generated service/message headers and the generic service API. The repository expects a Protobuf compiler and development library; the audited host has `protoc`/`libprotobuf` 3.21.12 installed.
- `pthread` and the standard Linux socket/event libraries come from the normal compiler and libc development environment.
- On Ubuntu/Debian the classic TinyXML development package is `libtinyxml-dev`; this host has the required `tinyxml.h` header and `libtinyxml.so.2.6.2` runtime installed.

## Implemented

- CMake declares a static `kairpc` library from `common/`, `net/`, `net/tcp/`, `net/coder/`, and `net/rpc/` sources.
- `Eventloop`, `Fd_Event`, `Timer`, `IOThread`, and `IOThreadGroup` provide the intended reactor primitives.
- `TcpAcceptor`, `TcpServer`, `TcpClient`, `TcpConnection`, and `TcpBuffer` provide the TCP transport surface.
- TinyPB encoding and decoding support start/end markers, a package length, message ID, method name, error fields, serialized Protobuf body, and a checksum field.
- The decoder handles partial packets, multiple packets in one buffer, invalid package lengths, invalid end markers, and non-negative field bounds. The nominal maximum packet size is 10 MiB.
- `RpcChannel`, `RpcController`, `RpcDispatcher`, `RpcClosure`, and message-ID generation implement the intended Protobuf generic-service call path.
- The dispatcher registers local services, parses `Service.Method`, deserializes requests, invokes `Service::CallMethod`, serializes responses, and maps several errors into TinyPB response fields.
- A standalone service-discovery executable listens on query port 8080 and control port 9090 by default. Control commands include `add`, `modify`, `delete`, `heartbeat`, and `lookup`.
- Service discovery stores one address and `last_seen_ms` per method and lazily removes expired entries on lookup. The RPC dispatcher starts a detached heartbeat thread at approximately half the configured TTL.
- Protobuf-generated example files and three example executables (`rpc_server`, `rpc_client`, `tinypb_codec_test`) are described in `test/`; `scripts/generate_test_proto.sh` regenerates them with `protoc` and normalizes trailing whitespace; `test_interface` exercises the runtime discovery command helper.

## Partially implemented

- **Build and dependency integration:** CMake checks Protobuf, Threads, `tinyxml.h`, and `libtinyxml` during configuration. Fresh Debug, ASan, UBSan, and TSan configurations build the complete repository; there is still no registered CTest suite or dedicated sanitizer option in the project.
- **Reactor model:** The code uses level-triggered epoll by default; no `EPOLLET` is enabled. TCP reads attempt to drain until `EAGAIN`, but accepts, errors, half-close events, and shutdown are not handled consistently.
- **TCP lifetime:** `TcpConnection::clear()` removes the event registration and changes state but does not close the socket. `IOThreadGroup::~IOThreadGroup()` is empty, and event callbacks capture raw `this` pointers. Shutdown and restart ownership is not defined.
- **Writes:** The transport has an output buffer and attempts partial-write handling, but the write index is never advanced after `write(2)`, so pending bytes can be sent repeatedly. Completion callbacks are fired even when the connection did not prove that all bytes were delivered.
- **RPC timeout/cancellation:** A timer reports timeout and guards the user callback with `Finished()`, but the timer is not canceled on success, outstanding socket callbacks are not removed, and disconnects do not fail pending calls immediately.
- **Service discovery:** TTL and heartbeat exist, but the protocol is an unframed, single-read/single-write TCP command exchange with no request timeout, authentication, persistence, multi-instance routing, or lifecycle stop for heartbeat threads.
- **Logging/high availability:** Async logging, signal hooks, and a fork/exec restart helper exist, but the logger currently prints queued entries to stdout after opening a file, and the restart path is not a graceful or signal-safe shutdown design.

## Not implemented or not verified

- No Raft, consensus, replicated state, leader election, persistence, or distributed database functionality exists in this checkout.
- No automated test runner is configured: there is no `enable_testing()`, `add_test()`, GoogleTest/Catch2 target, or CI configuration.
- No verified end-to-end RPC test, reconnect test, failure-injection test, service-discovery TTL test, concurrent registry test, or load/stability benchmark is present.
- Checksum integrity is not implemented. The encoder writes the constant value `1` and the decoder only stores it.
- Protobuf body validity is delegated to the dispatcher; the wire decoder does not distinguish an invalid body until dispatch.
- Cancellation notification (`RpcController::NotifyOnCancel`) is empty.
- IPv6, TLS, authentication/authorization, backpressure limits, graceful draining, and durable service registration are absent.

## Architecture and runtime flow

The intended request path is:

```text
client call
  -> RpcChannel::CallMethod
  -> serviceDiscovery(query TCP connection)
  -> TcpClient / TcpConnection
  -> TinyPBCoder::encode
  -> TCP socket
  -> server main Eventloop accepts
  -> round-robin IOThread Eventloop owns TcpConnection
  -> TinyPBCoder::decode
  -> RpcDispatcher
  -> registered Protobuf Service::CallMethod
  -> RpcClosure serializes response
  -> TcpConnection output buffer / write event
  -> client decode and msg_id lookup
  -> RpcChannel callback / RpcController
```

### Threads and ownership

- The caller thread creates or obtains a thread-local `Eventloop` through `Eventloop::GetCurrentEventLoop()`.
- `TcpServer` uses that loop as a main reactor for the listening descriptor and creates `Config::m_io_threads` `IOThread` objects. Each IO thread constructs its own event loop and waits on a start semaphore.
- `TcpServer::onAccept()` accepts one connection, chooses the next IO thread, constructs a `TcpConnection`, and stores a `shared_ptr` in `m_client`. The `Fd_Event` used by the connection comes from the process-wide `FdEventGroup` and is indexed by file descriptor.
- Cross-thread epoll changes are queued as lambdas containing raw `Fd_Event*` and woken through an `eventfd`. There is no explicit owner or generation check for a queued event after close/reuse.
- `TcpConnection` owns its buffers and coder, but not the fd event or socket lifetime. The server set owns accepted connection objects; client callbacks and epoll callbacks capture raw `this`.
- `Timer` owns a multimap of shared timer events and is itself attached to the event loop through a timerfd. Repeated events are reinserted after expiration. The timer uses `CLOCK_MONOTONIC` for the fd but `gettimeofday()` for due times.
- `RpcChannel` owns the client, controller, messages, and closure through `shared_ptr`. It registers callbacks by message ID in `TcpConnection`; there is no duplicate-ID rejection or pending-call table cleanup on connection loss.
- The service center is a single process/single-thread event loop around two listening sockets. Its global `std::map` is not synchronized because all registry access currently occurs in that thread.

### Message IDs and callback flow

`MsgIDUtil` creates a 20-digit per-thread ID seeded from `/dev/urandom` and then increments it. A call can instead reuse the thread-local runtime message ID or an explicitly set controller ID. The client stores a response callback keyed by that string; an unknown response ID is silently ignored. `RpcChannel::callBack()` attempts an exactly-once guard with `RpcController::Finished()`, but missing-service and several disconnect paths return without invoking it.

## Correctness risks

### Critical

1. **Output data is not consumed after a write.** `TcpConnection::onWrite()` writes from `m_out_buffer` but never calls `moveReadIndex(rt)` (`net/tcp/tcp_connection.cc:178-211`). The buffer remains readable after a successful write, so data can be resent or completion can be reported while bytes remain queued.
2. **A full input-buffer read is rejected.** `TcpBuffer::moveWriteIndex()` rejects `j >= m_buffer.size()` (`net/tcp/tcp_buffer.cc:105-112`). `TcpConnection::onRead()` calls it after reading exactly the available capacity (`net/tcp/tcp_connection.cc:80-93`), leaving the write index unchanged even though the kernel consumed bytes and corrupting subsequent framing.
3. **Raw callback lifetime is unchecked.** Event-loop tasks, fd callbacks, timers, RPC callbacks, and the detached heartbeat capture raw `this` or continue after their owner is destroyed (`net/eventloop.cc:233-250`, `net/tcp/tcp_connection.cc:257-277`, `net/rpc/rpc_channel.cc:139-212`, `net/rpc/rpc_dispatcher.cc:41-65`). There is no cancellation barrier before object destruction, making use-after-free during shutdown/reconnect plausible.

### High

1. **Socket ownership and close are incomplete.** `TcpConnection::clear()` unregisters the descriptor but never closes it (`net/tcp/tcp_connection.cc:222-234`); `TcpConnection`'s destructor also does not close it. The server therefore leaks accepted fds, and stale epoll events can target reused descriptors. `TcpClient` closes its fd independently in its destructor (`net/tcp/tcp_client.cc:40-45`), creating separate ownership paths.
2. **Error and half-close events are mishandled.** `Eventloop::loop()` checks `EPOLLERR` only, ignores `EPOLLHUP`/`EPOLLRDHUP`, and queues the `OUT_EVENT` handler instead of the `ERROR_EVENT` handler (`net/eventloop.cc:158-176`). Peer shutdown can therefore leave pending RPCs unresolved.
3. **Connect failure does not establish a valid replacement connection.** On asynchronous failure, `TcpClient` closes `m_fd` and creates a new socket without updating the `Fd_Event`, setting nonblocking mode, or registering the new descriptor (`net/tcp/tcp_client.cc:61-90`). The callback is still invoked, and no retry policy exists.
4. **The RAII mutex wrapper unlocks twice.** `ScopeMutex::unlock()` does not clear `m_is_lock`, so the destructor calls `unlock()` again (`common/mutex.h:29-33`, `common/mutex.h:18-20`). The code manually calls `unlock()` throughout the reactor, timer, and logger. Return codes are ignored; depending on mutex state this creates undefined/error-prone unlock behavior and race windows.
5. **RPC discovery failure can terminate the process or skip the callback.** `serviceDiscovery()` calls `exit(1)` on socket/connect/read errors (`net/service_discovery/service_discovery.h:15-43`), while `RpcChannel::CallMethod()` returns immediately for `"unknown host"` without setting a controller error or invoking `done` (`net/rpc/rpc_channel.cc:51-58`).
6. **The service center leaves query sockets registered after use.** Query clients are not removed from epoll after a response, and the `recv <= 0` path closes without `EPOLL_CTL_DEL` (`net/service_discovery/run_server_discovery.cc:305-341`). Descriptor reuse can produce stale events.
7. **Shutdown leaks threads and resources.** `IOThreadGroup::~IOThreadGroup()` is empty (`net/iothreadgroup.cc:15-17`); `TcpServer` deletes its main loop before the IO-thread group and does not provide a coordinated stop/drain sequence (`net/tcp/tcp_server.cc:27-40`). Timerfds, eventfds, and accepted sockets are not consistently closed.

### Medium

1. TinyPB preserves but does not verify checksum (`net/coder/tinypb_coder.cc:224-230`, `net/coder/tinypb_coder.cc:326-329`). Empty/malformed method and message IDs are accepted by the wire decoder; oversized individual string lengths are only bounded indirectly by package length.
2. Public `TcpBuffer` methods accept signed sizes without rejecting negative values (`net/tcp/tcp_buffer.cc:37-45`, `net/tcp/tcp_buffer.cc:48-61`), and `resizeBuffer()`/capacity arithmetic is not overflow-checked.
3. `TcpConnection::onWrite()` does not handle `write()` returning zero or errors other than `EAGAIN`, and callback completion is not tied to a specific byte range. `TcpAcceptor` can construct a connection after `accept()` failure (`net/tcp/tcp_acceptor.cc:56-68`, `net/tcp/tcp_server.cc:64-76`).
4. `FdEvent::listen()` mutates the error callback by toggling it to null on later calls (`net/fd_event.cc:43-58`), and `FdEventGroup::getFdEvent()` has no negative-fd guard (`net/fd_event_group.cc:44-58`).
5. Timer due times use wall-clock `gettimeofday()` while timerfd uses monotonic time (`net/timer.cc:22-30`, `net/timer.cc:49`, `net/timer_event.cc:20-24`), so clock adjustments can delay or accelerate callbacks.
6. `Logger::syncLoop()` unlocks `lock` instead of `lock2` (`common/log.cc:93-100`), and the async logger prints to stdout rather than writing queued entries to its opened file (`common/log.cc:268-290`). Stop does not wake a thread blocked in `pthread_cond_wait` (`common/log.cc:301-303`).
7. `IPNetAddr` accepts port 65536, which wraps when stored in `uint16_t`, and uses `inet_addr()` with limited validation (`net/tcp/net_addr.cc:15-31`, `net/tcp/net_addr.cc:77-89`).

### Low

1. Naming, comments, and APIs retain the original TalonRPC terminology and typos (`talon` namespace, `cancle`, `excute`), increasing maintenance cost.
2. CMake uses broad `aux_source_directory()` discovery, empty subdirectory CMake files, unqualified link names, and no install/package metadata.
3. The high-availability helper uses non-async-signal-safe work in signal handlers and attempts to handle `SIGKILL`, which cannot be caught (`common/high_availability.h:17-30`).

## Testing gaps

- `test/test_tinypb_coder.cc` is a standalone `assert` program, not a registered test. It covers one round-trip, partial input, sticky packets, and an oversized length header, but not checksum, all malformed field combinations, negative sizes, allocation/capacity edges, or concurrent use.
- `test/test_rpc_server.cc` and `test/test_rpc_client.cc` are manual examples. They require live service discovery, three processes, and fixed local ports. The server example uses a fork/exec restart helper; it is not an automated integration test.
- `test/test_rpc_client.cc` exercises neither unknown service behavior nor connection failure, timeout, peer close, duplicate message IDs, cancellation, or callback exactly-once semantics.
- There are no tests for EventLoop cross-thread task ordering, fd reuse, HUP/RDHUP/ERR, partial writes, shutdown, IO-thread teardown, timer cancellation, logger shutdown, or service-center TTL expiration.
- There are no race, load, fuzz, protocol differential, or long-running heartbeat tests.

## Build baseline

Environment:

```text
OS: Ubuntu 24.04.4 LTS (WSL)
Compiler: g++ 13.3.0
CMake: 3.28.3
Protobuf compiler/library: protoc 3.21.12 / libprotobuf.so.32
TinyXML runtime: libtinyxml.so.2.6.2 present; tinyxml.h development header present
```

Commands and results:

```text
cmake -S . -B build-stage1-final4 -DCMAKE_BUILD_TYPE=Debug       PASS
cmake --build build-stage1-final4 -j$(nproc)                    PASS
./scripts/build.sh build-stage1-final4-script                     PASS
./build-stage1-final4/bin/tinypb_codec_test                       PASS
service-discovery registration/query smoke                         PASS
RPC server/client loopback smoke                                    PASS
```

The `rpc_service_discovery` target receives `${PROJECT_SOURCE_DIR}/common` privately from `test/CMakeLists.txt`, which resolves its direct `config_reader.h` include without broadening global include paths. The canonical Protobuf command is `./scripts/generate_test_proto.sh`; it uses `protoc 3.21.12`, strips trailing whitespace from generated outputs, and reproduces the committed files exactly.

The project does not enable `-Wall`, `-Wextra`, or `-Wpedantic`. The Debug build retains existing `-Wformat-security` warnings for dynamic strings passed to `printf`/`snprintf` in `common/log.cc` and `common/log.h`, plus the existing `sprintf` format-overflow warning in `common/high_availability.h:80`.

## Sanitizer baseline

```text
ASan full-project configure/build: PASS (build-stage1-final-asan3)
ASan TinyPB/RPC/service-discovery runtime: PASS, no diagnostic
UBSan full-project configure/build: PASS (build-stage1-final-ubsan3)
UBSan TinyPB/RPC/service-discovery runtime: PASS, no diagnostic
TSan full-project configure/build: PASS (build-stage1-final-tsan3)
TSan service-discovery runtime: WSL RUNTIME BLOCKED, exit 66: "FATAL: ThreadSanitizer: unexpected memory mapping ..."
```

The TSan failure is environmental in this WSL run and is not evidence that the application is race-free.

## Technical debt

- Production and test code share global singletons (`Config`, `Logger`, `Eventloop`, `FdEventGroup`, `RpcDispatcher`) with process/thread-local state and no reset contract.
- Raw pointers are used at all major ownership boundaries: fd events, sockets, loops, Protobuf messages, closures, timers, and callbacks.
- The transport has no explicit connection state machine for connecting, connected, draining, closed, or failed; `TcpConnection::setState()` ignores its argument and always sets `Connected` (`net/tcp/tcp_connection.cc:214-216`).
- Error codes and callback outcomes are not a single documented contract. Several paths log, exit, return, or invoke the callback inconsistently.
- Service discovery is a plaintext control plane bound to all interfaces, with no framing, limits, authentication, persistence, or multi-node semantics.
- Generated Protobuf sources are committed; `scripts/generate_test_proto.sh` documents the `protoc 3.21.12` generator and trailing-whitespace normalization policy.

## Recommended repository-specific upgrade stages

1. **Dependency and reproducible baseline:** declare TinyXML/Protobuf/pthread dependencies, add a documented Linux build matrix, preserve this audit, and make build output deterministic.
2. **Deterministic automated tests:** add CTest and focused unit targets for `TcpBuffer`, `Fd_Event`, timers, message IDs, TinyPB, controller state, and service-discovery command parsing. Add a loopback RPC smoke test once the build is unblocked.
3. **TinyPB hardening:** define integer/field limits, validate checksum or remove the field, reject empty/invalid IDs and methods, fuzz the decoder, and test fragmentation/coalescing and malformed packets.
4. **Transport correctness:** fix buffer index advancement, partial writes, zero/error writes, accept draining, HUP/RDHUP/ERR handling, nonblocking setup, and explicit close-once semantics. Add fd-reuse and failure-injection tests.
5. **Ownership and shutdown:** replace raw callback captures with lifetime-safe cancellation or weak ownership, define EventLoop/IOThread/TcpConnection ownership, close all descriptors, and implement coordinated server/client stop and drain.
6. **RPC semantics:** make discovery/connect failures return controller errors, guarantee callback exactly once, reject/handle duplicate and unknown IDs, cancel timers and pending operations, and define timeout/cancellation/disconnect behavior.
7. **Service discovery robustness:** frame and bound control/query messages, add I/O deadlines, validate addresses, support multiple nodes per method with deterministic selection, make heartbeat stoppable, and test TTL/restart/concurrency behavior.
8. **Observability and safety:** repair async logging/file rotation, remove unsafe signal-handler work, add structured metrics and error context, and document the security boundary of the control plane.
9. **Sanitizers, CI, and stability:** run ASan/UBSan/TSan in supported CI environments, add protocol fuzzing, long-running heartbeat/RPC tests, load tests, and performance benchmarks only after correctness gates pass.
