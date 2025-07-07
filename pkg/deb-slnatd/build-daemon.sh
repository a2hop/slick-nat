#!/bin/bash

set -e

# Variables
DAEMON_PKG_DIR="pkg/build/slick-nat-daemon"

# Clean previous build
rm -rf "$DAEMON_PKG_DIR"
mkdir -p "$DAEMON_PKG_DIR/DEBIAN"
mkdir -p "$DAEMON_PKG_DIR/usr/sbin"
mkdir -p "$DAEMON_PKG_DIR/lib/systemd/system"
mkdir -p "$DAEMON_PKG_DIR/usr/share/man/man8"

# Copy daemon files
cp slick-nat-daemon "$DAEMON_PKG_DIR/usr/sbin/"
cp pkg/deb-slnatd/control "$DAEMON_PKG_DIR/DEBIAN/"

# Create systemd service file
cat > "$DAEMON_PKG_DIR/lib/systemd/system/slnatd.service" << 'EOF'
[Unit]
Description=SlickNat IPv6 NAT Daemon
After=network.target
Wants=network.target

[Service]
Type=simple
ExecStart=/usr/sbin/slick-nat-daemon --config /etc/slnatd/config
Restart=always
RestartSec=5
User=root
Group=root

[Install]
WantedBy=multi-user.target
EOF

# Create config directory structure
mkdir -p "$DAEMON_PKG_DIR/etc/slnatd"

# Create default config file
cat > "$DAEMON_PKG_DIR/etc/slnatd/config" << 'EOF'
# SlickNat Daemon Configuration
# Listen on multiple IPv6 addresses

# Listen on localhost
listen ::1 7001

# Listen on all IPv6 addresses (uncomment if needed)
# listen :: 7001

# Specify additional addresses to listen on
# listen 2001:db8::1 7001
# listen fd00::1 7001

# Kernel proc file path
proc_path /proc/net/slick_nat_mappings
EOF


# Create man page for daemon
cat > "$DAEMON_PKG_DIR/usr/share/man/man8/slick-nat-daemon.8" << 'EOF'
.TH SLICK-NAT-DAEMON 8 "2024" "SlickNat" "System Administration"
.SH NAME
slick-nat-daemon \- SlickNat IPv6 NAT daemon
.SH SYNOPSIS
.B slick-nat-daemon
.RI [ options ]
.SH DESCRIPTION
Daemon that provides IPv6 NAT mapping information via JSON API.
Can listen on multiple IPv6 addresses simultaneously.
.SH OPTIONS
.TP
.B --config PATH
Configuration file path (default: /etc/slnatd/config)
.TP
.B --proc PATH
Kernel proc file path (default: /proc/net/slick_nat_mappings)
.SH CONFIGURATION
The configuration file supports the following directives:
.TP
.B listen ADDRESS PORT
Listen on the specified IPv6 address and port
.TP
.B proc_path PATH
Path to the kernel proc file
.SH FILES
.TP
.I /etc/slnatd/config
Main configuration file
.TP
.I /etc/slick-nat/daemon.conf
Legacy configuration file (deprecated)
.TP
.I /proc/net/slick_nat_mappings
Kernel NAT mappings
.SH EXAMPLES
Configuration file example:
.nf
# Listen on localhost
listen ::1 7001
# Listen on specific address
listen 2001:db8::1 7001
# Set proc path
proc_path /proc/net/slick_nat_mappings
.fi
.SH SEE ALSO
.BR slnatc (1)
EOF

# Set permissions
chmod 755 "$DAEMON_PKG_DIR/usr/sbin/slick-nat-daemon"
chmod 644 "$DAEMON_PKG_DIR/lib/systemd/system/slnatd.service"
chmod 644 "$DAEMON_PKG_DIR/etc/slnatd/config"
chmod 644 "$DAEMON_PKG_DIR/etc/slick-nat/daemon.conf"

# Build package
dpkg-deb --build "$DAEMON_PKG_DIR"
mv "$DAEMON_PKG_DIR.deb" pkg/output/slick-nat-daemon.deb