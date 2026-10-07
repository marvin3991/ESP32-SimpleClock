#!/usr/bin/env bash
# Builds and runs the host tests: no board needed, a C/C++ compiler and bash
# (macOS or Linux; tested with Apple clang). Exits non-zero on a failure.
#   tests/run.sh       run all
#   tests/run.sh -v    also print the firmware's log during the simulation
# CC and CXX choose the compilers (default cc and c++).
set -u
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../src"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cc="${CC:-cc}"
cxx="${CXX:-c++}"
cflags=(-O2 -Wall -Wextra)
cxxflags=(-std=gnu++17 -O2 -Wall -Wextra -I "$here/stubs" -I "$src")
failed=0

build() { "$@" || { echo "build failed: $*" >&2; exit 2; }; }

# NTP packets: conversions, offset and delay, refused replies.
build "$cxx" "${cxxflags[@]}" -o "$out/ntp_proto_test" "$here/ntp_proto_test.cpp" "$src/ntp_proto.cpp"
"$out/ntp_proto_test" || failed=1

# timekeep.cpp against simulated hardware; its clock calls go to sim_clock.c.
build "$cc" -std=gnu11 "${cflags[@]}" -c "$here/sim_clock.c" -o "$out/sim_clock.o"
build "$cxx" "${cxxflags[@]}" -Dgettimeofday=sim_gettimeofday -Dsettimeofday=sim_settimeofday -Dtime=sim_time \
    -c "$src/timekeep.cpp" -o "$out/timekeep.o"
build "$cxx" "${cxxflags[@]}" -c "$src/ntp_proto.cpp" -o "$out/ntp_proto.o"
build "$cxx" "${cxxflags[@]}" -c "$here/timekeep_sim.cpp" -o "$out/timekeep_sim.o"
build "$cxx" -o "$out/timekeep_sim" "$out/timekeep_sim.o" "$out/timekeep.o" "$out/ntp_proto.o" "$out/sim_clock.o"
"$out/timekeep_sim" "$@" || failed=1

if [ "$failed" -ne 0 ]; then echo "host tests FAILED"; else echo "host tests passed"; fi
exit "$failed"
