# Slick NAT - DKMS Distribution

This package provides DKMS (Dynamic Kernel Module Support) for the Slick NAT kernel module, allowing automatic rebuilding and installation across kernel updates.

## Prerequisites

### Required Packages

**Ubuntu/Debian:**
```bash
sudo apt update
sudo apt install dkms linux-headers-$(uname -r) build-essential
```

**RHEL/CentOS:**
```bash
sudo yum install dkms kernel-devel gcc make
```

**Fedora:**
```bash
sudo dnf install dkms kernel-devel gcc make
```

### System Requirements

- Linux kernel 4.14 or later
- DKMS 2.2 or later
- GCC compiler
- Kernel headers for running kernel

## Installation

### Quick Installation

```bash
# Navigate to DKMS directory
cd ~/slick-nat/dkms

# Make scripts executable
chmod +x install.sh uninstall.sh

# Install the module
sudo ./install.sh
```

### Manual Installation

```bash
# Copy source to DKMS directory
sudo mkdir -p /usr/src/slick-nat-0.3
sudo cp -r ../src/* /usr/src/slick-nat-0.3/
sudo cp dkms.conf /usr/src/slick-nat-0.3/
sudo cp Makefile /usr/src/slick-nat-0.3/

# Add to DKMS
sudo dkms add -m slick-nat -v 0.3

# Build the module
sudo dkms build -m slick-nat -v 0.3

# Install the module
sudo dkms install -m slick-nat -v 0.3

# Install management script
sudo cp /usr/src/slick-nat-0.3/slnat /usr/local/bin/slnat
sudo chmod +x /usr/local/bin/slnat
```

## Usage

### Load Module
```bash
sudo modprobe slick_nat
```

### Configure NAT Mappings
```bash
# Add mapping
sudo slnat eth0 add 2001:db8:internal::/64 2001:db8:external::/64

# List mappings
sudo slnat eth0 list

# Check status
slnat status
```

### Automatic Loading on Boot
```bash
# Add to /etc/modules for automatic loading
echo 'slick_nat' | sudo tee -a /etc/modules
```

## Uninstallation

### Quick Uninstall
```bash
cd ~/slick-nat/dkms
sudo ./uninstall.sh
```

### Manual Uninstall
```bash
# Remove from DKMS
sudo dkms remove -m slick-nat -v 0.3 --all

# Remove source directory
sudo rm -rf /usr/src/slick-nat-0.3

# Remove management script
sudo rm -f /usr/local/bin/slnat
```

## DKMS Commands

### Check Status
```bash
sudo dkms status
```

### Rebuild Module
```bash
sudo dkms build -m slick-nat -v 0.3
sudo dkms install -m slick-nat -v 0.3
```

### Remove and Reinstall
```bash
sudo dkms remove -m slick-nat -v 0.3 --all
sudo dkms add -m slick-nat -v 0.3
sudo dkms build -m slick-nat -v 0.3
sudo dkms install -m slick-nat -v 0.3
```

## Distribution

### Creating a Tarball
```bash
# Create distribution tarball from project root
cd ~/slick-nat
tar -czf slick-nat-0.3-dkms.tar.gz \
    --exclude='.git*' \
    --exclude='*.ko' \
    --exclude='*.o' \
    --exclude='.tmp_versions' \
    --exclude='Module.symvers' \
    --exclude='modules.order' \
    --exclude='old/' \
    --transform 's,^,slick-nat-0.3/,' \
    src/ dkms/ Readme.md
```

### Creating a Debian Package
```bash
# Create debian package structure
mkdir -p slick-nat-dkms-0.3/usr/src/slick-nat-0.3
mkdir -p slick-nat-dkms-0.3/usr/local/bin
mkdir -p slick-nat-dkms-0.3/DEBIAN

# Copy files
cp -r ../src/* slick-nat-dkms-0.3/usr/src/slick-nat-0.3/
cp dkms.conf slick-nat-dkms-0.3/usr/src/slick-nat-0.3/
cp Makefile slick-nat-dkms-0.3/usr/src/slick-nat-0.3/
cp ../src/slnat slick-nat-dkms-0.3/usr/local/bin/

# Create control file
cat > slick-nat-dkms-0.3/DEBIAN/control << EOF
Package: slick-nat-dkms
Version: 0.3
Section: kernel
Priority: optional
Architecture: all
Depends: dkms
Maintainer: Your Name <your.email@example.com>
Description: Slick NAT kernel module (DKMS)
 Bidirectional IPv6 NAT kernel module with DKMS support
EOF

# Create postinst script
cat > slick-nat-dkms-0.3/DEBIAN/postinst << 'EOF'
#!/bin/bash
set -e
dkms add -m slick-nat -v 0.3
dkms build -m slick-nat -v 0.3
dkms install -m slick-nat -v 0.3
EOF

# Create prerm script
cat > slick-nat-dkms-0.3/DEBIAN/prerm << 'EOF'
#!/bin/bash
set -e
dkms remove -m slick-nat -v 0.3 --all || true
EOF

# Make scripts executable
chmod +x slick-nat-dkms-0.3/DEBIAN/postinst
chmod +x slick-nat-dkms-0.3/DEBIAN/prerm

# Build package
dpkg-deb --build slick-nat-dkms-0.3
```

## Directory Structure

The project follows this structure:
```
slick-nat/
├── src/                    # Source files
│   ├── slick-nat.c        # Main module
│   ├── ndp.c              # NDP proxy
│   ├── ndp.h              # NDP header
│   ├── slnat              # Management script
│   ├── Makefile           # Source build configuration
│   └── Maintain.md        # Maintenance docs
├── dkms/                   # DKMS configuration
│   ├── dkms.conf          # DKMS config
│   ├── Makefile           # DKMS build configuration
│   ├── install.sh         # Installation script
│   ├── uninstall.sh       # Uninstallation script
│   └── README-DKMS.md     # This file
├── old/                    # Legacy files
└── Readme.md              # Main documentation
```

## Troubleshooting

### Common Issues

1. **Module fails to build**
   ```bash
   # Check DKMS logs
   sudo dkms status
   cat /var/lib/dkms/slick-nat/0.3/build/make.log
   ```

2. **Kernel headers missing**
   ```bash
   # Install headers for current kernel
   sudo apt install linux-headers-$(uname -r)  # Ubuntu/Debian
   sudo yum install kernel-devel  # RHEL/CentOS
   ```

3. **Module not loading**
   ```bash
   # Check kernel logs
   sudo dmesg | grep -i slick
   
   # Check module dependencies
   modinfo slick_nat
   ```

### Debug Information

```bash
# DKMS version
dkms --version

# Kernel version
uname -r

# Available headers
ls /lib/modules/$(uname -r)/

# Module information
modinfo slick_nat
```

## Support

For issues specific to DKMS packaging, check:
- DKMS logs in `/var/lib/dkms/slick-nat/0.3/build/`
- System logs with `journalctl -f`
- Kernel ring buffer with `dmesg`

For module-specific issues, refer to the main README.md and src/Maintain.md files.
