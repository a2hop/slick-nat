#!/bin/bash

set -e

# Build configuration
VERSION="1.0.0"
BUILD_DIR="/tmp/slick-nat-client-build"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "Building SlickNat Client package..."
echo "Project root: $PROJECT_ROOT"
echo "Build directory: $BUILD_DIR"

# Clean and create build directory
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# Build client binary
echo "Building client binary..."
cd "$PROJECT_ROOT"

g++ -std=c++17 -O2 -o slnatc slnat-agent/net-client/slnatc.cpp -lnlohmann_json

# Build client package
echo "Building client package..."
CLIENT_PKG_DIR="$BUILD_DIR/slick-nat-client"
mkdir -p "$CLIENT_PKG_DIR/DEBIAN"
mkdir -p "$CLIENT_PKG_DIR/usr/bin"
mkdir -p "$CLIENT_PKG_DIR/usr/share/man/man1"
mkdir -p "$CLIENT_PKG_DIR/usr/share/doc/slick-nat-client"

# Copy client files
cp slnatc "$CLIENT_PKG_DIR/usr/bin/"
cp pkg/deb-slnatc/control "$CLIENT_PKG_DIR/DEBIAN/"

# Create man page for client
cat > "$CLIENT_PKG_DIR/usr/share/man/man1/slnatc.1" << 'EOF'
.TH SLNATC 1 "2024" "SlickNat" "User Commands"
.SH NAME
slnatc \- SlickNat client utility
.SH SYNOPSIS
.B slnatc
.I daemon_address command
.RI [ options ]
.SH DESCRIPTION
Query SlickNat daemon for IPv6 NAT mapping information.
.SH COMMANDS
.TP
.B get2kip [ip]
Get global unicast IP for local/specified IP
.TP
.B resolve <ip>
Resolve IP address mapping
.TP
.B ping
Ping the daemon
.SH EXAMPLES
.TP
slnatc ::1 get2kip 7607:af56:abb1:c7::100
.TP
slnatc 7000::1 ping
.SH SEE ALSO
.BR slick-nat-daemon (8)
EOF

# Create changelog
cat > "$CLIENT_PKG_DIR/usr/share/doc/slick-nat-client/changelog.Debian" << EOF
slick-nat-client (1.0.0) unstable; urgency=low

  * Initial release of SlickNat client

 -- SlickNat Team <team@slicknat.org>  $(date -R)
EOF

# Compress files
gzip -9 "$CLIENT_PKG_DIR/usr/share/doc/slick-nat-client/changelog.Debian"
gzip -9 "$CLIENT_PKG_DIR/usr/share/man/man1/slnatc.1"

# Set permissions
chmod 755 "$CLIENT_PKG_DIR/usr/bin/slnatc"

# Build package
echo "Building client .deb package..."
cd "$BUILD_DIR"

# Calculate package size and update control file
CLIENT_SIZE=$(du -sk slick-nat-client | cut -f1)
echo "Installed-Size: $CLIENT_SIZE" >> slick-nat-client/DEBIAN/control

# Build client package
dpkg-deb --build slick-nat-client
echo "Client package built: $BUILD_DIR/slick-nat-client.deb"

# Copy package to output directory
OUTPUT_DIR="$PROJECT_ROOT/pkg/output"
mkdir -p "$OUTPUT_DIR"
cp slick-nat-client.deb "$OUTPUT_DIR/"

echo "Client package built successfully!"
echo "Package: $OUTPUT_DIR/slick-nat-client.deb"
echo ""
echo "To install:"
echo "  sudo dpkg -i $OUTPUT_DIR/slick-nat-client.deb"
echo ""
echo "To use:"
echo "  slnatc ::1 ping"
echo "  slnatc ::1 get2kip <ip_address>"
