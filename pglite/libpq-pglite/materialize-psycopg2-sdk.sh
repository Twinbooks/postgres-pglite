#!/bin/bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
RUNTIME_DIR=${INSTALL_FOLDER:-"$ROOT_DIR/pglite/out/native"}
SDK_DIR=${SDK_DIR:-"$RUNTIME_DIR/psycopg2-sdk"}
REAL_PG_CONFIG="$ROOT_DIR/src/bin/pg_config/pg_config"

copy_public_headers() {
  mkdir -p "$SDK_DIR/include" "$SDK_DIR/include/libpq"
  cp "$ROOT_DIR/src/interfaces/libpq/libpq-fe.h" "$SDK_DIR/include/"
  cp "$ROOT_DIR/src/interfaces/libpq/libpq-events.h" "$SDK_DIR/include/"
  cp "$ROOT_DIR/src/include/postgres_ext.h" "$SDK_DIR/include/"
  cp "$ROOT_DIR/src/include/pg_config_ext.h" "$SDK_DIR/include/"
  cp "$ROOT_DIR/src/include/pg_config_manual.h" "$SDK_DIR/include/"
  cp "$ROOT_DIR/src/include/libpq/libpq-fs.h" "$SDK_DIR/include/libpq/"
}

replace_install_name_if_present() {
  local target_file="$1"
  local old_name="$2"
  local new_name="$3"

  if otool -L "$target_file" | awk '{print $1}' | grep -Fxq "$old_name"; then
    install_name_tool -change "$old_name" "$new_name" "$target_file"
  fi
}

materialize_macos_sdk() {
  local runtime_lib="$RUNTIME_DIR/lib"
  local sdk_lib="$SDK_DIR/lib"
  local sdk_wrapper="$sdk_lib/libpq.5.dylib"
  local sdk_libpglite="$sdk_lib/libpglite.0.dylib"
  local runtime_libpglite="$runtime_lib/libpglite.0.dylib"
  local runtime_real_libpq="$runtime_lib/libpq.5.dylib"

  mkdir -p "$sdk_lib"
  cp "$runtime_lib/libpqpglite.5.dylib" "$sdk_wrapper"
  cp "$runtime_lib/libpglite.0.dylib" "$sdk_libpglite"
  ln -sf "libpq.5.dylib" "$sdk_lib/libpq.dylib"
  ln -sf "libpglite.0.dylib" "$sdk_lib/libpglite.dylib"

  install_name_tool -id "$sdk_libpglite" "$sdk_libpglite"
  replace_install_name_if_present "$sdk_libpglite" "@loader_path/libpq.5.dylib" "$runtime_real_libpq"

  install_name_tool -id "$sdk_wrapper" "$sdk_wrapper"
  replace_install_name_if_present "$sdk_wrapper" "@loader_path/libpglite.0.dylib" "$sdk_libpglite"
  replace_install_name_if_present "$sdk_wrapper" "$runtime_libpglite" "$sdk_libpglite"

  if command -v codesign >/dev/null 2>&1; then
    codesign --force -s - "$sdk_libpglite" >/dev/null
    codesign --force -s - "$sdk_wrapper" >/dev/null
  fi
}

materialize_linux_sdk() {
  local runtime_lib="$RUNTIME_DIR/lib"
  local sdk_lib="$SDK_DIR/lib"
  local runtime_wrapper
  local runtime_libpglite

  runtime_wrapper=$(find "$runtime_lib" -maxdepth 1 -name 'libpqpglite.so*' | sort | head -n 1)
  runtime_libpglite=$(find "$runtime_lib" -maxdepth 1 -name 'libpglite.so*' | sort | head -n 1)

  if [ -z "$runtime_wrapper" ] || [ -z "$runtime_libpglite" ]; then
    echo "linux sdk materialization requires libpqpglite.so* and libpglite.so* in $runtime_lib" >&2
    exit 1
  fi

  mkdir -p "$sdk_lib"
  cp "$runtime_wrapper" "$sdk_lib/$(basename "$runtime_wrapper")"
  cp "$runtime_libpglite" "$sdk_lib/$(basename "$runtime_libpglite")"
  ln -sf "$(basename "$runtime_wrapper")" "$sdk_lib/libpq.so.5"
  ln -sf "libpq.so.5" "$sdk_lib/libpq.so"
}

write_pg_config() {
  local sdk_bin="$SDK_DIR/bin"
  local script_path="$sdk_bin/pg_config"

  mkdir -p "$sdk_bin"
  cat > "$script_path" <<EOF
#!/bin/sh
set -eu
REAL_PG_CONFIG="$REAL_PG_CONFIG"
SDK_DIR="$SDK_DIR"
case "\${1:-}" in
  --bindir) printf '%s\n' "\$SDK_DIR/bin" ;;
  --libdir) printf '%s\n' "\$SDK_DIR/lib" ;;
  --includedir) printf '%s\n' "\$SDK_DIR/include" ;;
  --pkgincludedir) printf '%s\n' "\$SDK_DIR/include" ;;
  --includedir-server) printf '%s\n' "\$SDK_DIR/include" ;;
  --ldflags) printf '%s\n' "-L\$SDK_DIR/lib" ;;
  --cppflags) printf '%s\n' "-I\$SDK_DIR/include -I\$SDK_DIR/include/libpq" ;;
  *) exec "\$REAL_PG_CONFIG" "\$@" ;;
esac
EOF
  chmod +x "$script_path"
}

rm -rf "$SDK_DIR"
mkdir -p "$SDK_DIR"
ln -sfn "$RUNTIME_DIR/share" "$SDK_DIR/share"
copy_public_headers

case "$(uname -s)" in
  Darwin) materialize_macos_sdk ;;
  Linux) materialize_linux_sdk ;;
  *)
    echo "unsupported platform for psycopg2 sdk: $(uname -s)" >&2
    exit 1
    ;;
esac

write_pg_config

echo "psycopg2 sdk written to $SDK_DIR"
