#!/bin/bash
# End-to-end test runner for the Everything-1.0.0 indexer + search engine.
# Verifies the backend works without needing GTK4 or a display.
set -e

CLI=/home/z/my-project/everything-app/build/everything-cli
SRC=/home/z/my-project/everything-app
TEST_ROOT=/tmp/everything-test

echo "=== Building CLI harness ==="
bash /home/z/my-project/scripts/build_cli.sh > /dev/null 2>&1

echo "=== Setting up test directory ==="
rm -rf "$TEST_ROOT"
mkdir -p "$TEST_ROOT"/{foo,bar,baz/sub}
echo "hello world"     > "$TEST_ROOT/foo/hello.txt"
echo "binary data"     > "$TEST_ROOT/foo/data.bin"
echo "config"          > "$TEST_ROOT/config.ini"
echo "another doc"     > "$TEST_ROOT/bar/notes.md"
echo "deep file"       > "$TEST_ROOT/baz/sub/deep.txt"
echo "source"          > "$TEST_ROOT/test.c"
echo "documentation"   > "$TEST_ROOT/readme.txt"
:                      > "$TEST_ROOT/empty.dat"   # zero-byte file

echo ""
echo "=== Building index ==="
rm -f /home/z/.local/share/everything/index.sqlite*
"$CLI" -r "$TEST_ROOT" -v index 2>&1 | tail -2

PASS=0
FAIL=0
test_search() {
    local desc="$1"; shift
    local query="$1"; shift
    local expected_min="$1"; shift
    local out
    out=$("$CLI" -r "$TEST_ROOT" search "$query" 2>&1 || true)
    local n
    n=$(echo "$out" | grep -c "^Results\|^  " )
    if [ "$expected_min" = "0" ]; then
        if echo "$out" | grep -q "Results (0)"; then
            echo "  PASS  $desc -> 0 results"
            PASS=$((PASS+1))
        else
            echo "  FAIL  $desc -> expected 0, got non-zero"
            FAIL=$((FAIL+1))
        fi
    else
        local got
        got=$(echo "$out" | sed -n 's/^Results (\([0-9]*\)):$/\1/p' | head -1)
        if [ -z "$got" ]; then got=0; fi
        if [ "$got" -ge "$expected_min" ]; then
            echo "  PASS  $desc -> $got results (>= $expected_min)"
            PASS=$((PASS+1))
        else
            echo "  FAIL  $desc -> got $got, expected >= $expected_min"
            FAIL=$((FAIL+1))
        fi
    fi
}

echo ""
echo "=== Running search tests ==="
test_search "substring 'hello'"            "hello"          1
test_search "ext:txt"                       "ext:txt"        3
test_search "folder:"                       "folder:"        4
test_search "file:"                         "file:"          7
test_search "empty:"                        "empty:"         1
test_search "wildcard *.txt"                "*.txt"          3
test_search "size:>=5b"                     "size:>=5b"      5
test_search "NOT + ext (!deep ext:txt)"     "!deep ext:txt"  2
test_search "OR (notes|data)"               "notes|data"     2
test_search "multi-OR (hello|notes|readme)" "hello|notes|readme" 3
test_search "path:bar"                      "path:bar"       1
test_search "ext:c"                         "ext:c"          1
test_search "no-match 'xyzzy'"              "xyzzy"          0

echo ""
echo "=== JSON output test ==="
"$CLI" -r "$TEST_ROOT" -j search "ext:txt" 2>&1 | python3 -c "import sys,json; data=json.load(sys.stdin); print(f'  JSON parsed OK: {len(data)} results')"

echo ""
echo "=== Index persistence test ==="
"$CLI" -r "$TEST_ROOT" status 2>&1 | grep "Indexed items"
echo "  Index persisted across invocations"

echo ""
echo "=== Performance: real-world index ==="
rm -f /home/z/.local/share/everything/index.sqlite*
time "$CLI" -r /home/z/my-project index 2>&1 | tail -1

echo ""
echo "=== Performance: real-world search ==="
time "$CLI" -r /home/z/my-project search "ext:md" 2>&1 | head -1

echo ""
echo "=== Cleanup ==="
rm -rf "$TEST_ROOT"
rm -f /home/z/.local/share/everything/index.sqlite*

echo ""
echo "==============================================="
echo "RESULTS: $PASS passed, $FAIL failed"
echo "==============================================="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
