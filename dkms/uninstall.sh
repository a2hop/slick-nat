#!/bin/bash

set -e

PACKAGE_NAME="slick-nat"
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

# Remove every registered version from DKMS.
# This used to target one hardcoded version, so after a version bump the
# uninstaller silently left the older DKMS entry and its source tree behind.
INSTALLED_VERSIONS="$(dkms status -m "${PACKAGE_NAME}" 2>/dev/null \
                      | sed -n "s|^${PACKAGE_NAME}[/,] *\([^,:]*\).*|\1|p" \
                      | sort -u)"

for PACKAGE_VERSION in ${INSTALLED_VERSIONS}; do
    echo "Removing ${PACKAGE_NAME}/${PACKAGE_VERSION} from DKMS..."
    dkms remove -m "${PACKAGE_NAME}" -v "${PACKAGE_VERSION}" --all || true
done

# Remove any leftover source directories
for SRCDIR in /usr/src/"${PACKAGE_NAME}"-*; do
    [[ -d "${SRCDIR}" ]] || continue
    echo "Removing source directory ${SRCDIR}..."
    rm -rf "${SRCDIR}"
done

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
