# TinyPB Protocol

KaiRPC 的 RPC 请求和响应使用 TinyPB 二进制协议承载。协议通过固定起止标记和包总长度描述一个完整消息，字段中的整数均使用网络字节序。

## Packet Format

| Field | Size | Description |
| --- | ---: | --- |
| start flag | 1 byte | 包起始标记，当前为 `0x02` |
| package length | 4 bytes | 整包长度，包含 start flag、package length、checksum 和 end flag |
| msg_id length | 4 bytes | `msg_id` 字节长度 |
| msg_id | variable | 请求 ID，用于请求和响应匹配 |
| method_name length | 4 bytes | `method_name` 字节长度 |
| method_name | variable | RPC 方法全名，例如 `Order.makeOrder` |
| error_code | 4 bytes | RPC 错误码，`0` 表示调用成功 |
| error_info length | 4 bytes | `error_info` 字节长度 |
| error_info | variable | 错误描述文本 |
| protobuf serialized body | variable | Protobuf 序列化后的请求或响应 body |
| checksum | 4 bytes | 校验和字段，当前保留但尚未做真实校验 |
| end flag | 1 byte | 包结束标记，当前为 `0x03` |

当前协议没有单独的 `protobuf body length` 字段。解码时通过 `package length` 减去固定字段、字符串字段、checksum 和 end flag 后推导 body 长度。

## TCP Sticky And Partial Packets

TCP 是字节流协议，可能出现粘包和半包。TinyPB 使用 `package length` 处理这两类情况：

- 半包：如果当前 buffer 中从 start flag 开始的数据不足 `package length`，解码器不会移动 read index，会等待更多数据到达。
- 粘包：如果 buffer 中包含多个完整 TinyPB 包，解码器会在解析完一个包后移动 read index，并继续解析后续包。
- 非法包：如果包长度小于最小包长、超过最大包长、end flag 不匹配或字段长度越界，解码器会丢弃异常包头或完整异常包，避免在同一位置死循环。

当前最大包长限制为 10 MB。

## Request And Response Matching

`msg_id` 是一次 RPC 调用的唯一标识。客户端发起请求时写入 `msg_id`，服务端响应时回传相同的 `msg_id`，客户端据此匹配对应的 pending call 并触发回调。

## Method Dispatch

`method_name` 保存 RPC 方法全名。服务端收到请求后，`RpcDispatcher` 会把方法全名拆分为 service 名称和 method 名称，然后在已注册的 Protobuf Service 中查找并执行对应方法。

## Error Handling

`error_code` 和 `error_info` 用于表达 RPC 调用失败：

- `error_code == 0`：调用成功，body 中保存正常响应。
- `error_code != 0`：调用失败，`error_info` 保存失败原因，客户端会把错误写入 `RpcController`。

## Checksum

TinyPB 包中保留了 `checksum` 字段。当前实现会编码和解码该字段，但尚未执行真实校验。后续可以补充一致的 checksum 算法，并在 encode 和 decode 两端同时启用。
