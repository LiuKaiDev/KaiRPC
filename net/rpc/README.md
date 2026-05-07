# RPC Module

`net/rpc/` 包含 KaiRPC 的远程调用核心组件：

- `RpcChannel`：客户端调用入口，负责服务发现、请求组包、发送和响应回调。
- `RpcDispatcher`：服务端请求分发器，负责定位 Protobuf Service 与 Method。
- `RpcController`：记录调用状态、错误码、超时和消息 ID。
- `RpcClosure`：封装异步回调逻辑。
- `msg_id_util`：生成 RPC 请求消息 ID。
