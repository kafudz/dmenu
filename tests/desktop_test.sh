#!/bin/sh
# Regression tests for desktop.c. Usage: desktop_test.sh /path/to/desktop_dump
#
# desktop_dump prints one line per app: name<TAB>exec<TAB>[arg0][arg1]...
dump=$1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
fail=0
tab=$(printf '\t')

mk() { # mk dir file name [extra lines...]
	d=$1 f=$2 n=$3; shift 3
	mkdir -p "$tmp/$d/applications"
	{ printf '[Desktop Entry]\nType=Application\nName=%s\n' "$n"; for l; do printf '%s\n' "$l"; done; } \
		> "$tmp/$d/applications/$f.desktop"
}
run() { XDG_DATA_HOME=$tmp/home XDG_DATA_DIRS=$tmp/sys1:$tmp/sys2 "$@" "$dump"; }
has() { printf '%s\n' "$1" | cut -f1 | grep -qxF -- "$2"; }            # app listed?
argv() { printf '%s\n' "$1" | awk -F'\t' -v n="$2" '$1 == n { print $3 }'; } # its argv column
exec_line() { printf '%s\n' "$1" | awk -F'\t' -v n="$2" '$1 == n { print $2 }'; }
check() { # check description expected actual
	if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1"; echo "  want: $2"; echo "  got:  $3"; fail=1; fi
}
listed() { # listed description name output
	if has "$3" "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi
}
hidden() { # hidden name output
	if has "$2" "$1"; then echo "FAIL $1 should be hidden"; fail=1; else echo "ok   $1 hidden"; fi
}

mk sys1 plain Plain 'Exec=plain %U'
mk sys1 quote "Bob's App" 'Exec=run %c'
mk sys1 term Term 'Exec=htop' 'Terminal=true'
mk sys1 nodisp NoDisp 'Exec=x' 'NoDisplay=true'
mk sys1 hid Hid 'Exec=x' 'Hidden=1'
mk sys1 gnome GnomeOnly 'Exec=g' 'OnlyShowIn=GNOME;'
mk sys1 kde KdeOnly 'Exec=k' 'OnlyShowIn=KDE;'
mk sys1 both BothDesktops 'Exec=b' 'OnlyShowIn=GNOME;KDE;'
mk sys1 escsep EscapedSemicolon 'Exec=es' 'OnlyShowIn=GNOME\;KDE;'
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
check "field codes dropped, no empty argument" "[plain]" "$(argv "$out" Plain)"
check "%c stays one argument" "[run][Bob's App]" "$(argv "$out" "Bob's App")"
check "display line of %c is shell-safe" "run 'Bob'\\''s App'" "$(exec_line "$out" "Bob's App")"
check "Terminal=true wrapped (xterm)" "[xterm][-e][htop]" "$(argv "$out" Term)"
listed "OnlyShowIn matches a component of ubuntu:GNOME" GnomeOnly "$out"
listed "OnlyShowIn list matches any item" BothDesktops "$out"
listed "NotShowIn=KDE shown on GNOME" NotKde "$out"
listed "TryExec found" TryOk "$out"
listed "user dir overrides system" OverUser "$out"
check "CRLF files parse" "[crlf]" "$(argv "$out" Crlf)"
check "[Desktop Action] ignored" "[main]" "$(argv "$out" Actions)"
listed "newline in Name neutralised" "Two Lines" "$out"
for bad in KdeOnly EscapedSemicolon NoDisp Hid TryMiss MaskUser MaskSys NoExec EmptyExec Lnk OverSys; do
	hidden "$bad" "$out"
done
check "sorted case-insensitively" "alpha
Beta" "$(printf '%s\n' "$out" | cut -f1 | grep -E '^(alpha|Beta)$')"
listed "KdeOnly shown on KDE" KdeOnly "$(run env XDG_CURRENT_DESKTOP=KDE)"
check "\$TERMINAL with arguments honoured" "[st][--class][x][-e][htop]" \
	"$(argv "$(run env TERMINAL='st --class x' XDG_CURRENT_DESKTOP=x)" Term)"

# Exec= is parsed per the Desktop Entry spec and never reaches a shell
mk sys1 meta Meta 'Exec=echo "a b" '"'c;d'"' e;f $(touch /tmp/pwned) `id` > /tmp/x && y %U'
mk sys1 esc Esc 'Exec=prog "say \\"hi\\" \\$HOME" ""  x'
mk sys1 icon Icon 'Exec=app %i --file=%f %c' 'Icon=ic'
mk sys1 noicon NoIcon 'Exec=app %i end'
mk sys1 badquote BadQuote 'Exec=app "oops'
mk sys1 onlycode OnlyCode 'Exec=%U'
mk sys1 pct Pct 'Exec=app 100%% %z'
out2=$(run env XDG_CURRENT_DESKTOP=x)
check "shell syntax in Exec stays literal argument text" \
	"[echo][a b]['c;d'][e;f][\$(touch][/tmp/pwned)][\`id\`][>][/tmp/x][&&][y]" "$(argv "$out2" Meta)"
check "quoted escapes and empty argument" '[prog][say "hi" $HOME][][x]' "$(argv "$out2" Esc)"
check "%i, %f and %c expansion" "[app][--icon][ic][--file=][Icon]" "$(argv "$out2" Icon)"
check "%i without Icon= expands to nothing" "[app][end]" "$(argv "$out2" NoIcon)"
check "%% is a literal percent" "[app][100%]" "$(argv "$out2" Pct)"
hidden BadQuote "$out2"   # unterminated quote
hidden OnlyCode "$out2"   # nothing left to run

# security: non-regular and oversized files must be ignored, never block
mkfifo "$tmp/sys2/applications/fifo.desktop"
ln -s /dev/zero "$tmp/sys2/applications/zero.desktop"
{ printf '[Desktop Entry]\nType=Application\nName=Huge\nExec='; head -c 2000000 /dev/zero | tr '\0' a; echo; } > "$tmp/sys2/applications/huge.desktop"
mk sys2 realtarget LinkedOk 'Exec=linked'
ln -s "$tmp/sys2/applications/realtarget.desktop" "$tmp/sys2/applications/symlink.desktop"
sec=$(run timeout 10 env XDG_CURRENT_DESKTOP=x); rc=$?
if [ $rc -eq 0 ]; then echo "ok   FIFO/device .desktop files do not hang"; else echo "FAIL hang or crash (rc=$rc)"; fail=1; fi
hidden Huge "$sec"
listed "symlinked .desktop still loads" LinkedOk "$sec"
rm -f "$tmp/sys2/applications/fifo.desktop" "$tmp/sys2/applications/zero.desktop"
check "no dirs -> empty list" "" "$(XDG_DATA_HOME=/nonexistent XDG_DATA_DIRS=/nonexistent "$dump")"
exit $fail
