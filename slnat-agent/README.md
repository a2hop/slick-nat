# SlickNat Agent - C++ Implementation

This directory contains the C++ implementation of the SlickNat agent daemon and client.

## Components

- `net-agent/slnat-daemon.cpp` - Main daemon that reads NAT mappings from kernel module
- `net-client/slnatc.cpp` - Network client for remote querying
- `CMakeLists.txt` - CMake build configuration
- `build.sh` - Build script for easy compilation

## Building

### Quick Build

```bash
./build.sh
```

### Manual Build

```bash
mkdir build
cd build
cmake ..
make
```

### Build Options

```bash
# Build in debug mode
./build.sh --debug

# Clean build
./build.sh --clean

# Build and install
./build.sh --install

# Use specific number of parallel jobs
./build.sh --jobs 4

# Show help
./build.sh --help
```

## Dependencies

- C++17 compatible compiler
- CMake 3.10+
- pthread
- Internet connection (for downloading nlohmann/json during build)

The build system automatically downloads and includes the nlohmann/json library, so no additional JSON library installation is required.

## Usage

### Daemon

```bash
./build/slick-nat-daemon [--address ADDR] [--port PORT] [--proc PATH]
```

Options:
- `--address ADDR`: IPv6 address to listen on (default: ::1)
- `--port PORT`: Port to listen on (default: 7001)  
- `--proc PATH`: Kernel proc file path (default: /proc/net/slick_nat_mappings)

Examples:
```bash
# Listen on all interfaces
./build/slick-nat-daemon --address :: --port 7001

# Listen on specific address (like 7000::1)
./build/slick-nat-daemon --address 7000::1 --port 7001
```

### Network Client (slnatc)

```bash
./build/slnatc <daemon_prefix> <command> [options]
```

Commands:
- `get2kip [ip]` - Get global unicast IP for local/specified IP
- `resolve <ip>` - Resolve IP address mapping  
- `ping` - Ping the daemon

Examples:
```bash
# Get global IP for local address in 7000:: network
./build/slnatc 7000 get2kip

# Get global IP for specific address
./build/slnatc 7000 get2kip 7000::100

# Resolve specific IP
./build/slnatc 7000 resolve 2001:db8::1

# Ping daemon at 7000::1
./build/slnatc 7000 ping

# Connect to different network
./build/slnatc 8000 get2kip
```

## Features

- **Network-accessible daemon**: Listens on IPv6 TCP socket for remote access
- **Multi-network support**: Client can connect to different SlickNat networks
- **Automatic address expansion**: `slnatc 7000` connects to `7000::1`
- **Global IP resolution**: `get2kip` command specifically for 2000::/3 mappings
- **Real kernel integration**: Reads actual NAT mappings from kernel module
- **Automatic mapping reload**: Reloads mappings every 5 seconds
- **Proper IPv6 handling**: Implements correct prefix matching and address remapping
- **Multi-threaded**: Handles multiple concurrent client connections
- **Signal handling**: Clean shutdown on SIGINT/SIGTERM

## Protocol

The daemon uses a JSON-based RPC protocol over IPv6 TCP:

### Request Format
```json
{
    "command": "get2kip",
    "ip": "7000::100"
}
```

### Response Format
```json
{
    "internal_ip": "7000::100",
    "global_ip": "2001:db8::100",
    "interface": "eth0",
    "status": "success"
}
```

### Error Response
```json
{
    "error": "No global unicast mapping found",
    "status": "not_found"
}
```

## Network Architecture

The typical setup involves:
1. **Daemon Host**: Runs the kernel module and daemon (e.g., at 7000::1)
2. **Client Hosts**: Connect to daemon from anywhere in the network
3. **Multiple Networks**: Each SlickNat network has its own daemon prefix

```
Network 7000::/64:
├── 7000::1 (daemon)
├── 7000::2 (client)
└── 7000::100 (client)

Network 8000::/64:
├── 8000::1 (daemon)  
├── 8000::2 (client)
└── 8000::200 (client)
```

## Installation

After building, you can install the binaries system-wide:

```bash
# Install to /usr/local/bin
./build.sh --install

# Or manually
cd build
sudo make install
```

## Integration

The daemon integrates with the SlickNat kernel module by:
1. Reading mappings from `/proc/net/slick_nat_mappings`
2. Parsing the mapping format: `interface internal_prefix/len -> external_prefix/len`
3. Building internal lookup tables for efficient IP resolution
4. Automatically reloading mappings when they change
5. Providing remote access via IPv6 TCP for network clients

This provides a clean userspace interface for applications across the network to query NAT mappings without needing direct kernel module access.
