#!/bin/bash
# Compile the standalone CLI harness for the indexer + search engine.
# This proves the native Linux backend works end-to-end without GTK4.
set -e

SRC=/home/z/my-project/everything-app
OUT=/home/z/my-project/everything-app/build

mkdir -p "$OUT"

CFLAGS=$(pkg-config --cflags glib-2.0 gobject-2.0 sqlite3 2>&1)
LIBS=$(pkg-config --libs   glib-2.0 gobject-2.0 sqlite3 gthread-2.0 2>&1)
CFLAGS="$CFLAGS -I$SRC/src -Wall -Wextra -Wno-unused-parameter -O2 -g -std=c11 -D_GNU_SOURCE -pthread"
LIBS="$LIBS -lpthread"

echo "=== CFLAGS: $CFLAGS"
echo "=== LIBS:   $LIBS"
echo ""

echo "=== Compiling config.c"
gcc $CFLAGS -c "$SRC/src/everything-config.c" -o "$OUT/everything-config.o"

echo "=== Compiling result-row.c"
gcc $CFLAGS -c "$SRC/src/everything-result-row.c" -o "$OUT/everything-result-row.o"

echo "=== Compiling index.c"
gcc $CFLAGS -c "$SRC/src/everything-index.c" -o "$OUT/everything-index.o"

echo "=== Compiling search.c"
gcc $CFLAGS -c "$SRC/src/everything-search.c" -o "$OUT/everything-search.o"

echo "=== Compiling everything-cli.c"
gcc $CFLAGS -c "$SRC/tests/everything-cli.c" -o "$OUT/everything-cli.o"

echo "=== Linking"
gcc $CFLAGS -o "$OUT/everything-cli" \
    "$OUT/everything-cli.o" \
    "$OUT/everything-config.o" \
    "$OUT/everything-result-row.o" \
    "$OUT/everything-index.o" \
    "$OUT/everything-search.o" \
    $LIBS

echo ""
echo "=== Build OK ==="
ls -la "$OUT/everything-cli"
"$OUT/everything-cli" --help 2>&1 | head -10
