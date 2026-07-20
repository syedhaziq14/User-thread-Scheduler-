#!/bin/bash
cd "/mnt/e/OS project/uthread"

PASS=0
FAIL=0

for run in 1 2 3 4 5; do
    echo "=== Run $run ==="
    output=$(./tests/test_semaphore 2>&1)
    echo "$output"

    # Extract consumed item numbers and verify all 20 present exactly once
    items=$(echo "$output" | grep "consumed item" | sed 's/.*consumed item *//' | sort -n)
    expected=$(seq 0 19 | tr '\n' ' ' | sed 's/ $//')
    actual=$(echo "$items" | tr '\n' ' ' | sed 's/ $//')

    if [ "$actual" = "$expected" ]; then
        echo ">> RESULT: PASS (all 20 items consumed exactly once)"
        PASS=$((PASS + 1))
    else
        echo ">> RESULT: FAIL"
        echo "   Expected: $expected"
        echo "   Got:      $actual"
        FAIL=$((FAIL + 1))
    fi
    echo ""
done

echo "=============================="
echo "PASSED: $PASS / 5    FAILED: $FAIL / 5"
