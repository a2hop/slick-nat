# Slick NAT - Debian Package Builder

This directory contains the build system for creating Debian packages (.deb) for Slick NAT.

## Building the Package

### Prerequisites

```bash
# Install build tools
sudo apt install build-essential devscripts debhelper dpkg-dev

# Install DKMS (for testing)
sudo apt install dkms
```

### Build Process

```bash
# Navigate to the package directory
cd ~/slick-nat/pkg/deb

# Make build script executable
chmod +x build-deb.sh

# Build the package
./build-deb.sh
```

This will create:
- `build/slick-nat-dkms-0.0.3_amd64.deb` - The debian package (architecture-specific)
- `build/slick-nat-dkms-0.0.3_amd64/` - The package contents (for inspection)

## Package Contents

The generated package includes:

### Files Installed
- `/usr/src/slick-nat-0.0.3/` - Source code for DKMS
- `/usr/local/bin/slnat` - Management script
- `/usr/share/doc/slick-nat-dkms/` - Documentation
- `/etc/modules-load.d/slick-nat.conf` - Auto-loading configuration
- `/etc/slnat/slnat.conf.template` - Configuration template
- `/etc/slnat/slnat.conf` - Main configuration file (created from template)
- `/etc/slnat/load-routes.sh` - Route loading script
- `/lib/systemd/system/slnat.service` - Systemd service for automatic route loading

### DKMS Integration
- Automatically builds module for current kernel
- Rebuilds on kernel updates
- Handles module loading/unloading

## Installation

### From Built Package
```bash
# Install the package (architecture-specific filename)
sudo dpkg -i build/slick-nat-dkms_0.0.3_amd64.deb

# Install missing dependencies (if any)
sudo apt-get install -f
```

### Verification
```bash
# Check if module is loaded
lsmod | grep slick_nat

# Test management script
slnat status

# Check DKMS status
sudo dkms status slick-nat/0.0.3

# Check configuration
cat /etc/slnat/slnat.conf

# Load routes from configuration
sudo systemctl start slnat

# Check service status
sudo systemctl status slnat
```

## Distribution

### Creating Release Archive
```bash
# Create distributable tarball
cd build
tar -czf slick-nat-dkms_0.0.3_amd64.tar.gz slick-nat-dkms_0.0.3_amd64.deb

# Or create multiple format archives
gzip -c slick-nat-dkms_0.0.3_amd64.deb > slick-nat-dkms_0.0.3_amd64.deb.gz
```

### Multi-Architecture Support
```bash
# The build script automatically detects architecture
# On different systems, you'll get:
# - slick-nat-dkms_0.0.3_amd64.deb (on x86_64)
# - slick-nat-dkms_0.0.3_arm64.deb (on aarch64)
# - slick-nat-dkms_0.0.3_i386.deb (on i386)
```

### APT Repository Integration
```bash
# For local APT repository with architecture support
mkdir -p /var/www/html/repo/deb
cp build/slick-nat-dkms_0.0.3_*.deb /var/www/html/repo/deb/
cd /var/www/html/repo
dpkg-scanpackages deb /dev/null | gzip -9c > deb/Packages.gz
```

## Package Information

### Version Format
- **Semver Compatible**: 0.0.3 (MAJOR.MINOR.PATCH)
- **Architecture Specific**: Package names include architecture (e.g., `_amd64.deb`)
- **DKMS Version**: Matches source version for consistency

### Dependencies
- `dkms` (>= 2.2) - Dynamic Kernel Module Support
- `linux-headers-generic` - Kernel headers for building
- `build-essential` (recommended) - Build tools

### Supported Systems
- Ubuntu 18.04+ (amd64, arm64, i386)
- Debian 10+ (amd64, arm64, i386)
- Linux kernel 4.14+

## Customization

### Modify Package Details
Edit the variables in `build-deb.sh`:
```bash
PACKAGE_NAME="slick-nat-dkms"
PACKAGE_VERSION="0.0.3"
MAINTAINER="Lukasz Xu-Kafarski <lucas@xtec.one>"
```

### Add Dependencies
Edit `control` file:
```
Depends: dkms (>= 2.2), your-additional-package
```

### Modify Installation Behavior
Edit the maintainer scripts:
- `postinst` - Post-installation actions
- `prerm` - Pre-removal actions
- `postrm` - Post-removal cleanup

## Testing

### Test Installation
```bash
# Install in test environment
sudo dpkg -i build/slick-nat-dkms_0.0.3_amd64.deb

# Test functionality
modprobe slick_nat
slnat status
```

### Test Configuration
```bash
# Install in test environment
sudo dpkg -i build/slick-nat-dkms_0.0.3_amd64.deb

# Test configuration loading
sudo systemctl start slnat
sudo systemctl status slnat

# Test route clearing
sudo systemctl stop slnat
slnat status  # Should show no mappings

# Test restart behavior
sudo systemctl restart slnat
slnat status  # Should show mappings from config file

# Test functionality
modprobe slick_nat
slnat status
```

### Test Removal
```bash
# Remove package
sudo dpkg -r slick-nat-dkms

# Verify cleanup
lsmod | grep slick_nat  # Should be empty
ls /usr/src/slick-nat-*  # Should not exist
```

### Test Upgrade
```bash
# Modify version to 0.0.4 and rebuild
sed -i 's/0.0.3/0.0.4/g' build-deb.sh control postinst prerm postrm
./build-deb.sh

# Install newer version over old one
sudo dpkg -i build/slick-nat-dkms_0.0.4_amd64.deb
```

### Cross-Architecture Testing
```bash
# Test on different architectures
# ARM64 system will generate: slick-nat-dkms_0.0.3_arm64.deb
# i386 system will generate: slick-nat-dkms_0.0.3_i386.deb
```

## Configuration Management

### Configuration Files
The package installs configuration files in `/etc/slnat/`:

- `slnat.conf.template` - Template configuration file
- `slnat.conf` - Main configuration file (created from template during installation)
- `load-routes.sh` - Script to load routes from configuration

### Configuration Format
Edit `/etc/slnat/slnat.conf` to configure NAT mappings:

```bash
# Example configuration
eth0 fd00:internal::/64 2001:db8:external::/64
wlan0 192.168.1.0/24 10.0.0.0/24
docker0 172.17.0.0/16 192.168.100.0/24
```

### Automatic Route Loading
The package includes a systemd service for automatic route loading:

```bash
# Enable automatic loading on boot
sudo systemctl enable slnat

# Start the service now (loads routes from config)
sudo systemctl start slnat

# Stop the service (clears all routes)
sudo systemctl stop slnat

# Restart the service (clears all routes and reloads from config)
sudo systemctl restart slnat

# Reload the service (clears all routes and reloads from config)
sudo systemctl reload slnat

# Check service status
sudo systemctl status slnat

# View service logs
sudo journalctl -u slnat
```

### Service Behavior
- **Start**: Clears existing routes and loads routes from `/etc/slnat/slnat.conf`
- **Stop**: Clears all NAT mappings
- **Restart**: Clears all mappings and reloads from configuration file
- **Reload**: Same as restart - clears all mappings and reloads from configuration file

### Manual Route Loading
```bash
# Load routes manually
sudo /etc/slnat/load-routes.sh

# Or use the systemd service
sudo systemctl start slnat
```

## Troubleshooting

### Build Failures
```bash
# Check for missing dependencies
sudo apt install build-essential devscripts

# Verify source files exist
ls -la ../../src/
ls -la ../../dkms/

# Check architecture detection
dpkg --print-architecture
```

### Installation Issues
```bash
# Check DKMS logs
sudo dkms status slick-nat/0.0.3
cat /var/lib/dkms/slick-nat/0.0.3/build/make.log

# Check kernel headers
ls /lib/modules/$(uname -r)/build

# Verify package architecture matches system
dpkg --print-architecture
file build/slick-nat-dkms_0.0.3_*.deb
```

### Module Loading Issues
```bash
# Check module info
modinfo slick_nat

# Check dependencies
ldd /lib/modules/$(uname -r)/updates/dkms/slick_nat.ko

# Check kernel logs
dmesg | grep slick
```

### Version Conflicts
```bash
# Check installed version
sudo dkms status slick-nat
dpkg -l | grep slick-nat

# Remove old versions
sudo dkms remove slick-nat/0.0.2 --all
sudo dpkg -r slick-nat-dkms
```

## Automation

### CI/CD Integration
```bash
#!/bin/bash
# Example CI build script for multiple architectures
cd pkg/deb
./build-deb.sh

# Upload architecture-specific packages
ARCH=$(dpkg --print-architecture)
aws s3 cp build/slick-nat-dkms_0.0.3_${ARCH}.deb s3://releases/
```

### Makefile Integration
Add to main Makefile:
```makefile
deb:
	cd pkg/deb && ./build-deb.sh

install-deb: deb
	sudo dpkg -i pkg/deb/build/slick-nat-dkms_0.0.3_$(shell dpkg --print-architecture).deb

clean-deb:
	rm -rf pkg/deb/build/
```

### Batch Building
```bash
# Build for multiple architectures (if cross-compilation is set up)
for arch in amd64 arm64 i386; do
    ARCH=$arch ./build-deb.sh
done
```

## Version Management

### Semver Compliance
- **0.0.3**: Current stable release
- **0.0.4**: Next patch release (bug fixes)
- **0.1.0**: Next minor release (new features)
- **1.0.0**: Next major release (breaking changes)

### Version Bumping
```bash
# Update version in all files
OLD_VERSION="0.0.3"
NEW_VERSION="0.0.4"

sed -i "s/${OLD_VERSION}/${NEW_VERSION}/g" \
    build-deb.sh control postinst prerm postrm \
    ../../dkms/dkms.conf ../../dkms/install.sh ../../dkms/uninstall.sh \
    ../../src/slick-nat.c ../../Readme.md
```

### Release Checklist
- [ ] Update version in all files
- [ ] Test build on target architectures
- [ ] Test installation and removal
- [ ] Test DKMS functionality
- [ ] Update documentation
- [ ] Create release notes

## Architecture Support

### Supported Architectures
- **amd64**: Intel/AMD 64-bit (most common)
- **arm64**: ARM 64-bit (servers, Apple M1, etc.)
- **i386**: Intel 32-bit (legacy systems)

### Package Naming Convention
- Format: `slick-nat-dkms_VERSION_ARCHITECTURE.deb`
- Examples:
  - `slick-nat-dkms_0.0.3_amd64.deb`
  - `slick-nat-dkms_0.0.3_arm64.deb`
  - `slick-nat-dkms_0.0.3_i386.deb`

### Cross-Architecture Notes
- DKMS packages are architecture-independent for source
- Binary modules are built on target architecture
- Package metadata includes architecture for proper dependency resolution

This package builder provides a complete solution for distributing Slick NAT as a professional Debian package with proper semver versioning, architecture support, and DKMS integration.
