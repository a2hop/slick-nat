# SlickNat Debian Packages

This directory contains the Debian packaging files for SlickNat.

## Packages

- **slick-nat-client** - Command-line client utility (`slnatc`)
- **slick-nat-daemon** - Background daemon service (`slick-nat-daemon`)

## Building

Make sure you have the required dependencies installed:

```bash
sudo apt install build-essential dpkg-dev nlohmann-json3-dev
```

### Build Individual Packages

Build only the client:
```bash
chmod +x pkg/build-client.sh
./pkg/build-client.sh
```

Build only the daemon:
```bash
chmod +x pkg/build-daemon.sh
./pkg/build-daemon.sh
```

### Build Both Packages

Build both packages at once:
```bash
chmod +x pkg/build-packages.sh
./pkg/build-packages.sh
```

All packages will be created in `pkg/output/`.

## Installation

Install individual packages:

```bash
# Client only
sudo dpkg -i pkg/output/slick-nat-client.deb

# Daemon only
sudo dpkg -i pkg/output/slick-nat-daemon.deb

# Both packages
sudo dpkg -i pkg/output/slick-nat-client.deb pkg/output/slick-nat-daemon.deb
```

If there are dependency issues, resolve them with:

```bash
sudo apt-get install -f
```

## Usage

### Client
```bash
slnatc ::1 ping
slnatc ::1 get2kip 7607:af56:abb1:c7::100
```

### Daemon
```bash
# Start the service
sudo systemctl start slick-nat-daemon

# Enable auto-start on boot
sudo systemctl enable slick-nat-daemon

# Check status
sudo systemctl status slick-nat-daemon
```

## Configuration

The daemon configuration is in `/etc/slnatd/config`.

### Configuration Format

The configuration file supports the following directives:

```
# Listen on multiple IPv6 addresses
listen ::1 7001
listen 2001:db8::1 7001
listen fd00::1 7002

# Kernel proc file path
proc_path /proc/net/slick_nat_mappings
```

### Example Configuration

```bash
# Edit the configuration file
sudo nano /etc/slnatd/config

# Add your listening addresses
listen ::1 7001
listen 2001:db8::1 7001
listen :: 7001

# Restart the daemon
sudo systemctl restart slick-nat-daemon
```

### Multiple Address Support

The daemon can now listen on multiple IPv6 addresses simultaneously. Each `listen` directive creates a separate listening socket. This is useful for:

- Listening on localhost and specific network interfaces
- Supporting multiple network namespaces
- Providing redundancy across different addresses

## Logs

Check daemon logs with:
```bash
sudo journalctl -u slick-nat-daemon -f
```

The logs will show which addresses the daemon is listening on and client connections to each address.

## Package Structure

Each package is built independently with its own:
- Build script (`build-client.sh`, `build-daemon.sh`)
- Control file (`pkg/deb-slnatc/control`, `pkg/deb-slnatd/control`)
- Temporary build directory
- Output in `pkg/output/`

This allows for independent versioning, building, and deployment of each component.
