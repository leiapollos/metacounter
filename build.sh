#!/bin/bash

set -e

mkdir -p bin

echo "Building metacounter for Linux/macOS..."
gcc -O2 -Wall -o bin/metacounter metacounter.c

echo "Build successful! Executable is 'bin/metacounter'."
echo "Run it with: ./bin/metacounter"