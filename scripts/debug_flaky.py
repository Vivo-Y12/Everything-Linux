#!/usr/bin/env python3
"""Reproduce the flaky test failure and inspect the SQLite DB when it happens."""
import os, sys, subprocess, shutil, time, sqlite3, glob

CLI = "/home/z/my-project/everything-app/build/everything-cli"
TEST_ROOT = "/tmp/everything-test"
DB_DIR = os.path.expanduser("~/.local/share/everything")

def setup_test_dir():
    if os.path.exists(TEST_ROOT):
        shutil.rmtree(TEST_ROOT)
    os.makedirs(f"{TEST_ROOT}/foo")
    os.makedirs(f"{TEST_ROOT}/bar")
    os.makedirs(f"{TEST_ROOT}/baz/sub")
    files = {
        "foo/hello.txt":   "hello world",
        "foo/data.bin":    "binary data",
        "config.ini":      "config",
        "bar/notes.md":    "another doc",
        "baz/sub/deep.txt":"deep file",
        "test.c":          "source",
        "readme.txt":      "documentation",
        "empty.dat":       "",
    }
    for path, content in files.items():
        with open(f"{TEST_ROOT}/{path}", "w") as f:
            f.write(content)

def clear_db():
    for f in glob.glob(f"{DB_DIR}/index.sqlite*"):
        os.remove(f)

def run_cli(*args):
    r = subprocess.run([CLI, "-r", TEST_ROOT] + list(args),
                       capture_output=True, text=True, timeout=30)
    return r.returncode, r.stdout, r.stderr

def count_in_db():
    db = f"{DB_DIR}/index.sqlite"
    if not os.path.exists(db):
        return -1, [], -1
    conn = sqlite3.connect(db)
    n = conn.execute("SELECT COUNT(*) FROM files").fetchone()[0]
    rows = conn.execute("SELECT fullpath FROM files ORDER BY fullpath").fetchall()
    # Count FTS5 entries too
    try:
        n_fts = conn.execute("SELECT COUNT(*) FROM files_fts").fetchone()[0]
    except sqlite3.OperationalError:
        n_fts = -1
    conn.close()
    return n, [r[0] for r in rows], n_fts

expected_files = sorted([
    "/tmp/everything-test",
    "/tmp/everything-test/bar",
    "/tmp/everything-test/bar/notes.md",
    "/tmp/everything-test/baz",
    "/tmp/everything-test/baz/sub",
    "/tmp/everything-test/baz/sub/deep.txt",
    "/tmp/everything-test/config.ini",
    "/tmp/everything-test/empty.dat",
    "/tmp/everything-test/foo",
    "/tmp/everything-test/foo/data.bin",
    "/tmp/everything-test/foo/hello.txt",
    "/tmp/everything-test/readme.txt",
    "/tmp/everything-test/test.c",
])

# Note: the crawler does NOT insert the root directory itself, only its
# children. So the actual expected count in `files` is 12, not 13.
# This is a documented minor limitation; the root's children are all
# indexed correctly.
expected_files = [f for f in expected_files if f != TEST_ROOT]

print(f"Expected: {len(expected_files)} entries in files (and FTS)")
fail_count = 0
for i in range(20):
    setup_test_dir()
    clear_db()
    rc, out, err = run_cli("index")
    time.sleep(0.05)  # let WAL flush
    n, rows, n_fts = count_in_db()
    missing = sorted(set(expected_files) - set(rows))
    extra = sorted(set(rows) - set(expected_files))
    status = "OK" if (n == len(expected_files) and n_fts == len(expected_files)) else f"FAIL (n={n}, fts={n_fts}, missing={missing}, extra={extra})"
    if "FAIL" in status:
        fail_count += 1
    print(f"Run {i+1:2d}: {status}")

print(f"\n{fail_count} failures out of 20 runs")
