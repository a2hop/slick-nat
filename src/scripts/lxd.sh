#!/bin/bash

# Check if module is loaded (works differently in containers)
check_module() {
    if [ ! -f "$PROC_FILE" ]; then
        if is_container; then
            echo "Error: Slick NAT module proc interface not available in container"
            echo "This may be because:"
            echo "1. Module is not loaded on the host"
            echo "2. Container doesn't have access to the proc interface"
            echo "3. Container is not in the correct network namespace"
            echo ""
            echo "On the host, run: sudo modprobe slick_nat"
            echo "And configure LXD to expose the proc interface (see documentation)"
        else
            echo "Error: Slick NAT module not loaded"
            echo "Load it with: sudo modprobe slick_nat"
        fi
        exit 1
    fi
}

# Check container permissions
check_container_permissions() {
    if is_container; then
        # Check if we can write to the proc file
        if [ ! -w "$PROC_FILE" ]; then
            echo "Warning: No write access to $PROC_FILE"
            echo "Container may need additional privileges or device access"
            echo "See documentation for LXD configuration"
        fi
        
        # Check if we're in the right network namespace
        if [ ! -d "/sys/class/net" ]; then
            echo "Warning: Limited network namespace access"
            echo "Some network operations may not work properly"
        fi
    fi
}

lxd_config() {
    local container_name="$1"
    
    if [ -z "$container_name" ]; then
        echo "Usage: $0 lxd-config <container_name>"
        echo ""
        echo "Configure an LXD container to access Slick NAT"
        echo "This command must be run on the LXD host"
        echo ""
        echo "Example:"
        echo "  $0 lxd-config mycontainer"
        return 1
    fi
    
    # Check if we have the LXD config library
    if [ ! -f "$LXD_CONFIG_LIB" ]; then
        echo "Error: LXD configuration library not found at $LXD_CONFIG_LIB"
        echo "This may indicate an incomplete installation"
        return 1
    fi
    
    # Check if we're in a container
    if is_container; then
        echo "Error: LXD configuration must be run on the host system"
        echo "This command configures containers from the host"
        return 1
    fi
    
    # Check if lxc command exists
    if ! command -v lxc >/dev/null 2>&1; then
        echo "Error: lxc command not found"
        echo "This command requires LXD to be installed"
        return 1
    fi
    
    # Check if running as root or with proper permissions
    if [ "$EUID" -ne 0 ] && ! groups | grep -q lxd; then
        echo "Error: This command requires root privileges or lxd group membership"
        echo "Run with: sudo $0 lxd-config $container_name"
        echo "Or add your user to the lxd group: sudo usermod -a -G lxd $USER"
        return 1
    fi
    
    echo "Configuring LXD container '$container_name' for Slick NAT access..."
    
    # Source and execute the LXD configuration library
    # Pass the container name as argument
    (
        source "$LXD_CONFIG_LIB"
        # The sourced script expects CONTAINER_NAME as $1
        set -- "$container_name"
        # Execute the main configuration logic
        main "$@"
    )
}