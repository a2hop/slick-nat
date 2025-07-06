#!/bin/bash

MODULE_NAME="slick_nat"
MODULE_FILE="src/${MODULE_NAME}.ko"

load() {
    if lsmod | grep -q "^${MODULE_NAME}\s"; then
        echo "Module ${MODULE_NAME} is already loaded."
        return
    fi

    if [ ! -f "${MODULE_FILE}" ]; then
        echo "Module file ${MODULE_FILE} not found. Please run 'make' first."
        exit 1
    fi

    echo "Loading ${MODULE_NAME} module..."
    sudo insmod ./${MODULE_FILE}
    if [ $? -ne 0 ]; then
        echo "Failed to load module."
        exit 1
    fi

    echo "Enabling IPv6 forwarding..."
    sudo sysctl -w net.ipv6.conf.all.forwarding=1
    
    echo "Module loaded successfully."
    dmesg | tail
}

unload() {
    if ! lsmod | grep -q "^${MODULE_NAME}\s"; then
        echo "Module ${MODULE_NAME} is not loaded."
        return
    fi

    echo "Unloading ${MODULE_NAME} module..."
    sudo rmmod ${MODULE_NAME}
    if [ $? -ne 0 ]; then
        echo "Failed to unload module."
        exit 1
    fi
    
    echo "Module unloaded successfully."
    dmesg | tail
}

case "$1" in
    load)
        load
        ;;
    unload)
        unload
        ;;
    *)
        echo "Usage: $0 {load|unload}"
        exit 1
esac

exit 0
