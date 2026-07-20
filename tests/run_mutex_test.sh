#!/bin/bash
cd "/mnt/e/OS project/uthread"

echo "====== WITHOUT MUTEX (race condition) ======"
./tests/test_mutex nolock

echo ""
echo "====== WITH MUTEX (5 consecutive runs) ======"
for i in 1 2 3 4 5; do
    echo -n "Run $i: "
    ./tests/test_mutex lock
done
