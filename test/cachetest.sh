#!/bin/bash
# tools/cachetest.c (the Linux L1/L2 check): its verdict on synthetic speed curves.
nb=$(dirname "$(dirname "$(readlink -f "$0")")")
mkdir -p "$nb/test/vm"
gcc -DSELFTEST -O2 -Wall -Werror -Wno-unused-function -o "$nb/test/vm/cachetest-self" "$nb/tools/cachetest.c" || { echo "FAIL [cachetest]: build"; exit 1; }
"$nb/test/vm/cachetest-self"
