#!/bin/bash
set -e
echo "Building Slick NAT kernel module..."
make
echo "Build complete."
echo ""
echo "To load the module: ./loader.sh load"
echo "To configure NAT:   ./src/slnat <interface> add <internal> <external>"
echo "To check status:    ./src/slnat status"
