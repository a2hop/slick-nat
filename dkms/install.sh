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

# Check if DKMS is installed
if ! command -v dkms &> /dev/null; then
    echo "DKMS is not installed. Please install it first:"
    echo "  Ubuntu/Debian: apt install dkms"
    echo "  RHEL/CentOS: yum install dkms"
    echo "  Fedora: dnf install dkms"
    exit 1
fi

# Check if kernel headers are installed
if [[ ! -d "/lib/modules/$(uname -r)/build" ]]; then
    echo "Kernel headers are not installed. Please install them first:"
    echo "  Ubuntu/Debian: apt install linux-headers-$(uname -r)"
    echo "  RHEL/CentOS: yum install kernel-devel"
    echo "  Fedora: dnf install kernel-devel"
    exit 1
fi

echo "Installing Slick NAT kernel module..."

# Create source directory
SRCDIR="/usr/src/${PACKAGE_NAME}-${PACKAGE_VERSION}"
mkdir -p "${SRCDIR}"

# Copy source files from the src directory
cp -r ../src/* "${SRCDIR}/"

# Copy DKMS configuration files
cp dkms.conf "${SRCDIR}/"
cp Makefile "${SRCDIR}/"

# Remove any existing DKMS installation
if dkms status | grep -q "${PACKAGE_NAME}"; then
    echo "Removing existing DKMS installation..."
    dkms remove -m "${PACKAGE_NAME}" -v "${PACKAGE_VERSION}" --all || true
fi

# Add to DKMS
echo "Adding module to DKMS..."
dkms add -m "${PACKAGE_NAME}" -v "${PACKAGE_VERSION}"

# Build the module
echo "Building module..."
dkms build -m "${PACKAGE_NAME}" -v "${PACKAGE_VERSION}"

# Install the module
echo "Installing module..."
dkms install -m "${PACKAGE_NAME}" -v "${PACKAGE_VERSION}"

# Install management script
echo "Installing management script..."
cp "${SRCDIR}/slnat" /usr/local/bin/slnat
chmod +x /usr/local/bin/slnat

# Load the module
echo "Loading module..."
modprobe "${MODULE_NAME}" || true

echo "Installation complete!"
echo ""
echo "Usage:"
echo "  Load module: modprobe ${MODULE_NAME}"
echo "  Unload module: modprobe -r ${MODULE_NAME}"
echo "  Configure: slnat <interface> add <internal_prefix> <external_prefix>"
echo "  List mappings: slnat <interface> list"
echo ""
echo "For automatic loading on boot, add '${MODULE_NAME}' to /etc/modules"
