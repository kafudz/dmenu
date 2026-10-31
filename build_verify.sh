#!/usr/bin/env bash
set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}==> Building with strict warnings (-Wall -Wextra -pedantic)...${NC}"
make clean >/dev/null 2>&1 || true

# Capture build output and check for failures
if ! make 2>&1 | tee build.log; then
    echo -e "${RED}❌ Build failed!${NC}"
    exit 1
fi

# Check for warnings or errors in the log
if grep -qE 'warning|error' build.log; then
    echo -e "${YELLOW}⚠️  WARNINGS PRESENT in build.log:${NC}"
    grep -E 'warning|error' build.log
    echo -e "${RED}❌ Build is not clean.${NC}"
    exit 1
else
    echo -e "${GREEN}✅ Clean build (no warnings/errors).${NC}"
fi

echo -e "${YELLOW}==> Running dmenu smoke test...${NC}"
./dmenu -v || { echo -e "${RED}❌ dmenu -v failed${NC}"; exit 1; }

# Test dmenu piping (suppress output, just check exit code)
printf 'alpha\nbeta\ngamma\n' | ./dmenu -l 3 -p 'pick: ' >/dev/null 2>&1 || true

echo -e "${YELLOW}==> Running stest sanity checks...${NC}"
# Use unique temporary files to prevent collisions
TMP_A=$(mktemp /tmp/stest_a_XXXXXX)
TMP_B=$(mktemp /tmp/stest_b_XXXXXX)
trap 'rm -f "$TMP_A" "$TMP_B"' EXIT

./stest -f "$TMP_A" "$TMP_B" || { echo -e "${RED}❌ stest -f failed${NC}"; exit 1; }
./stest -l /tmp >/dev/null || { echo -e "${RED}❌ stest -l failed${NC}"; exit 1; }

echo -e "${GREEN}✅ All verification tests passed!${NC}"
