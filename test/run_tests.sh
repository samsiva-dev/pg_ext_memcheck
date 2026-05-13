#!/usr/bin/env bash
# run_tests.sh — self-contained regression runner for pg_ext_memcheck
#
# Creates a temporary PostgreSQL 17 cluster inside test/tmp_pgdata/, runs all
# pg_regress tests, then stops and removes the cluster on exit (pass or fail).
#
# Usage (from the project root):
#   ./test/run_tests.sh
#
# Optional env overrides:
#   PG_CONFIG   path to pg_config   (default: distdb postgresql-17.2)
#   PGPORT      port for temp server (default: 9752)

set -euo pipefail

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PG_CONFIG="${PG_CONFIG}"

PG_BIN="$("$PG_CONFIG" --bindir)"
PG_LIBDIR="$("$PG_CONFIG" --pkglibdir)"
PG_SHAREDIR="$("$PG_CONFIG" --sharedir)"
PG_REGRESS="$("$PG_CONFIG" --libdir)/pgxs/src/test/regress/pg_regress"

PGPORT="${PGPORT:-9752}"
PGDATA="$SCRIPT_DIR/tmp_pgdata"
PGLOG="$PGDATA/server.log"

# ---------------------------------------------------------------------------
# Cleanup trap — always runs on exit
# ---------------------------------------------------------------------------
cleanup() {
    local exit_code=$?
    echo ""
    echo "--- Stopping temporary cluster ---"
    "$PG_BIN/pg_ctl" -D "$PGDATA" stop -m fast -w 2>/dev/null || true
    rm -rf "$PGDATA"
    echo "--- Cluster removed ---"
    exit $exit_code
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Step 1 — Build and install the extension against PG 17
# ---------------------------------------------------------------------------
echo "=== Building pg_ext_memcheck against $("$PG_CONFIG" --version) ==="
cd "$ROOT_DIR"
make all PG_CONFIG="$PG_CONFIG" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "=== Installing pg_ext_memcheck to $PG_LIBDIR ==="
make install PG_CONFIG="$PG_CONFIG"

# ---------------------------------------------------------------------------
# Step 2 — initdb a fresh cluster
# ---------------------------------------------------------------------------
echo "=== Initialising temporary cluster in $PGDATA ==="
"$PG_BIN/initdb" \
    --pgdata="$PGDATA" \
    --auth=trust \
    --username=postgres \
    --no-instructions

# ---------------------------------------------------------------------------
# Step 3 — Configure the cluster
# ---------------------------------------------------------------------------
cat >> "$PGDATA/postgresql.conf" << EOF

# pg_ext_memcheck regression test overrides
port                    = $PGPORT
listen_addresses        = '127.0.0.1'
shared_preload_libraries = 'pg_ext_memcheck'
log_min_messages        = warning
EOF

# ---------------------------------------------------------------------------
# Step 4 — Start the server and wait for it to be ready
# ---------------------------------------------------------------------------
echo "=== Starting temporary server on port $PGPORT ==="
"$PG_BIN/pg_ctl" \
    -D "$PGDATA" \
    -l "$PGLOG" \
    start \
    -w \
    -o "-p $PGPORT"

# Verify the server is reachable (suppress pager)
PAGER=cat "$PG_BIN/psql" -h 127.0.0.1 -p "$PGPORT" -U postgres -d postgres \
    -c "SELECT version();" -q

# ---------------------------------------------------------------------------
# Step 5 — Run pg_regress
# ---------------------------------------------------------------------------
echo "=== Running regression tests ==="
cd "$ROOT_DIR"

"$PG_REGRESS" \
    --inputdir="$SCRIPT_DIR" \
    --outputdir="$SCRIPT_DIR" \
    --bindir="$PG_BIN" \
    --host=127.0.0.1 \
    --port="$PGPORT" \
    --user=postgres \
    --dbname=contrib_regression \
    00_setup \
    01_gucs \
    02_session_lifecycle \
    03_violation_log \
    04_scenario_growth_benchmark \
    05_scenario_tx_abort_loop \
    06_scenario_unknown \
    07_executor_hook_mode \
    08_ring_buffer_overflow \
    09_flush_clears_buffer \
    10_min_leak_bytes_threshold \
    11_violation_log_schema \
    12_idempotent_install

echo ""
echo "=== All regression tests passed ==="
