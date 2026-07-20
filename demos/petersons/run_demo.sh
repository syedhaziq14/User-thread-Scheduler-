#!/bin/bash
cd "/mnt/e/OS project/uthread/demos/petersons"

# Ensure the core library is built
make -C ../.. build > /dev/null

echo "==================================================="
echo "Building and running with -O0 (No optimizations)"
echo "Expectation: Usually passes because memory operations happen exactly as written in code."
echo "==================================================="
gcc -Wall -Wextra -pthread -g -O0 -I../../include petersons.c ../../obj/uthread.o -o petersons_O0
./petersons_O0

echo ""
echo "==================================================="
echo "Building and running with -O3 (Aggressive optimizations)"
echo "Expectation: Likely FAILS because the compiler reorders instructions,"
echo "violating the sequential consistency assumption of Peterson's Solution."
echo "==================================================="
gcc -Wall -Wextra -pthread -g -O3 -I../../include petersons.c ../../obj/uthread.o -o petersons_O3
./petersons_O3
