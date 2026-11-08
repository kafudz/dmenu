#!/bin/sh
# Regression tests for stest. Usage: stest_test.sh /path/to/stest
stest=$1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
fail=0

check() { # check description expected actual
	if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1"; echo "  want: $2"; echo "  got:  $3"; fail=1; fi
}

mkdir "$tmp/d"
: > "$tmp/d/file"
ln -s "$tmp/missing" "$tmp/d/broken"

# A failed stat() while walking a directory must not be reported as a readdir()
# error when EOF is reached. The broken symlink is ignored, the file is listed.
out=$("$stest" -l "$tmp/d" 2>"$tmp/err")
rc=$?
check "directory listing finds the regular file" "file" "$out"
if [ $rc -eq 0 ]; then echo "ok   listing returns success"; else echo "FAIL listing returns success (rc=$rc)"; fail=1; fi
if [ -s "$tmp/err" ]; then
	echo "FAIL spurious readdir error: $(cat "$tmp/err")"
	fail=1
else
	echo "ok   no spurious readdir error"
fi

# An unknown option is an error (exit status 2), as for test(1).
"$stest" -Z >/dev/null 2>&1
check "unknown option exits 2" "2" "$?"

exit $fail
