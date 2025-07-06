# Slick NAT - Bidirectional IPv6 NAT Kernel Module

A high-performance IPv6 NAT kernel module that provides bidirectional address translation with dynamic mapping configuration and per-namespace support.

## Features

- **Bidirectional IPv6 NAT**: Seamless translation between internal and external IPv6 address spaces
- **Dynamic Configuration**: Add/remove mappings without module reload through proc filesystem
- **Batch Processing**: Efficiently apply multiple NAT rules at once for improved performance
- **Per-Network Namespace Support**: Isolated NAT instances for containers and virtual environments
- **Container Support**: Works with LXD, Docker, and other containerization platforms
- **Neighbor Discovery Proxy**: Automatic NDP response for external prefixes
- **ICMP Error Handling**: Proper translation of embedded packets in ICMP error messages
- **TTL/Hop Limit Management**: Generates appropriate time exceeded messages for traceroute support
- **Interface-Specific Mappings**: Different NAT rules per network interface
- **High Performance**: Radix tree-based lookups for O(log n) mapping resolution

## Version

Current version: **0.0.3** (semver compatible)

## Architecture

The module operates at the netfilter PRE_ROUTING and POST_ROUTING hooks, intercepting IPv6 packets and performing address translation based on configured prefix mappings. It maintains separate mapping tables for each network namespace with optimized radix tree lookups.

### Key Components

- **NAT Engine**: Core translation logic with bidirectional address remapping
- **NDP Proxy**: Handles neighbor solicitation/advertisement for external prefixes
- **ICMP Processor**: Translates embedded packets in ICMP error messages
- **Mapping Manager**: Dynamic configuration through proc filesystem interface
- **Radix Tree Index**: Fast O(log n) prefix lookups for high performance

## Installation

### Prerequisites

- Linux kernel 4.14 or later
- Kernel headers for your running kernel
- GCC compiler
- Make utility

### Method 1: Direct Build and Install

```bash
# Clone or extract the source
cd ~/slick-nat

# Build the module
make

# Load the module
sudo insmod src/slick_nat.ko

# Verify loading
lsmod | grep slick_nat
```

### Method 2: DKMS Installation (Recommended)

```bash
# Install using DKMS for automatic kernel updates
cd dkms
sudo ./install.sh

# Verify installation
sudo dkms status
lsmod | grep slick_nat
```

### Method 3: Debian Package Installation

```bash
# Build debian package
cd pkg/deb
./build-deb.sh

# Install package
sudo dpkg -i build/slick-nat-dkms-0.0.3_amd64.deb

# Install missing dependencies if needed
sudo apt-get install -f
```

### Automatic Loading

```bash
# Enable automatic loading on boot
echo 'slick_nat' | sudo tee -a /etc/modules-load.d/slick-nat.conf

# Load module with parameters (optional)
echo 'options slick_nat param=value' | sudo tee /etc/modprobe.d/slick-nat.conf
```

## Configuration

### Management Script

Use the provided `slnat` script for easy management:

```bash
# Make executable (if building from source)
chmod +x src/slnat

# Add mapping
sudo slnat eth0 add 2001:db8:internal::/64 2001:db8:external::/64

# List mappings
sudo slnat eth0 list

# Remove mapping
sudo slnat eth0 del 2001:db8:internal::/64

# Check module status
slnat status

# Batch operations
slnat create-template /tmp/nat-config.txt
slnat add-batch /tmp/nat-config.txt
slnat del-batch /tmp/nat-delete.txt
```

### Direct Proc Interface

```bash
# Add mapping
echo "add eth0 2001:db8:internal::/64 2001:db8:external::/64" | sudo tee /proc/net/slick_nat_mappings

# Remove mapping
echo "del eth0 2001:db8:internal::/64" | sudo tee /proc/net/slick_nat_mappings

# View mappings
cat /proc/net/slick_nat_mappings

# Batch operations
cat batch-file.txt | sudo tee /proc/net/slick_nat_batch
```

## Container Support

Slick NAT works with containerized environments including LXD and Docker. The kernel module runs on the host, while containers can access the configuration interface.

### LXD Configuration

```bash
# Configure container for NAT access
sudo ./containers/lxd-config.sh container-name

# Use NAT from within container
lxc exec container-name -- slnat eth0 add 2001:db8:internal::/64 2001:db8:external::/64
```

### Docker Support

```bash
# Run container with NAT access
docker run -it --privileged \
  -v /proc/net/slick_nat_mappings:/proc/net/slick_nat_mappings \
  ubuntu:20.04 bash

# Configure NAT from within container
slnat eth0 add 2001:db8:internal::/64 2001:db8:external::/64
```

See [Container-Support.md](containers/Container-Support.md) for detailed configuration instructions.

## Usage Examples

### Basic NAT Setup

```bash
# Map internal network to external addresses
sudo slnat eth0 add 2001:db8:internal::/64 2001:db8:external::/64

# Internal hosts with addresses 2001:db8:internal::1, 2001:db8:internal::2, etc.
# will appear as 2001:db8:external::1, 2001:db8:external::2, etc. externally
```

### Batch Configuration

```bash
# Create a template configuration file
sudo slnat create-template /tmp/nat-rules.txt

# Edit the file with your preferred text editor
sudo nano /tmp/nat-rules.txt

# Apply multiple rules at once
sudo slnat add-batch /tmp/nat-rules.txt

# Create and apply deletion batch
echo "del eth0 2001:db8:internal:1::/64" > /tmp/del-rules.txt
echo "del eth0 2001:db8:internal:2::/64" >> /tmp/del-rules.txt
sudo slnat del-batch /tmp/del-rules.txt
```

### Multi-Interface Configuration

```bash
# Different mappings for different interfaces
sudo slnat eth0 add 2001:db8:lan::/64 2001:db8:wan1::/64
sudo slnat eth1 add 2001:db8:lan::/64 2001:db8:wan2::/64
```

### Container/Namespace Support

```bash
# Each network namespace has its own mapping table
sudo ip netns exec container1 slnat veth0 add 2001:db8:cont1::/64 2001:db8:pub1::/64
sudo ip netns exec container2 slnat veth0 add 2001:db8:cont2::/64 2001:db8:pub2::/64

# Or use with LXD containers
lxc exec container1 -- slnat eth0 add 2001:db8:cont1::/64 2001:db8:pub1::/64
```

## Network Topology Examples

### Simple NAT Gateway

```
Internal Network: 2001:db8:internal::/64
External Network: 2001:db8:external::/64

[Internal Hosts] ── [NAT Gateway] ── [External Network]
   ::1, ::2, ::3        eth0              Internet
```

### Multi-Homed NAT

```
Internal: 2001:db8:internal::/64
External1: 2001:db8:wan1::/64 (eth0)
External2: 2001:db8:wan2::/64 (eth1)

[Internal Network] ── [NAT Gateway] ── [ISP1]
                           │
                           └─────────── [ISP2]
```

### Container-Based NAT

```
Host Network: 2001:db8:host::/64
Container1: 2001:db8:cont1::/64 → 2001:db8:pub1::/64
Container2: 2001:db8:cont2::/64 → 2001:db8:pub2::/64

[Host] ── [Container1] ── [NAT] ── [Public1]
    └─── [Container2] ── [NAT] ── [Public2]
```

## Performance Characteristics

### Lookup Performance
- **Radix Tree Implementation**: O(log n) lookup time
- **Tail Segment Optimization**: Fast path for /112, /96, /80, /64 prefixes
- **Scalability**: Handles thousands of mappings efficiently

### Memory Usage
- **Base Module**: ~50KB kernel memory
- **Per Mapping**: ~200 bytes (including radix tree overhead)
- **Per Namespace**: ~1KB for management structures

### Throughput
- **Zero-copy Translation**: No packet data copying
- **Interrupt Context Safe**: Processes packets in softirq context
- **SMP Scalable**: Lock-free radix tree reads with RCU protection

## Performance Considerations

- **Batch Operations**: Apply multiple rules in a single transaction for improved performance
- **Optimized Lookups**: Radix tree implementation provides O(log n) performance
- **Tail Segment Priority**: Optimized for /112, /96, /80 prefixes commonly used in modern deployments
- **Memory Efficiency**: Minimal per-mapping overhead with shared tree structures
- **Lock-free Reads**: RCU-protected mapping tables for high throughput
- **NUMA-aware**: Proper memory allocation for multi-socket systems
- **Interrupt Context Safe**: Can process packets in softirq context
- **SMP Scalable**: Concurrent packet processing across multiple CPUs

## Troubleshooting

### Common Issues

1. **Module fails to load**
   ```bash
   # Check kernel logs
   dmesg | tail -20
   
   # Verify kernel version compatibility
   uname -r
   
   # Check for missing dependencies
   sudo apt install linux-headers-$(uname -r)
   ```

2. **Mappings not working**
   ```bash
   # Verify mappings are loaded
   cat /proc/net/slick_nat_mappings
   
   # Check interface names
   ip link show
   
   # Verify IPv6 forwarding is enabled
   sysctl net.ipv6.conf.all.forwarding
   ```

3. **Container access issues**
   ```bash
   # Check if proc file is accessible in container
   lxc exec container -- ls -la /proc/net/slick_nat_mappings
   
   # Verify container configuration
   lxc config show container
   ```

4. **Performance issues**
   ```bash
   # Check mapping count
   cat /proc/net/slick_nat_mappings | wc -l
   
   # Monitor radix tree efficiency
   dmesg | grep -i "radix\|slick"
   ```

5. **NDP not responding**
   ```bash
   # Check if addresses are properly configured
   ip -6 addr show
   
   # Verify no conflicting NDP responders
   cat /proc/net/ipv6_route
   ```

### Debug Mode

```bash
# Enable kernel debug messages
echo 8 > /proc/sys/kernel/printk

# Watch NAT activity
dmesg -w | grep "Slick NAT"

# Performance debugging
echo 'function_graph' > /sys/kernel/debug/tracing/current_tracer
echo 'nat_hook_func' > /sys/kernel/debug/tracing/set_ftrace_filter
```

## Distribution and Packaging

### Debian Package
```bash
# Build debian package
cd pkg/deb
./build-deb.sh

# Generated package
ls build/slick-nat-dkms-0.0.3_amd64.deb
```

### DKMS Package
```bash
# Install DKMS version
cd dkms
sudo ./install.sh

# Check DKMS status
sudo dkms status slick-nat/0.0.3
```

### Source Distribution
```bash
# Create source tarball
tar -czf slick-nat-0.0.3.tar.gz \
    --exclude='.git*' \
    --exclude='*.ko' \
    --exclude='*.o' \
    --exclude='build/' \
    --transform 's,^,slick-nat-0.0.3/,' \
    src/ dkms/ pkg/ containers/ Readme.md Makefile
```

## Limitations

- Maximum 10,000 concurrent mappings per namespace (configurable)
- Supports only TCP, UDP, and ICMPv6 protocols
- No IPv4 support (IPv6 only)
- Link-local addresses are not translated
- Container mappings are per-namespace (isolated between containers)

## Security Notes

- Root privileges required for configuration
- Mappings are stored in kernel memory (not persistent)
- No authentication mechanism for proc interface
- Consider iptables rules for access control
- Container access requires proper LXD/Docker configuration

## Version History

- **0.0.3**: Semver versioning, improved package structure, batch processing support
- **0.0.2**: Radix tree optimization, performance improvements
- **0.0.1**: Initial release with basic NAT functionality

## License

GPL v2 - See source files for full license text.

## Support

For bugs and feature requests:
- Check the source code comments and maintainer documentation in `src/Maintain.md`
- Review DKMS documentation in `dkms/README-DKMS.md`
- Container support guide in `containers/Container-Support.md`
- Package building instructions in `pkg/deb/README.md`

## Contributing

1. Follow kernel coding standards
2. Test with multiple kernel versions
3. Update documentation
4. Performance test with high mapping counts
5. Verify container compatibility
