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

# Remove any existing problematic device configurations
echo "Cleaning up existing configurations..."
lxc config device remove "$CONTAINER_NAME" slick-nat-proc 2>/dev/null || true

# Method 1: Use raw.lxc bind mount (more reliable)
echo "Adding bind mount configuration..."
lxc config set "$CONTAINER_NAME" raw.lxc "
lxc.mount.entry = /proc/net/slick_nat_mappings proc/net/slick_nat_mappings none bind,create=file 0 0
lxc.cap.keep = net_admin net_raw
lxc.aa_profile = unconfined
"

# Method 2: Alternative - shared mount namespace approach
echo "Configuring security settings..."
lxc config set "$CONTAINER_NAME" security.privileged false
lxc config set "$CONTAINER_NAME" security.nesting true
lxc config set "$CONTAINER_NAME" security.syscalls.intercept.mount true
lxc config set "$CONTAINER_NAME" security.syscalls.intercept.mount.allowed "proc"

# Method 3: Proxy device as fallback
echo "Adding proxy device for proc access..."
lxc config device add "$CONTAINER_NAME" slick-nat-proxy proxy \
    listen="unix:/tmp/slick_nat_sock" \
    connect="unix:/tmp/slick_nat_sock" \
    bind=container || echo "Warning: Proxy device configuration failed"

# Copy management script to container
echo "Installing management script in container..."
if lxc info "$CONTAINER_NAME" | grep -q "Status: RUNNING"; then
    lxc file push /usr/local/bin/slnat "$CONTAINER_NAME/usr/local/bin/slnat" 2>/dev/null || echo "Warning: Could not copy slnat script"
    lxc exec "$CONTAINER_NAME" -- chmod +x /usr/local/bin/slnat 2>/dev/null || echo "Warning: Could not set permissions"
else
    echo "Container not running - will copy script after start"
fi

echo "Configuration complete!"
echo ""
echo "To start the container:"
echo "  lxc start $CONTAINER_NAME"
echo ""
echo "To test the configuration:"
echo "  lxc exec $CONTAINER_NAME -- slnat status"
echo ""
echo "If you encounter issues, try the alternative privileged method:"
echo "  lxc config set $CONTAINER_NAME security.privileged true"
echo "  lxc restart $CONTAINER_NAME"
