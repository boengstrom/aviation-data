#!/bin/bash
# Start the Aviation Data Front End (CIS 51 Vibe Coding) on port 8080.
# Rebuilds automatically if the C++ source is newer than the binary.
cd "$(dirname "$0")"
if [ ! -f airline_service ] || [ airline_service.cpp -nt airline_service ]; then
  echo "Building airline_service..."
  g++ -std=c++17 -O2 -pthread -o airline_service airline_service.cpp || exit 1
fi
exec ./airline_service data
