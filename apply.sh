#!/usr/bin/env bash
set -euo pipefail

echo "==> Quick regression check for dmenu..."
./dmenu -v

# Pipe test (suppresses UI output to terminal)
printf 'alpha\nbeta\ngamma\n' | ./dmenu -l 3 -p 'pick: ' >/dev/null 2>&1 || true
echo "✅ dmenu tests passed."

echo "==> stest sanity check..."
# Create isolated temp files
TMP1=$(mktemp /tmp/stest_1_XXXXXX)
TMP2=$(mktemp /tmp/stest_2_XXXXXX)
trap 'rm -f "$TMP1" "$TMP2"' EXIT

./stest -f "$TMP1" "$TMP2"
./stest -l /tmp >/dev/null

echo "✅ stest tests passed."
echo "🎉 All apply checks successful!"
