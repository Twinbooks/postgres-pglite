#!/bin/bash

set -euo pipefail

. "$(cd "$(dirname "$0")" && pwd)/upstream-psycopg2-env.sh"

if [ ! -f "$PATHLIKE_PATCH" ]; then
  echo "missing upstream psycopg2 patch: $PATHLIKE_PATCH" >&2
  exit 1
fi

if [ ! -f "$DISTNAME_PATCH" ]; then
  echo "missing upstream psycopg2 patch: $DISTNAME_PATCH" >&2
  exit 1
fi

if [ ! -f "$DARWIN_OPENSSL_PATCH" ]; then
  echo "missing upstream psycopg2 patch: $DARWIN_OPENSSL_PATCH" >&2
  exit 1
fi

"$SCRIPT_DIR/materialize-psycopg2-sdk.sh"

rm -rf "$WORK_DIR"
mkdir -p "$DOWNLOAD_DIR" "$WORK_DIR/src"

"$PYTHON_BIN" -m venv "$VENV_DIR"
"$VENV_DIR/bin/python" -m pip install --upgrade pip setuptools wheel >/dev/null
"$PYTHON_BIN" - "$PSYCOPG2_SDIST_URL" "$SDIST_PATH" <<'PY'
import shutil
import sys
import urllib.request

url, destination = sys.argv[1], sys.argv[2]
with urllib.request.urlopen(url) as response, open(destination, "wb") as target:
    shutil.copyfileobj(response, target)
PY
tar -xzf "$SDIST_PATH" -C "$WORK_DIR/src"

patch -d "$SOURCE_DIR" -p1 < "$PATHLIKE_PATCH" >/dev/null
patch -d "$SOURCE_DIR" -p1 < "$DISTNAME_PATCH" >/dev/null
patch -d "$SOURCE_DIR" -p1 < "$DARWIN_OPENSSL_PATCH" >/dev/null

echo "prepared upstream psycopg2 source in $SOURCE_DIR"
