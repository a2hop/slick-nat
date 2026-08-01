#!/bin/bash
#
# Development reload helper: rebuild from git, then swap the running module.
#
# Build BEFORE unloading. This script used to unload first, so a failed
# `git pull` or a broken build left the box with no module and no NAT.

set -e

cd "$(dirname "$0")"

echo "Fetching latest source..."
git pull

echo "Building..."
./build.sh

# Only now, with a known-good .ko on disk, is it safe to take the running
# module out.
./loader.sh unload
./loader.sh load

# Configure NAT mappings
./src/slnat bri1 add 7607:af56:ff8:d12::/96 2607:f8f8:631:d601:2000:d12::/96
./src/slnat bri1 add 7607:af56:abb1:c7::/96 2a0a:8dc0:509b:21::/96

#./src/slnat add-batch /etc/slick-nat/routes
