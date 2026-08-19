# RPC Completion Semantics

Each logical client RPC attempt is owned by one request state. The state starts
pending and becomes terminal on the first of: successful response, connect
failure, transport disconnect, timeout, or explicit cancellation.

Terminalization is centralized and idempotent. It removes the pending response
callback, invalidates the timeout event, records the controller error when the
result is not success, and runs the user `Closure` once after internal cleanup.

- A successful response leaves `RpcController::Failed()` false.
- Connect refusal and peer disconnect fail the controller with the existing
  transport error codes.
- A deadline fails with `ERROR_RPC_CALL_TIMEOUT`.
- `StartCancel()` fails with `ERROR_RPC_CALL_CANCELLED`; `NotifyOnCancel()` is
  invoked when registered.
- A response that arrives after timeout or cancellation is ignored.
- A transport error after a successful response cannot rewrite the success.

KaiRPC guarantees at-most-once user completion per logical RPC attempt. This is
not exactly-once remote execution: the server-side business method may already
have run when a client times out or disconnects.
