#!/bin/sh
# Regression tests for dmenu_path.
# Usage: dmenu_path_test.sh /path/to/dmenu_path /path/to/stest
dmenu_path=$1
stest=$2
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
fail=0

bin="$tmp/bin"
cachedir="$tmp/cache"
cache="$cachedir/dmenu_run"
mkdir -p "$bin" || exit 1

# dmenu_path runs with PATH=$bin only, so give it the tools it uses.
for cmd in mkdir stat id rm mv cat sort; do
	ln -s "$(command -v "$cmd")" "$bin/$cmd" || exit 1
done
cp "$stest" "$bin/stest" || exit 1
chmod 755 "$bin/stest" || exit 1

for name in alpha beta; do
	: > "$bin/$name"
	chmod 755 "$bin/$name"
done

run() { PATH=$bin XDG_CACHE_HOME=$cachedir "$dmenu_path"; }
has() { printf '%s\n' "$1" | grep -qxF -- "$2"; }

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

listed() { # description name output
	if has "$3" "$2"; then
		echo "ok   $1"
	else
		echo "FAIL $1"
		fail=1
	fi
}

file_mode() {
	stat -c '%a' "$1" 2>/dev/null || stat -f '%Lp' "$1" 2>/dev/null
}

out=$(run 2>/dev/null)
listed "first run lists executables" alpha "$out"
listed "first run lists executables" beta "$out"
check "cache created" 1 "$([ -f "$cache" ] && echo 1 || echo 0)"
check "cache is not a symlink" 1 "$([ -L "$cache" ] && echo 0 || echo 1)"
check "cache mode is private" 600 "$(file_mode "$cache")"

out=$(run 2>/dev/null)
check "second run uses the cache" "$(cat "$cache")" "$out"

# A cache directory writable by other users must not be used at all.
chmod 777 "$cachedir"
rm -f "$cache"
out=$(run 2>/dev/null)
listed "shared cache directory still lists executables" alpha "$out"
check "no cache written in a shared directory" 0 "$([ -e "$cache" ] && echo 1 || echo 0)"
chmod 700 "$cachedir"

# A cache file writable by other users must be rebuilt, not trusted.
printf 'EVIL\n' > "$cache"
chmod 666 "$cache"
out=$(run 2>/dev/null)
check "writable cache contents are ignored" 0 "$(has "$out" EVIL && echo 1 || echo 0)"
check "writable cache is replaced" 1 \
	"$([ -f "$cache" ] && [ ! -L "$cache" ] && [ "$(file_mode "$cache")" = 600 ] && echo 1 || echo 0)"
listed "rebuilt cache lists executables" alpha "$out"

# A symlinked cache must not overwrite its target.
victim="$tmp/victim"
printf 'SAFE\n' > "$victim"
rm -f "$cache"
ln -s "$victim" "$cache"
out=$(run 2>/dev/null)
check "symlinked cache target is untouched" SAFE "$(cat "$victim")"
check "symlinked cache is replaced by a regular file" 1 \
	"$([ -f "$cache" ] && [ ! -L "$cache" ] && echo 1 || echo 0)"
listed "symlink rebuild lists executables" alpha "$out"

exit $fail
