#!/bin/bash
# two independent cores in one shared library: core A as-is, core B with every global symbol prefixed b_
set -e
cd "$(dirname "$0")/.."
mkdir -p build/sim2
python3 gen.py --interp core/cpu_interp.c 2>/dev/null
F="-O2 -w -fPIC -DGB_INTERP_ONLY -Icore"
gcc $F -c core/cpu_interp.c -o build/sim2/cpu_interp.o
gcc $F -c core/gbcore.c -o build/sim2/gbcore.o
gcc $F -c core/tetris_extras.c -o build/sim2/tetris_extras.o
ld -r -o build/sim2/core_a.o build/sim2/cpu_interp.o build/sim2/gbcore.o build/sim2/tetris_extras.o
nm -g --defined-only build/sim2/core_a.o | awk '{print $3" b_"$3}' > build/sim2/rename.txt
objcopy --redefine-syms=build/sim2/rename.txt build/sim2/core_a.o build/sim2/core_b.o
gcc -O2 -Wall -fPIC -shared ${LINKSIM_FLAGS:-} -o build/linksim.so test/linksim.c android/link.c android/cpubot.c build/sim2/core_a.o build/sim2/core_b.o -Icore
echo built build/linksim.so
