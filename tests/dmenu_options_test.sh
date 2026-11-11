#!/bin/sh
# Regression tests for command-line option parsing.
# Usage: dmenu_options_test.sh /path/to/dmenu
#
# Every case ends with -v, which exits before opening the display, so these
# tests need no X server.
dmenu=$1
fail=0

check() { # description expected actual
	if [ "$2" = "$3" ]; then
		echo "ok   $1"
	else
		echo "FAIL $1"
		echo "  want: $2"
		echo "  got:  $3"
		fail=1
	fi
}

version() { # description args...
	desc=$1
	shift
	out=$("$dmenu" "$@" 2>&1)
	rc=$?
	check "$desc: exit status" 0 "$rc"
	case $out in
	dmenu-*)
		echo "ok   $desc: version printed"
		;;
	*)
		echo "FAIL $desc: version printed"
		echo "  got:  $out"
		fail=1
		;;
	esac
}

# -bw used to be written as MIN(parsenum(argv[++i], ...), MAX_BORDER), which
# evaluated ++i twice and swallowed the following option.
version "-bw followed by -bc" -bw 3 -bc red -v
version "-bw followed by -v" -bw 3 -v
version "-bw clamped above the maximum" -bw 1001 -bc red -v
version "-op followed by -bw" -op 50 -bw 3 -bc red -v
version "-ip and -pp followed by -v" -ip 4 -pp 5 -v

out=$("$dmenu" -bw -3 -v 2>&1)
rc=$?
check "negative -bw is rejected" 1 "$rc"

out=$("$dmenu" -bw nope -v 2>&1)
rc=$?
check "non-numeric -bw is rejected" 1 "$rc"

"$dmenu" -bw >/dev/null 2>&1
rc=$?
check "missing -bw argument exits 2" 2 "$rc"

exit $fail
