# Container Support for Slick NAT

This document explains how to use Slick NAT with containerized environments, particularly LXD containers.

## Overview

Slick NAT is a kernel module that operates at the host level. Containers can access the module's functionality through the proc filesystem interface, but proper configuration is required.

## Architecture

```
Host System (Kernel Module)
├── slick_nat.ko (loaded)
├── /proc/net/slick_nat_mappings (proc interface)
└── Network Namespaces
    ├── Host namespace
    └── Container namespaces
        ├── Container 1 (may have access)
        └── Container 2 (may have access)
```

## Common Issues and Solutions

### Symbolic Link Mount Errors

The most common issue is mounting `/proc/net/slick_nat_mappings` when it's a symbolic link:

```
ERROR: Too many levels of symbolic links - Failed to mount
```

**Solution**: Use bind mounts through `raw.lxc` configuration instead of device mounts.

## LXD Configuration

### Prerequisites

1. Load the module on the host:
```bash
sudo modprobe slick_nat
```

2. Verify module is loaded:
```bash
lsmod | grep slick_nat
cat /proc/net/slick_nat_mappings
```

### Method 1: Privileged Container

For privileged containers (less secure but simpler):

```bash
# Create privileged container
lxc launch ubuntu:20.04 nat-container
lxc config set nat-container security.privileged true

# Add proc device access
lxc config device add nat-container slick-nat-proc disk \
    source="/proc/net/slick_nat_mappings" \
    path="/proc/net/slick_nat_mappings" \
    readonly=false

# Install management script
lxc file push /usr/local/bin/slnat nat-container/usr/local/bin/slnat
lxc exec nat-container -- chmod +x /usr/local/bin/slnat

# Restart container
lxc restart nat-container
```

### Method 2: Unprivileged Container (Recommended)

For unprivileged containers (more secure):

```bash
# Create unprivileged container
lxc launch ubuntu:20.04 nat-container

# Use raw.lxc configuration for reliable proc access
lxc config set nat-container raw.lxc "
lxc.mount.entry = /proc/net/slick_nat_mappings proc/net/slick_nat_mappings none bind,create=file 0 0
lxc.cap.keep = net_admin net_raw
lxc.aa_profile = unconfined
"

# Enable nesting for network operations
lxc config set nat-container security.nesting true
lxc config set nat-container security.syscalls.intercept.mount true
lxc config set nat-container security.syscalls.intercept.mount.allowed "proc"

# Start container
lxc start nat-container

# Install management script
lxc file push /usr/local/bin/slnat nat-container/usr/local/bin/slnat
lxc exec nat-container -- chmod +x /usr/local/bin/slnat
```

### Method 3: Automated Configuration

Use the provided configuration script:

```bash
# Configure container automatically
sudo ./lxd-config.sh nat-container

# Start container
lxc start nat-container

# Test the configuration
lxc exec nat-container -- slnat status
```

## Network Namespace Considerations

### Understanding Network Namespaces

Each container runs in its own network namespace, and the NAT mappings are also per-namespace. This means:

1. **Mappings are isolated**: Each container has its own separate NAT mapping table
2. **Interface names may differ**: Container interfaces may have different names
3. **Network isolation**: Containers can only see their own network interfaces and mappings

### Container Network Setup

```bash
# Inside container - check available interfaces
ip link show

# Add mapping using container's interface name
slnat eth0 add 2001:db8:container::/64 2001:db8:external::/64

# The mapping only affects this container's network namespace
```

## Security Considerations

### Privileged vs Unprivileged

**Privileged Containers:**
- Full access to host kernel interfaces
- Can modify NAT mappings freely
- Less secure - container escape possible
- Simpler configuration

**Unprivileged Containers:**
- Limited access to kernel interfaces
- Require explicit configuration
- More secure - isolated from host
- Complex setup but recommended

### Access Control

Since NAT mappings are per-namespace, consider:

1. **Dedicated NAT containers**: Use specialized containers for NAT management
2. **Read-only access**: Mount proc interface read-only where possible
3. **Network policies**: Use LXD network policies to limit container access
4. **Namespace isolation**: Each container maintains its own mapping table

## Troubleshooting

### Container Cannot Start (Symbolic Link Error)

```bash
# Check if the error is related to symbolic links
incus info --show-log container-name

# If you see "Too many levels of symbolic links", remove device mount:
lxc config device remove container-name slick-nat-proc

# Use raw.lxc configuration instead:
lxc config set container-name raw.lxc "
lxc.mount.entry = /proc/net/slick_nat_mappings proc/net/slick_nat_mappings none bind,create=file 0 0
"

# Restart container
lxc start container-name
```

### Alternative: Privileged Container Workaround

If unprivileged containers continue to have issues:

```bash
# Make container privileged (less secure but more compatible)
lxc config set container-name security.privileged true
lxc restart container-name

# Test access
lxc exec container-name -- ls -la /proc/net/slick_nat_mappings
```

### Container Cannot Access Module

```bash
# Check if proc file exists in container
lxc exec container -- ls -la /proc/net/slick_nat_mappings

# Check if it's properly mounted
lxc exec container -- mount | grep slick_nat

# Check container configuration
lxc config show container

# Check module on host
lsmod | grep slick_nat

# Verify host proc file exists
ls -la /proc/net/slick_nat_mappings
```

### Permission Denied Errors

```bash
# Check file permissions in container
lxc exec container -- ls -la /proc/net/slick_nat_mappings

# Check container capabilities
lxc exec container -- capsh --print

# Check AppArmor profile
lxc exec container -- cat /proc/self/attr/current

# Try disabling AppArmor for testing
lxc config set container raw.lxc "lxc.aa_profile = unconfined"
lxc restart container
```

### Network Interface Issues

```bash
# Check interfaces in container
lxc exec container -- ip link show

# Check if interface exists in container namespace
lxc exec container -- ls /sys/class/net/

# Map host interface to container interface
# Host: eth0 -> Container: eth0 (usually same)
```

## Docker Support

For Docker containers, the approach is similar but uses different commands:

```bash
# Run privileged container with proc access
docker run -it --privileged \
  -v /proc/net/slick_nat_mappings:/proc/net/slick_nat_mappings \
  ubuntu:20.04 bash

# Inside container
./slnat status
```

## Best Practices

### 1. Always Use Bind Mounts

Avoid device mounts for proc files, use raw.lxc bind mounts:

```bash
# Good: Use bind mount
lxc config set container raw.lxc "
lxc.mount.entry = /proc/net/slick_nat_mappings proc/net/slick_nat_mappings none bind,create=file 0 0
"

# Avoid: Device mount (may cause symbolic link issues)
# lxc config device add container slick-nat-proc disk ...
```

### 2. Test Configuration Step by Step

```bash
# 1. Start with basic container
lxc launch ubuntu:20.04 test-container

# 2. Configure access
./lxd-config.sh test-container

# 3. Start container
lxc start test-container

# 4. Test proc access
lxc exec test-container -- ls -la /proc/net/slick_nat_mappings

# 5. Install and test slnat
lxc file push /usr/local/bin/slnat test-container/usr/local/bin/slnat
lxc exec test-container -- chmod +x /usr/local/bin/slnat
lxc exec test-container -- slnat status
```

### 3. Dedicated NAT Management

Create a dedicated container for NAT management:

```bash
# Create NAT management container
lxc launch ubuntu:20.04 nat-manager
./lxd-config.sh nat-manager

# Use this container for all NAT operations
lxc exec nat-manager -- slnat eth0 add 2001:db8:internal::/64 2001:db8:external::/64
```

### 4. Automation Scripts

Create scripts to manage container NAT configurations:

```bash
#!/bin/bash
# Container NAT setup script
CONTAINER="$1"
INTERNAL="$2"
EXTERNAL="$3"

lxc exec "$CONTAINER" -- slnat eth0 add "$INTERNAL" "$EXTERNAL"
```

### 5. Monitoring and Logging

Monitor NAT operations from containers:

```bash
# In container
slnat status
journalctl -f  # Check for errors

# On host
dmesg | grep -i slick
```

## Limitations

1. **Symbolic link issues**: `/proc/net/slick_nat_mappings` may be a symbolic link requiring bind mounts
2. **Per-namespace mappings**: Each container has its own isolated NAT mapping table
3. **Host interfaces**: Mappings reference host network interfaces
4. **Kernel dependency**: Module must be loaded on host
5. **Privilege requirements**: Some operations require elevated privileges or specific capabilities

## Examples

### Basic Container NAT Setup

```bash
# 1. Load module on host
sudo modprobe slick_nat

# 2. Create and configure container
lxc launch ubuntu:20.04 app-container
./lxd-config.sh app-container

# 3. Configure NAT from container
lxc exec app-container -- slnat eth0 add 2001:db8:app::/64 2001:db8:public::/64

# 4. Verify configuration
lxc exec app-container -- slnat status
```

### Multi-Container Setup

```bash
# Configure multiple containers with different prefixes
# Each container has its own isolated mapping table
lxc exec container1 -- slnat eth0 add 2001:db8:app1::/64 2001:db8:public1::/64
lxc exec container2 -- slnat eth0 add 2001:db8:app2::/64 2001:db8:public2::/64

# Each container only sees its own mappings
lxc exec container1 -- slnat status  # Shows only container1's mappings
lxc exec container2 -- slnat status  # Shows only container2's mappings
```

This approach allows containers to manage NAT configurations while maintaining security and proper isolation.
