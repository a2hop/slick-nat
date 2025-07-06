#!/bin/bash

set -e

# Package configuration
PACKAGE_NAME="slick-nat-dkms"
PACKAGE_VERSION="0.0.3"
ARCHITECTURE="$(dpkg --print-architecture)"
MAINTAINER="Slick NAT Project <lucas@xtec.one>"
DESCRIPTION="Slick NAT - Bidirectional IPv6 NAT kernel module with DKMS support"

# Build configuration
BUILD_DIR="$(pwd)/build"
PACKAGE_DIR="${BUILD_DIR}/${PACKAGE_NAME}_${PACKAGE_VERSION}_${ARCHITECTURE}"
SOURCE_DIR="$(realpath ../../)"

echo "Building ${PACKAGE_NAME} version ${PACKAGE_VERSION} for ${ARCHITECTURE}"
echo "Source directory: ${SOURCE_DIR}"
echo "Build directory: ${BUILD_DIR}"

# Clean previous builds
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

# Create package directory structure
mkdir -p "${PACKAGE_DIR}/DEBIAN"
mkdir -p "${PACKAGE_DIR}/usr/src/slick-nat-${PACKAGE_VERSION}"
mkdir -p "${PACKAGE_DIR}/usr/local/bin"
mkdir -p "${PACKAGE_DIR}/usr/share/doc/${PACKAGE_NAME}"
mkdir -p "${PACKAGE_DIR}/etc/modules-load.d"
mkdir -p "${PACKAGE_DIR}/etc/slnat"

# Copy source files
echo "Copying source files..."
cp -r "${SOURCE_DIR}/src/"* "${PACKAGE_DIR}/usr/src/slick-nat-${PACKAGE_VERSION}/"

# Copy DKMS configuration
cp "${SOURCE_DIR}/dkms/dkms.conf" "${PACKAGE_DIR}/usr/src/slick-nat-${PACKAGE_VERSION}/"
cp "${SOURCE_DIR}/dkms/Makefile" "${PACKAGE_DIR}/usr/src/slick-nat-${PACKAGE_VERSION}/"

# Copy management script
cp "${SOURCE_DIR}/src/slnat" "${PACKAGE_DIR}/usr/local/bin/"
chmod +x "${PACKAGE_DIR}/usr/local/bin/slnat"

# Copy documentation
cp "${SOURCE_DIR}/Readme.md" "${PACKAGE_DIR}/usr/share/doc/${PACKAGE_NAME}/"
cp "${SOURCE_DIR}/src/Maintain.md" "${PACKAGE_DIR}/usr/share/doc/${PACKAGE_NAME}/"
cp "${SOURCE_DIR}/dkms/README-DKMS.md" "${PACKAGE_DIR}/usr/share/doc/${PACKAGE_NAME}/"

# Create modules-load.d configuration
cat > "${PACKAGE_DIR}/etc/modules-load.d/slick-nat.conf" << EOF
# Slick NAT kernel module
# Remove this file or comment out the line below to disable auto-loading
slick_nat
EOF

# Create template configuration file
cat > "${PACKAGE_DIR}/etc/slnat/slnat.conf.template" << 'EOF'
# Slick NAT Configuration Template
# Copy this file to slnat.conf and customize for your network

# Example configuration:
# Format: INTERFACE INTERNAL_PREFIX EXTERNAL_PREFIX
# 
# eth0 2001:db8:internal::/64 2001:db8:external::/64
# wlan0 fd00:1234:5678::/64 2001:db8:wifi::/64

# Uncomment and modify the following lines for your network:
# eth0 fd00::/64 2001:db8:1::/64
# docker0 172.17.0.0/16 192.168.100.0/24

# Multiple interfaces can be configured:
# eth0 fd00:internal::/64 2001:db8:external::/64
# eth1 fd00:dmz::/64 2001:db8:dmz::/64

# Notes:
# - Lines starting with # are comments
# - Empty lines are ignored
# - Each line should contain: interface internal_prefix external_prefix
# - Prefixes should include the subnet mask (e.g., /64)
# - IPv4 and IPv6 prefixes are supported
EOF

# Create example startup script
cat > "${PACKAGE_DIR}/etc/slnat/load-routes.sh" << 'EOF'
#!/bin/bash
# Slick NAT Route Loading Script
# This script loads NAT mappings from /etc/slnat/slnat.conf using slnat add-batch

CONFIG_FILE="/etc/slnat/slnat.conf"
SLNAT_CMD="/usr/local/bin/slnat"

# Function to log messages
log_message() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

# Check if running from systemd (non-interactive mode)
NON_INTERACTIVE=false
if [ -n "$SYSTEMD_EXEC_PID" ] || [ "$1" = "--non-interactive" ]; then
    NON_INTERACTIVE=true
fi

if [ ! -f "$CONFIG_FILE" ]; then
    log_message "Configuration file $CONFIG_FILE not found"
    log_message "Copy /etc/slnat/slnat.conf.template to $CONFIG_FILE and customize it"
    exit 1
fi

if [ ! -x "$SLNAT_CMD" ]; then
    log_message "slnat command not found at $SLNAT_CMD"
    exit 1
fi

# Check if module is loaded
if ! lsmod | grep -q slick_nat; then
    log_message "slick_nat module not loaded, attempting to load..."
    if ! modprobe slick_nat; then
        log_message "Failed to load slick_nat module"
        exit 1
    fi
    log_message "slick_nat module loaded successfully"
fi

if [ "$NON_INTERACTIVE" = "true" ]; then
    log_message "Loading NAT mappings from $CONFIG_FILE (non-interactive mode)"
else
    log_message "Loading NAT mappings from $CONFIG_FILE using add-batch command"
fi

# Clear existing mappings first (in case of restart)
log_message "Clearing existing NAT mappings..."
$SLNAT_CMD clear-all || true

# Use slnat add-batch to process the entire configuration file
if $SLNAT_CMD add-batch "$CONFIG_FILE"; then
    log_message "NAT mappings loaded successfully"
    # Show current status only in interactive mode
    if [ "$NON_INTERACTIVE" != "true" ]; then
        $SLNAT_CMD status
    fi
else
    log_message "Error: Failed to load NAT mappings from $CONFIG_FILE"
    log_message "Check the configuration file format and try again"
    exit 1
fi
EOF

chmod +x "${PACKAGE_DIR}/etc/slnat/load-routes.sh"

# Create systemd service file for automatic loading
mkdir -p "${PACKAGE_DIR}/lib/systemd/system"
cat > "${PACKAGE_DIR}/lib/systemd/system/slnat.service" << 'EOF'
[Unit]
Description=Slick NAT Route Loading Service
After=network.target
Wants=network.target

[Service]
Type=oneshot
ExecStart=/etc/slnat/load-routes.sh
ExecStop=/usr/local/bin/slnat clear-all
ExecReload=/bin/bash -c '/usr/local/bin/slnat clear-all && /etc/slnat/load-routes.sh'
RemainAfterExit=yes
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

# Create copyright file
cat > "${PACKAGE_DIR}/usr/share/doc/${PACKAGE_NAME}/copyright" << EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: slick-nat
Source: https://github.com/a2hop/slick-nat

Files: *
Copyright: $(date +%Y) Slick NAT Project
License: GPL-2+
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 .
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 .
 You should have received a copy of the GNU General Public License along
 with this program; if not, write to the Free Software Foundation, Inc.,
 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 .
 On Debian systems, the complete text of the GNU General
 Public License version 2 can be found in "/usr/share/common-licenses/GPL-2".
EOF

# Copy debian control files
echo "Creating debian control files..."
cp control "${PACKAGE_DIR}/DEBIAN/"
cp postinst "${PACKAGE_DIR}/DEBIAN/"
cp prerm "${PACKAGE_DIR}/DEBIAN/"
cp postrm "${PACKAGE_DIR}/DEBIAN/"

# Update control file with architecture
sed -i "s/^Architecture:.*/Architecture: ${ARCHITECTURE}/" "${PACKAGE_DIR}/DEBIAN/control"

# Make scripts executable
chmod +x "${PACKAGE_DIR}/DEBIAN/postinst"
chmod +x "${PACKAGE_DIR}/DEBIAN/prerm"
chmod +x "${PACKAGE_DIR}/DEBIAN/postrm"

# Generate control file with calculated size
INSTALLED_SIZE=$(du -sk "${PACKAGE_DIR}" | cut -f1)
sed -i "s/^Installed-Size:.*/Installed-Size: ${INSTALLED_SIZE}/" "${PACKAGE_DIR}/DEBIAN/control"

# Generate md5sums
echo "Generating md5sums..."
find "${PACKAGE_DIR}" -type f ! -path "${PACKAGE_DIR}/DEBIAN/*" -exec md5sum {} + | \
    sed "s|${PACKAGE_DIR}/||" > "${PACKAGE_DIR}/DEBIAN/md5sums"

# Build the package
echo "Building debian package..."
DEB_FILENAME="slick-nat-dkms_${PACKAGE_VERSION}_${ARCHITECTURE}.deb"
fakeroot dpkg-deb --build "${PACKAGE_DIR}" "${BUILD_DIR}/${DEB_FILENAME}"

echo "Package built successfully: ${BUILD_DIR}/${DEB_FILENAME}"
echo ""
echo "To install:"
echo "  sudo dpkg -i ${BUILD_DIR}/${DEB_FILENAME}"
echo "  sudo apt-get install -f  # if dependencies are missing"
echo ""
echo "To test installation:"
echo "  lsmod | grep slick_nat"
echo "  slnat status"
