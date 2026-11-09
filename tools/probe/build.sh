#!/bin/sh
# Compilation de pd_probe (sans wxWidgets) : sh tools/probe/build.sh [sortie]
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-pd_probe}
OBJ=${OBJ:-$HOME/probe-obj}; mkdir -p $OBJ
for f in third_party/sqlite/sqlite3.c third_party/miniz/miniz.c third_party/miniz/miniz_tdef.c third_party/miniz/miniz_tinfl.c; do
  o=$OBJ/$(basename "$f" .c).o
  [ -f "$o" ] || gcc -O2 -c "$f" -o "$o" -Ithird_party/miniz -Ithird_party/sqlite
done
g++ -std=c++17 -O2 -DNDEBUG -o "$OUT" tools/probe/pd_probe.cpp src/core/*.cpp $OBJ/*.o \
  -Ithird_party -Ithird_party/miniz -Ithird_party/sqlite -lpthread -ldl
