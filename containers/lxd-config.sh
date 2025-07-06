#!/bin/bash

# LXD Configuration Script for Slick NAT
# This script can be run standalone or sourced as a library

set -e

# Main configuration function
main() {
    local CONTAINER_NAME="$1"

    if [ -z "$CONTAINER_NAME" ]; then
        echo "Usage: $0 <container_name>"
        echo ""
        echo "This script configures an LXD container to access Slick NAT"
        echo "Run this on the LXD host after loading the slick_nat module"
        return 1
    fi

    # Check if container exists
    if ! lxc info "$CONTAINER_NAME" >/dev/null 2>&1; then
        echo "Error: Container '$CONTAINER_NAME' does not exist"
        return 1
    fi

    # Check if slick_nat module is loaded
    if ! lsmod | grep -q slick_nat; then
        echo "Error: slick_nat module not loaded on host"
        echo "Load it first with: sudo modprobe slick_nat"
        return 1
    fi

    echo "Configuring container '$CONTAINER_NAME' for Slick NAT access..."

    # Stop container if running to avoid config conflicts
    if lxc info "$CONTAINER_NAME" | grep -q "Status: RUNNING"; then
        echo "Stopping container for configuration..."
        lxc stop "$CONTAINER_NAME"
    fi

    # Remove any existing problematic device configurations
    echo "Cleaning up existing configurations..."
    lxc config device remove "$CONTAINER_NAME" slick-nat-proc 2>/dev/null || true
    lxc config device remove "$CONTAINER_NAME" slick-nat-proxy 2>/dev/null || true

    # Clear any existing problematic configurations
    lxc config unset "$CONTAINER_NAME" raw.lxc 2>/dev/null || true
    lxc config unset "$CONTAINER_NAME" raw.apparmor 2>/dev/null || true

    # Method 1: Try simple privileged approach first
    echo "Configuring privileged container with basic settings..."
    lxc config set "$CONTAINER_NAME" security.privileged true
    lxc config set "$CONTAINER_NAME" security.nesting true

    # Try to start with basic privileged config
    echo "Testing basic privileged configuration..."
    if lxc start "$CONTAINER_NAME" 2>/dev/null; then
        echo "✓ Container started successfully with privileged mode"
        
        # Wait for container to be ready
        sleep 2
        
        # Test if proc file is accessible
        if lxc exec "$CONTAINER_NAME" -- ls -la /proc/net/slick_nat_mappings >/dev/null 2>&1; then
            echo "✓ Proc file is accessible - no additional mount needed"
        else
            echo "Adding bind mount for proc file..."
            lxc stop "$CONTAINER_NAME"
            lxc config set "$CONTAINER_NAME" raw.lxc "lxc.mount.entry = /proc/net/slick_nat_mappings proc/net/slick_nat_mappings none bind,create=file 0 0"
            lxc start "$CONTAINER_NAME"
            sleep 2
        fi
    else
        echo "Basic privileged configuration failed, trying advanced settings..."
        
        # Method 2: Add bind mount configuration
        echo "Adding bind mount configuration..."
        lxc config set "$CONTAINER_NAME" raw.lxc "lxc.mount.entry = /proc/net/slick_nat_mappings proc/net/slick_nat_mappings none bind,create=file 0 0"
        
        # Method 3: Disable AppArmor entirely (safer than custom profile)
        echo "Disabling AppArmor for container..."
        lxc config set "$CONTAINER_NAME" security.apparmor false
        
        # Try to start with full configuration
        if lxc start "$CONTAINER_NAME" 2>/dev/null; then
            echo "✓ Container started with advanced configuration"
            sleep 2
        else
            echo "✗ Advanced configuration failed, trying minimal setup..."
            lxc config unset "$CONTAINER_NAME" raw.lxc
            lxc config set "$CONTAINER_NAME" security.apparmor true
            lxc start "$CONTAINER_NAME"
            sleep 2
        fi
    fi

    # Copy management script to container
    echo "Installing management script in container..."
    if lxc info "$CONTAINER_NAME" | grep -q "Status: RUNNING"; then
        # Create directory if it doesn't exist
        lxc exec "$CONTAINER_NAME" -- mkdir -p /usr/local/bin
        
        if [ -f /usr/local/bin/slnat ]; then
            lxc file push /usr/local/bin/slnat "$CONTAINER_NAME/usr/local/bin/slnat" 2>/dev/null || echo "Warning: Could not copy slnat script"
            lxc exec "$CONTAINER_NAME" -- chmod +x /usr/local/bin/slnat 2>/dev/null || echo "Warning: Could not set permissions"
        else
            echo "Warning: /usr/local/bin/slnat not found on host"
        fi
    else
        echo "Container not running - skipping script installation"
    fi

    # Test proc file access
    echo "Testing proc file access..."
    if lxc exec "$CONTAINER_NAME" -- ls -la /proc/net/slick_nat_mappings >/dev/null 2>&1; then
        echo "✓ Proc file is accessible in container"
        
        # Test if slnat command works
        if lxc exec "$CONTAINER_NAME" -- which slnat >/dev/null 2>&1; then
            echo "Testing slnat command..."
            if lxc exec "$CONTAINER_NAME" -- slnat status >/dev/null 2>&1; then
                echo "✓ slnat command works correctly"
            else
                echo "⚠ slnat command found but may need configuration"
            fi
        else
            echo "⚠ slnat command not found in container"
        fi
    else
        echo "✗ Proc file not accessible - trying manual bind mount..."
        
        # Try manual bind mount as fallback
        if lxc exec "$CONTAINER_NAME" -- mount --bind /proc/net/slick_nat_mappings /proc/net/slick_nat_mappings 2>/dev/null; then
            echo "✓ Manual bind mount successful"
        else
            echo "✗ Manual bind mount failed - container may need different configuration"
        fi
    fi

    echo ""
    echo "Configuration complete!"
    echo ""
    echo "Container status:"
    lxc list "$CONTAINER_NAME"
    echo ""
    echo "Current container configuration:"
    lxc config show "$CONTAINER_NAME" | grep -E "(security\.|raw\.)"
    echo ""
    echo "To test the configuration:"
    echo "  lxc exec $CONTAINER_NAME -- ls -la /proc/net/slick_nat_mappings"
    echo "  lxc exec $CONTAINER_NAME -- slnat status"
    echo ""
    echo "If issues persist, try:"
    echo "  lxc exec $CONTAINER_NAME -- cat /proc/net/slick_nat_mappings"
    echo "  lxc exec $CONTAINER_NAME -- mount | grep slick_nat"
}

# If script is run directly (not sourced), execute main function
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    main "$@"
fi
