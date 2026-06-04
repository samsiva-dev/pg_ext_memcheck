#!/usr/bin/env bash
# run_tests.sh — self-contained regression runner for pg_ext_memcheck
#
# Creates a temporary PostgreSQL cluster (PG14–17) inside test/tmp_pgdata/,
# runs all pg_regress tests, then stops and removes the cluster on exit
# (pass or fail).
#
# Usage (from the project root):
#   ./test/run_tests.sh
#
# Optional env overrides:
#   PG_CONFIG   path to pg_config (default: pg_config from PATH)
#   PGPORT      port for temp server (default: 9752)

set -euo pipefail

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PG_CONFIG="${PG_CONFIG:-pg_config}"
ENVIRONMENT="${ENVIRONMENT:-"CI"}"

PG_BIN="$("$PG_CONFIG" --bindir)"
PG_LIBDIR="$("$PG_CONFIG" --pkglibdir)"
PG_SHAREDIR="$("$PG_CONFIG" --sharedir)"
# pg_regress may live under --libdir/pgxs (RPM/macOS) or --pkglibdir/pgxs (Debian/Ubuntu)
_pgxs_libdir="$("$PG_CONFIG" --libdir)/pgxs/src/test/regress/pg_regress"
_pgxs_pkglibdir="$("$PG_CONFIG" --pkglibdir)/pgxs/src/test/regress/pg_regress"
if [ -x "$_pgxs_libdir" ]; then
    PG_REGRESS="$_pgxs_libdir"
elif [ -x "$_pgxs_pkglibdir" ]; then
    PG_REGRESS="$_pgxs_pkglibdir"
else
    echo "ERROR: pg_regress not found (tried $\_pgxs_libdir and $_pgxs_pkglibdir)" >&2
    exit 1
fi

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
    # Print the server log before deleting PGDATA so failures are visible in CI
    if [ -f "$PGLOG" ]; then
        echo "=== PostgreSQL server log ($PGLOG) ==="
        cat "$PGLOG"
        echo "=== End of server log ==="
    fi
    "$PG_BIN/pg_ctl" -D "$PGDATA" stop -m fast -w 2>/dev/null || true
    rm -rf "$PGDATA"
    echo "--- Cluster removed ---"
    exit $exit_code
}
trap cleanup EXIT INT TERM

FIXTURE_DIR="$SCRIPT_DIR/fixture/buggy_pg_ext"

# ---------------------------------------------------------------------------
# Step 1 — Build and install pg_ext_memcheck
# ---------------------------------------------------------------------------
echo "=== Building pg_ext_memcheck against $("$PG_CONFIG" --version) ==="
cd "$ROOT_DIR"
make all PG_CONFIG="$PG_CONFIG" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "=== Installing pg_ext_memcheck to $PG_LIBDIR ==="

if [ "$ENVIRONMENT" = "CI" ]; then
    echo "CI environment detected; installing to system PostgreSQL directories"
    sudo make install PG_CONFIG="$PG_CONFIG"
else
    echo "Local environment detected; installing to user PostgreSQL directories"
    make install PG_CONFIG="$PG_CONFIG"
fi

# ---------------------------------------------------------------------------
# Step 1b — Build and install buggy_pg_ext fixture
# ---------------------------------------------------------------------------
echo "=== Building buggy_pg_ext fixture ==="
cd "$FIXTURE_DIR"
make all PG_CONFIG="$PG_CONFIG" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "=== Installing buggy_pg_ext fixture to $PG_LIBDIR ==="
if [ "$ENVIRONMENT" = "CI" ]; then
    sudo make install PG_CONFIG="$PG_CONFIG"
else
    make install PG_CONFIG="$PG_CONFIG"
fi
cd "$ROOT_DIR"

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
unix_socket_directories = '/tmp'
shared_preload_libraries = 'pg_ext_memcheck'
log_min_messages        = warning
EOF

# ---------------------------------------------------------------------------
# Step 4 — Start the server and wait for it to be ready
# ---------------------------------------------------------------------------
echo "=== Starting temporary server on port $PGPORT ==="
if ! "$PG_BIN/pg_ctl" \
    -D "$PGDATA" \
    -l "$PGLOG" \
    start \
    -w \
    -o "-p $PGPORT"; then
    echo "ERROR: pg_ctl could not start the server. Dumping log:" >&2
    cat "$PGLOG" >&2
    exit 1
fi

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
    12_idempotent_install \
    13_shmem_sentinel_probe \
    14_context_pattern_filter \
    15_buggy_ext_wrong_ctx_detection \
    16_buggy_ext_dsm_leak_detection \
    17_nested_query_analysis

echo ""
echo "=== All regression tests passed ==="
