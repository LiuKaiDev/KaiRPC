# KaiRPC Architecture

KaiRPC 的核心流程可以分为服务注册、服务发现、请求编码、网络传输、服务分发和响应回写。

1. RPC 服务端启动后读取 `conf/kairpc.xml` 和 `conf/service_center.conf`。
2. 服务端通过 `RpcDispatcher` 注册 Protobuf Service，并把方法名与服务地址注册到服务发现中心。
3. RPC 客户端通过 `RpcChannel` 发起远程调用，先向服务发现中心查询目标方法所在地址。
4. 客户端将方法名、消息 ID、序列化后的 Protobuf 请求体封装为 TinyPB 协议包。
5. TCP 网络层通过 `EventLoop`、`TcpClient`、`TcpConnection` 完成异步连接、写入和响应读取。
6. 服务端收到请求后由 `RpcDispatcher` 解析服务名和方法名，反序列化请求并调用对应 Protobuf Service。
7. 服务端将响应序列化后写回客户端，客户端根据消息 ID 匹配响应并执行回调。

## Service Discovery And TTL

服务发现中心维护一张方法名到服务节点地址的内存表。RPC 服务端启动后，`RpcDispatcher` 会把每个 Protobuf RPC 方法注册到服务发现中心；注册内容包含方法全名和服务端 `ip:port`。

每个服务节点会记录 `last_seen_ms`。服务端注册成功后会按 `service_ttl_ms / 2` 的间隔发送轻量 heartbeat；服务端重复执行注册，或通过运行时管理接口发送 `heartbeat` 命令时，也会刷新该节点的 `last_seen_ms`。TTL 由 `conf/service_center.conf` 中的 `service_ttl_ms` 控制，默认值为 `10000` 毫秒。

客户端查询方法地址时，服务发现中心会检查节点是否超过 TTL。过期节点不会被返回，并会在查询或运行时 `lookup` 时被惰性清理。这样即使服务进程异常退出，只要它不再重复注册或发送心跳，后续查询也不会继续返回这个失效节点。

主要模块：

- `common/`：配置读取、日志、运行时上下文、高可用辅助逻辑
- `net/`：epoll 事件循环、文件描述符事件、定时器、IO 线程
- `net/tcp/`：TCP 地址、连接、客户端、服务端和缓冲区
- `net/coder/`：TinyPB 协议对象与编解码
- `net/rpc/`：RPC Channel、Dispatcher、Controller、Closure 和消息 ID
- `net/service_discovery/`：服务发现中心和运行时管理接口
