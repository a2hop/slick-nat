#!/bin/bash

set -e

PACKAGE_NAME="slick-nat"
PACKAGE_VERSION="0.0.3"
MODULE_NAME="slick_nat"

# Check if running as root
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root (use sudo)" 
   exit 1
fi

echo "Uninstalling Slick NAT kernel module..."

# Unload the module if it's loaded
if lsmod | grep -q "${MODULE_NAME}"; then
    echo "Unloading module..."
    modprobe -r "${MODULE_NAME}" || true
fi

# Remove from DKMS
if dkms status | grep -q "${PACKAGE_NAME}"; then
    echo "Removing from DKMS..."
    dkms remove -m "${PACKAGE_NAME}" -v "${PACKAGE_VERSION}" --all || true
fi

# Remove source directory
SRCDIR="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"
if [[ -d "${SRCDIR}" ]]; then
    echo "Removing source directory..."
    rm -rf "${SRCDIR}"
fi

# Remove management script
if [[ -f "/usr/local/bin/slnat" ]]; then
    echo "Removing management script..."
    rm -f /usr/local/bin/slnat
fi

# Remove from /etc/modules if present
if grep -q "^${MODULE_NAME}$" /etc/modules 2>/dev/null; then
    echo "Removing from /etc/modules..."
    sed -i "/^${MODULE_NAME}$/d" /etc/modules
fi

# Remove from /etc/modules-load.d/ if present
if [[ -f "/etc/modules-load.d/slick-nat.conf" ]]; then
    echo "Removing from modules-load.d..."
    rm -f /etc/modules-load.d/slick-nat.conf
fi

echo "Uninstallation complete!"
