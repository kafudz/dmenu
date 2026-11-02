#!/bin/sh
# Regression tests for desktop.c. Usage: desktop_test.sh /path/to/desktop_dump
dump=$1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
fail=0

mk() { # mk dir file name [extra lines...]
	d=$1 f=$2 n=$3; shift 3
	mkdir -p "$tmp/$d/applications"
	{ printf '[Desktop Entry]\nType=Application\nName=%s\n' "$n"; for l; do printf '%s\n' "$l"; done; } \
		> "$tmp/$d/applications/$f.desktop"
}
run() { XDG_DATA_HOME=$tmp/home XDG_DATA_DIRS=$tmp/sys1:$tmp/sys2 "$@" "$dump"; }
check() { # check description expected actual
	if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1"; echo "  want: $2"; echo "  got:  $3"; fail=1; fi
}
has()  { printf '%s\n' "$2" | grep -q "^$3"; }

mk sys1 plain Plain 'Exec=plain %U'
mk sys1 quote "Bob's App" 'Exec=run %c'
mk sys1 term Term 'Exec=htop' 'Terminal=true'
mk sys1 nodisp NoDisp 'Exec=x' 'NoDisplay=true'
mk sys1 hid Hid 'Exec=x' 'Hidden=1'
mk sys1 gnome GnomeOnly 'Exec=g' 'OnlyShowIn=GNOME;'
mk sys1 kde KdeOnly 'Exec=k' 'OnlyShowIn=KDE;'
mk sys1 notkde NotKde 'Exec=n' 'NotShowIn=KDE;'
mk sys1 trymiss TryMiss 'Exec=x' 'TryExec=definitely-not-installed'
mk sys1 tryok TryOk 'Exec=x' 'TryExec=sh'
mk sys1 over OverSys 'Exec=sys'
mk home over OverUser 'Exec=user'
mk sys2 over OverSys2 'Exec=sys2'
mk home mask MaskUser 'Exec=x' 'Hidden=true'
mk sys2 mask MaskSys 'Exec=x'
mk sys2 act Actions 'Exec=main' '' '[Desktop Action a]' 'Name=A' 'Exec=wrong'
mk sys2 noexec NoExec
mk sys2 emptyexec EmptyExec 'Exec='
mk sys2 ctl 'Two\nLines' 'Exec=c'
mk sys2 zed alpha 'Exec=a'
mk sys2 zee Beta 'Exec=b'
printf '[Desktop Entry]\r\nType=Link\r\nName=Lnk\r\nURL=x\r\n' > "$tmp/sys2/applications/link.desktop"
printf '[Desktop Entry]\r\nType=Application\r\nName=Crlf\r\nExec=crlf\r\n' > "$tmp/sys2/applications/crlf.desktop"

out=$(run env XDG_CURRENT_DESKTOP=ubuntu:GNOME)
tab=$(printf '\t')
has x "$out" "Plain${tab}plain *$"       && echo "ok   field codes dropped"          || { echo "FAIL field codes dropped"; fail=1; }
has x "$out" "Bob's App${tab}run 'Bob'\\\\''s App'$" && echo "ok   %c is shell-quoted" || { echo "FAIL %c is shell-quoted"; fail=1; }
has x "$out" "Term${tab}xterm -e htop$"  && echo "ok   Terminal=true wrapped (xterm)"  || { echo "FAIL terminal wrap"; fail=1; }
has x "$out" "GnomeOnly"  && echo "ok   OnlyShowIn matches a component of ubuntu:GNOME" || { echo "FAIL OnlyShowIn list"; fail=1; }
has x "$out" "NotKde"     && echo "ok   NotShowIn=KDE shown on GNOME" || { echo "FAIL NotShowIn"; fail=1; }
has x "$out" "TryOk"      && echo "ok   TryExec found"  || { echo "FAIL TryExec found"; fail=1; }
has x "$out" "OverUser"   && echo "ok   user dir overrides system" || { echo "FAIL override"; fail=1; }
has x "$out" "Crlf${tab}crlf$" && echo "ok   CRLF files parse" || { echo "FAIL CRLF"; fail=1; }
has x "$out" "Actions${tab}main$" && echo "ok   [Desktop Action] ignored" || { echo "FAIL actions"; fail=1; }
has x "$out" "Two Lines"  && echo "ok   newline in Name neutralised" || { echo "FAIL control chars"; fail=1; }
for bad in KdeOnly NoDisp Hid TryMiss MaskUser MaskSys NoExec EmptyExec Lnk OverSys; do
	if has x "$out" "$bad"; then echo "FAIL $bad should be hidden"; fail=1; else echo "ok   $bad hidden"; fi
done
check "sorted case-insensitively" "alpha
Beta" "$(printf '%s\n' "$out" | cut -f1 | grep -E '^(alpha|Beta)$')"
has x "$(run env XDG_CURRENT_DESKTOP=KDE)" KdeOnly && echo "ok   KdeOnly shown on KDE" || { echo "FAIL KDE"; fail=1; }
has x "$(run env TERMINAL=st XDG_CURRENT_DESKTOP=x)" "Term${tab}st -e htop$" && echo "ok   \$TERMINAL honoured" || { echo "FAIL TERMINAL"; fail=1; }
check "no dirs -> empty list" "" "$(XDG_DATA_HOME=/nonexistent XDG_DATA_DIRS=/nonexistent "$dump")"
exit $fail
