# Install dependencies (Ubuntu/Debian)
sudo apt update
sudo apt install -y clang llvm libbpf-dev linux-headers-$(uname -r) linux-headers-generic linux-libc-dev iproute2 build-essential

make clean
# Build
make

# Install and run
#sudo make install
