#!/bin/bash

# Simple RDMA Build Script
# Uses CMake for building with compile_commands.json support

set -e  # Exit on error

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Build directory
BUILD_DIR="$PROJECT_ROOT/build"

# Create build directory if it doesn't exist
mkdir -p "$BUILD_DIR"

# Change to build directory
cd "$BUILD_DIR"

# Configure CMake with compile_commands.json generation
echo "Configuring CMake..."
cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Build the project
echo "Building..."
make -j$(nproc)

# Copy compile_commands.json to project root for IDE support
if [ -f "$BUILD_DIR/compile_commands.json" ]; then
    cp "$BUILD_DIR/compile_commands.json" "$PROJECT_ROOT/"
    echo "compile_commands.json copied to project root"
fi

echo "Build complete! Executables are in: $BUILD_DIR"
echo "  - $BUILD_DIR/sender"
echo "  - $BUILD_DIR/receiver"
