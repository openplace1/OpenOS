#!/bin/sh
# Builds and runs the host tests with g++ (or clang++ via CXX=clang++).
set -e
cd "$(dirname "$0")"
CXX="${CXX:-g++}"
"$CXX" -std=c++11 -Wall -Wextra -DOPENOS_HOST_TEST -Ishim -I../../src/Runtime \
    test_main.cpp ../../src/Runtime/OtaManifest.cpp -o host_tests
./host_tests
