#!/bin/bash

# LXD Configuration Script for Slick NAT

set -e

CONTAINER_NAME="$1"

if [ -z "$CONTAINER_NAME" ]; then
    echo "Usage: $0 <container_name>"
    echo ""
    echo "This script configures an LXD container to access Slick NAT"
    echo "Run this on the LXD host after loading the slick_nat module"
    exit 1
fi

# Check if container exists
if ! lxc info "$CONTAINER_NAME" >/dev/null 2>&1; then
    echo "Error: Container '$CONTAINER_NAME' does not exist"
    exit 1
fi

# Check if slick_nat module is loaded
if ! lsmod | grep -q slick_nat; then
    echo "Error: slick_nat module not loaded on host"
    echo "Load it first with: sudo modprobe slick_nat"
    exit 1
fi

echo "Configuring container '$CONTAINER_NAME' for Slick NAT access..."

# Method 1: Add proc file as device (for privileged containers)
echo "Adding proc device access..."
lxc config device add "$CONTAINER_NAME" slick-nat-proc disk \
    source="/proc/net/slick_nat_mappings" \
    path="/proc/net/slick_nat_mappings" \
    readonly=false || echo "Warning: Failed to add proc device"

# Method 2: Add raw.lxc configuration for unprivileged containers
echo "Adding raw LXC configuration..."
lxc config set "$CONTAINER_NAME" raw.lxc "
lxc.mount.entry = /proc/net/slick_nat_mappings proc/net/slick_nat_mappings none bind,create=file 0 0
"

# Method 3: Security configuration for unprivileged containers
echo "Configuring security settings..."
lxc config set "$CONTAINER_NAME" security.privileged false
lxc config set "$CONTAINER_NAME" security.nesting true

# Add network capabilities if needed
echo "Adding network capabilities..."
lxc config set "$CONTAINER_NAME" linux.kernel_modules "slick_nat"
lxc config set "$CONTAINER_NAME" raw.lxc "
lxc.cap.keep = sys_module net_admin net_raw
lxc.aa_profile = unconfined
"

# Copy management script to container
echo "Installing management script in container..."
lxc file push /usr/local/bin/slnat "$CONTAINER_NAME/usr/local/bin/slnat"
lxc exec "$CONTAINER_NAME" -- chmod +x /usr/local/bin/slnat

echo "Configuration complete!"
echo ""
echo "To test the configuration:"
echo "  lxc exec $CONTAINER_NAME -- slnat status"
echo ""
echo "Note: Container may need to be restarted for some changes to take effect"
echo "  lxc restart $CONTAINER_NAME"
