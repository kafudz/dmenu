# build with -Wall -Wextra -pedantic
make clean && make 2>&1 | tee build.log
grep -E 'warning|error' build.log && echo "WARNINGS PRESENT" || echo "clean build"

# smoke test
./dmenu -v
printf 'alpha\nbeta\ngamma\n' | ./dmenu -l 3 -p 'pick: '