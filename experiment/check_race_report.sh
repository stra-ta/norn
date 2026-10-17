#!/bin/sh
# Test wrapper for the weakened-order probe: pass only when ThreadSanitizer
# actually reports a data race. A nonzero exit alone is not trusted, because a
# probe that runs clean (race missed) also exits nonzero under some setups.
#
# Usage: check_race_report.sh <probe-binary>

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <probe-binary>"
  exit 2
fi

output=$(mktemp) || exit 2
"$1" >"$output" 2>&1
status=$?

if grep -q "WARNING: ThreadSanitizer: data race" "$output"; then
  echo "tsan reported the expected data race (probe exit $status)"
  head -3 "$output"
  rm -f "$output"
  exit 0
fi

echo "expected a ThreadSanitizer data race report; probe exited $status"
head -5 "$output"
rm -f "$output"
exit 1
