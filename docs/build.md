# KaiRPC Build Guide

## Supported environment

The validated environment is Ubuntu 24.04.4 LTS under WSL2, with GCC/G++ 13.3, CMake 3.28, and Protobuf 3.21.12. Other Linux distributions may work, but their package names and default TinyXML locations can differ.

KaiRPC uses the classic TinyXML 2.x API and library:

```cpp
#include <tinyxml.h>
```

TinyXML2 is not interchangeable with this dependency.

## Required packages

On Ubuntu/Debian, install the compiler, build tools, Protobuf development files, and classic TinyXML development package:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git protobuf-compiler libprotobuf-dev libtinyxml-dev
```

`pthread` and the standard Linux networking libraries are provided by the toolchain; no separate pthread package is required.

The CMake configure step checks for `tinyxml.h`, `libtinyxml`, Protobuf, and Threads. If TinyXML is missing, configuration stops with the package command above.

## Debug build

Use a fresh build directory for a clean baseline:

```bash
cmake -S . -B build-stage1 -DCMAKE_BUILD_TYPE=Debug
cmake --build build-stage1 -j"$(nproc)"
```

The repository script accepts an optional build directory and runs the same configure/build sequence:

```bash
./scripts/build.sh build-stage1-script
```

Executables are written to `build-stage1/bin/`; the static library is written to `build-stage1/lib/`.

## Release build

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j"$(nproc)"
```

## Existing smoke paths

After a successful full build, the standalone codec assertion program can be run with:

```bash
./build-stage1/bin/tinypb_codec_test
```

The service-discovery executable can be started with a bounded smoke run:

```bash
timeout 2s ./build-stage1/bin/rpc_service_discovery
```

It listens on the configured query/control ports. `test_interface` performs a runtime `lookup` and requires the service center to be running. `rpc_server` and `rpc_client` are manual multi-process examples, not automated tests; start them only after reviewing their fixed local ports and configuration.

There is currently no registered CTest suite.

## Sanitizers

Sanitizers are opt-in and do not change the release build:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build-asan -j"$(nproc)"

cmake -S . -B build-ubsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=undefined"
cmake --build build-ubsan -j"$(nproc)"
```

The Stage 0 WSL2 environment could compile a TSan target but failed to start it with `FATAL: ThreadSanitizer: unexpected memory mapping`. Treat TSan as environment-blocked on this host until it is run in a supported CI or Linux environment.
