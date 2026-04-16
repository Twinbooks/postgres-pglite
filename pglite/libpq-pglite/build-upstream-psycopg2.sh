#!/bin/bash

set -euo pipefail

. "$(cd "$(dirname "$0")" && pwd)/upstream-psycopg2-env.sh"

if [ ! -d "$SOURCE_DIR" ] || [ ! -f "$SOURCE_DIR/setup.py" ]; then
  echo "run ./prepare-upstream-psycopg2.sh first" >&2
  exit 1
fi

(
  cd "$SOURCE_DIR"
  PATH="$SDK_DIR/bin:$PATH" \
  "$VENV_DIR/bin/python" setup.py build_ext --pg-config "$SDK_DIR/bin/pg_config" install >/dev/null
)

echo "upstream psycopg2 installed into $VENV_DIR"
