#!/bin/bash

set -euo pipefail

. "$(cd "$(dirname "$0")" && pwd)/upstream-psycopg2-env.sh"

rm -rf "$DATA_DIR"
mkdir -p "$DATA_DIR"

"$VENV_DIR/bin/python" -X faulthandler - <<EOF
import psycopg2
psycopg2.connect(pglite_data_dir="$WORK_DIR/check-kw-bare").close()
EOF

"$VENV_DIR/bin/python" -X faulthandler - <<EOF
import psycopg2
psycopg2.connect("$WORK_DIR/check-positional-bare").close()
EOF

"$VENV_DIR/bin/python" -X faulthandler - <<EOF
from pathlib import Path
import psycopg2
psycopg2.connect(Path("$WORK_DIR/check-pathlike-bare")).close()
EOF

DATA_DIR="$DATA_DIR" "$VENV_DIR/bin/python" -X faulthandler "$SCRIPT_DIR/smoke-upstream-psycopg2.py"
