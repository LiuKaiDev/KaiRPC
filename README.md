# KaiRPC

KaiRPC 是一个基于 Linux epoll 和 Protobuf 实现的轻量级异步 RPC 框架，支持服务注册发现、远程方法调用、异步日志、定时器和服务异常重启。

## Features

- 基于 epoll 实现网络事件监听与分发
- 封装 EventLoop、TcpServer、TcpConnection 等网络组件
- 基于 Protobuf 完成请求参数和响应结果的序列化 / 反序列化
- 设计自定义 RPC 通信协议，支持请求响应匹配和错误处理
- 支持服务注册、服务发现和运行时服务上下线
- 支持异步日志、定时器和服务异常重启

## Architecture

KaiRPC 主要由以下模块组成：

- RpcClient / RpcChannel：客户端远程调用入口
- TcpClient / TcpServer：TCP 网络通信
- EventLoop：基于 epoll 的事件循环
- RpcDispatcher：服务端 RPC 请求分发
- Protobuf Codec：请求和响应序列化
- Service Discovery：服务注册与发现
- Logger / Timer：异步日志和定时器支持

详细架构说明见 [docs/architecture.md](docs/architecture.md)。

## Directory

- `common/`：配置、日志、工具类、高可用等公共模块
- `conf/`：服务端、客户端、服务发现中心配置文件
- `net/`：事件循环、TCP 网络层、定时器等核心网络模块
- `net/rpc/`：RPC 调用、分发、控制器、协议封装
- `net/service_discovery/`：服务注册发现相关代码
- `test/`：示例服务端、客户端和 Protobuf 文件
- `scripts/`：构建脚本

## Environment

- Linux
- C++17
- CMake
- Protobuf
- pthread
- tinyxml

## Build

```bash
./scripts/build.sh
```

也可以手动构建：

```bash
mkdir -p build
cd build
cmake ..
make -j"$(nproc)"
```

构建完成后，可执行文件位于 `build/bin/`，静态库位于 `build/lib/`。

## Configuration

- 服务端配置：`conf/kairpc.xml`
- 客户端配置：`conf/kairpc_client.xml`
- 服务发现中心配置：`conf/service_center.conf`

默认配置使用 `127.0.0.1`，适合本机启动服务发现中心、RPC 服务端和 RPC 客户端进行联调。

## Example

从项目根目录启动服务发现中心：

```bash
./build/bin/rpc_service_discovery
```

启动示例 RPC 服务端：

```bash
./build/bin/rpc_server
```

启动示例 RPC 客户端：

```bash
./build/bin/rpc_client
```

示例 Protobuf 定义位于 `test/order.proto`，服务端和客户端示例分别位于 `test/test_rpc_server.cc` 与 `test/test_rpc_client.cc`。
