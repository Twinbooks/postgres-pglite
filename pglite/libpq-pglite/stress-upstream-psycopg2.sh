#!/bin/bash

set -euo pipefail

. "$(cd "$(dirname "$0")" && pwd)/upstream-psycopg2-env.sh"

rm -rf "$DATA_DIR"
mkdir -p "$DATA_DIR"

WORK_DIR="$WORK_DIR" DATA_DIR="$DATA_DIR" "$VENV_DIR/bin/python" -X faulthandler "$SCRIPT_DIR/stress-upstream-psycopg2.py"
