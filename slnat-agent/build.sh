#!/bin/bash

# Build script for SlickNat Agent

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to print colored output
print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Default values
BUILD_TYPE="Release"
CLEAN=false
INSTALL=false
PARALLEL_JOBS=$(nproc)

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -c|--clean)
            CLEAN=true
            shift
            ;;
        -i|--install)
            INSTALL=true
            shift
            ;;
        -j|--jobs)
            PARALLEL_JOBS="$2"
            shift
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  -d, --debug      Build in debug mode"
            echo "  -c, --clean      Clean build directory before building"
            echo "  -i, --install    Install binaries after building"
            echo "  -j, --jobs N     Use N parallel jobs (default: $(nproc))"
            echo "  -h, --help       Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0                    # Build in release mode"
            echo "  $0 -d                 # Build in debug mode"
            echo "  $0 -c -i              # Clean build and install"
            echo "  $0 -j 4               # Use 4 parallel jobs"
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            exit 1
            ;;
    esac
done

print_info "Building SlickNat Agent..."
print_info "Build type: $BUILD_TYPE"
print_info "Parallel jobs: $PARALLEL_JOBS"

# Check if we're in the right directory
if [ ! -f "CMakeLists.txt" ]; then
    print_error "CMakeLists.txt not found. Please run this script from the slnat-agent directory."
    exit 1
fi

# Create build directory
BUILD_DIR="build"

if [ "$CLEAN" = true ] && [ -d "$BUILD_DIR" ]; then
    print_info "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

if [ ! -d "$BUILD_DIR" ]; then
    print_info "Creating build directory..."
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

# Check for required dependencies
print_info "Checking dependencies..."

# Check for CMake
if ! command -v cmake &> /dev/null; then
    print_error "CMake not found. Please install CMake 3.10 or later."
    exit 1
fi

# Check for C++ compiler
if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
    print_error "C++ compiler not found. Please install g++ or clang++."
    exit 1
fi

# Run CMake configuration
print_info "Configuring project..."
cmake -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      ..

if [ $? -ne 0 ]; then
    print_error "CMake configuration failed"
    exit 1
fi

# Build the project
print_info "Building project with $PARALLEL_JOBS parallel jobs..."
make -j"$PARALLEL_JOBS"

if [ $? -ne 0 ]; then
    print_error "Build failed"
    exit 1
fi

print_info "Build completed successfully!"

# Show built binaries
print_info "Built binaries:"
ls -la slick-nat-daemon slnatc 2>/dev/null || true

# Install if requested
if [ "$INSTALL" = true ]; then
    print_info "Installing binaries..."
    
    # Check if we need sudo
    if [ ! -w "/usr/local/bin" ]; then
        print_warning "Need sudo privileges to install to /usr/local/bin"
        sudo make install
    else
        make install
    fi
    
    if [ $? -eq 0 ]; then
        print_info "Installation completed successfully!"
    else
        print_error "Installation failed"
        exit 1
    fi
fi

# Go back to original directory
cd ..

print_info "Build script completed!"
print_info ""
print_info "Usage:"
print_info "  Daemon: ./build/slick-nat-daemon --address 7000::1"
print_info "  Client: ./build/slnatc 7000 get2kip"
print_info ""
print_info "For help:"
print_info "  ./build/slick-nat-daemon --help"
print_info "  ./build/slnatc 7000 --help"
