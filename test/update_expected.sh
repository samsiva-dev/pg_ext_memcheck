#!/usr/bin/env bash
# update_expected.sh
#
# Re-generates all test/expected/*.out files by running each SQL test through
# psql and capturing the actual output.  Use this once after a schema or
# behaviour change to seed fresh baselines, then commit the results.
#
# Usage:
#   ./test/update_expected.sh [--dbname <db>] [--host <host>] [--port <port>]
#
# Defaults to the local PostgreSQL instance (same as 'make installcheck').

set -euo pipefail

DBNAME="${PGDATABASE:-postgres}"
HOST="${PGHOST:-}"
PORT="${PGPORT:-5432}"
USER="${PGUSER:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dbname) DBNAME="$2"; shift 2 ;;
        --host)   HOST="$2";   shift 2 ;;
        --port)   PORT="$2";   shift 2 ;;
        --user)   USER="$2";   shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SQL_DIR="$SCRIPT_DIR/sql"
EXPECTED_DIR="$SCRIPT_DIR/expected"

mkdir -p "$EXPECTED_DIR"

PSQL_OPTS=(-d "$DBNAME" -p "$PORT" --no-psqlrc -v ON_ERROR_STOP=0)
[[ -n "$HOST" ]] && PSQL_OPTS+=(-h "$HOST")
[[ -n "$USER" ]] && PSQL_OPTS+=(-U "$USER")

for sql_file in "$SQL_DIR"/*.sql; do
    name="$(basename "$sql_file" .sql)"
    out_file="$EXPECTED_DIR/${name}.out"
    echo "Generating $out_file ..."
    psql "${PSQL_OPTS[@]}" -f "$sql_file" > "$out_file" 2>&1 || true
done

echo "Done.  Review test/expected/ and commit any changes."
