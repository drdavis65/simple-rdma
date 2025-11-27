# Simple RDMA

A simple RDMA example using Unreliable Connection (UC) Queue Pairs with RDMA Write with Immediate operations.

## Building

### Using CMake (Recommended)

```bash
mkdir build
cd build
cmake ..
make
```

The executables will be created in the `build/` directory:
- `build/sender` - RDMA sender program
- `build/receiver` - RDMA receiver program

### Build Options

- **Debug build**: `cmake -DCMAKE_BUILD_TYPE=Debug ..`
- **Release build**: `cmake -DCMAKE_BUILD_TYPE=Release ..` (default)

### Using the old compile script

Alternatively, you can use the simple compile script:
```bash
cd src
./compile.sh
```

## Running

1. Start the receiver:
```bash
./receiver
```

2. In another terminal, start the sender:
```bash
./sender
```

## Features

- UC (Unreliable Connection) QP type
- RDMA Write with Immediate operations
- RoCE v2 support
- Connection establishment via TCP
- Memory region registration and remote access

## Requirements

- libibverbs development libraries
- InfiniBand or RoCE capable hardware
- CMake 3.10 or later (for CMake build)

