# quick regression check
./dmenu -v
printf 'alpha\nbeta\ngamma\n' | ./dmenu -l 3 -p 'pick: '

# stest sanity
touch /tmp/a /tmp/b
./stest -f /tmp/a /tmp/b
./stest -l /tmp