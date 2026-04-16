#!/bin/bash

set -euo pipefail

. "$(cd "$(dirname "$0")" && pwd)/upstream-psycopg2-env.sh"

replace_install_name_if_present() {
  local target_file="$1"
  local old_name="$2"
  local new_name="$3"

  if otool -L "$target_file" | awk '{print $1}' | grep -Fxq "$old_name"; then
    install_name_tool -change "$old_name" "$new_name" "$target_file"
  fi
}

repair_macos_wheel() {
  local wheel_path="$1"
  local repair_dir unpacked_dir package_dir dylib_dir ext_path module_dir share_dir
  local bundled_libpq_wrapper="$SDK_DIR/lib/libpq.5.dylib"
  local bundled_libpglite="$SDK_DIR/lib/libpglite.0.dylib"
  local bundled_real_libpq="$RUNTIME_DIR/lib/libpq.5.dylib"
  local bundled_plpgsql="$RUNTIME_DIR/lib/plpgsql.dylib"
  local bundled_dict_snowball="$RUNTIME_DIR/lib/dict_snowball.dylib"
  local bundled_share_dir="$RUNTIME_DIR/share"

  if [ "$(uname -s)" != "Darwin" ]; then
    return
  fi

  if [ ! -f "$bundled_libpq_wrapper" ] || [ ! -f "$bundled_libpglite" ] || [ ! -f "$bundled_real_libpq" ] || [ ! -d "$bundled_share_dir" ]; then
    echo "missing psycopg2 sdk dylibs for wheel repair" >&2
    exit 1
  fi

  repair_dir=$(mktemp -d "$WORK_DIR/wheel-repair.XXXXXX")
  "$VENV_DIR/bin/python" -m wheel unpack --dest "$repair_dir" "$wheel_path" >/dev/null
  unpacked_dir=$(find "$repair_dir" -mindepth 1 -maxdepth 1 -type d | head -n 1)
  if [ -z "$unpacked_dir" ]; then
    echo "failed to unpack wheel for repair: $wheel_path" >&2
    exit 1
  fi

  package_dir="$unpacked_dir/psycopg2"
  dylib_dir="$package_dir/.dylibs"
  module_dir="$package_dir/lib"
  share_dir="$package_dir/share"
  ext_path=$(find "$package_dir" -maxdepth 1 -name '_psycopg*.so' | head -n 1)
  if [ ! -f "$ext_path" ]; then
    echo "failed to locate psycopg2 extension in unpacked wheel" >&2
    exit 1
  fi

  mkdir -p "$dylib_dir" "$module_dir"
  cp "$bundled_libpq_wrapper" "$dylib_dir/libpq.5.dylib"
  cp "$bundled_libpglite" "$dylib_dir/libpglite.0.dylib"
  cp "$bundled_real_libpq" "$dylib_dir/libpq-real.5.dylib"
  if [ -f "$bundled_plpgsql" ]; then
    cp "$bundled_plpgsql" "$module_dir/plpgsql.dylib"
  fi
  if [ -f "$bundled_dict_snowball" ]; then
    cp "$bundled_dict_snowball" "$module_dir/dict_snowball.dylib"
  fi
  cp -R "$bundled_share_dir" "$share_dir"

  install_name_tool -id "@loader_path/libpq.5.dylib" "$dylib_dir/libpq.5.dylib"
  install_name_tool -id "@loader_path/libpglite.0.dylib" "$dylib_dir/libpglite.0.dylib"
  install_name_tool -id "@loader_path/libpq-real.5.dylib" "$dylib_dir/libpq-real.5.dylib"

  replace_install_name_if_present "$dylib_dir/libpq.5.dylib" "$SDK_DIR/lib/libpglite.0.dylib" "@loader_path/libpglite.0.dylib"
  replace_install_name_if_present "$dylib_dir/libpq.5.dylib" "@loader_path/libpglite.0.dylib" "@loader_path/libpglite.0.dylib"

  replace_install_name_if_present "$dylib_dir/libpglite.0.dylib" "$SDK_DIR/lib/libpq.5.dylib" "@loader_path/libpq-real.5.dylib"
  replace_install_name_if_present "$dylib_dir/libpglite.0.dylib" "$RUNTIME_DIR/lib/libpq.5.dylib" "@loader_path/libpq-real.5.dylib"
  replace_install_name_if_present "$dylib_dir/libpglite.0.dylib" "@loader_path/libpq.5.dylib" "@loader_path/libpq-real.5.dylib"

  replace_install_name_if_present "$ext_path" "$SDK_DIR/lib/libpq.5.dylib" "@loader_path/.dylibs/libpq.5.dylib"
  if [ -f "$module_dir/plpgsql.dylib" ]; then
    replace_install_name_if_present "$module_dir/plpgsql.dylib" "@loader_path/libpglite.0.dylib" "@loader_path/../.dylibs/libpglite.0.dylib"
  fi
  if [ -f "$module_dir/dict_snowball.dylib" ]; then
    replace_install_name_if_present "$module_dir/dict_snowball.dylib" "@loader_path/libpglite.0.dylib" "@loader_path/../.dylibs/libpglite.0.dylib"
  fi

  if command -v codesign >/dev/null 2>&1; then
    codesign --force -s - "$dylib_dir/libpglite.0.dylib" >/dev/null
    codesign --force -s - "$dylib_dir/libpq.5.dylib" >/dev/null
    codesign --force -s - "$dylib_dir/libpq-real.5.dylib" >/dev/null
    if [ -f "$module_dir/plpgsql.dylib" ]; then
      codesign --force -s - "$module_dir/plpgsql.dylib" >/dev/null
    fi
    if [ -f "$module_dir/dict_snowball.dylib" ]; then
      codesign --force -s - "$module_dir/dict_snowball.dylib" >/dev/null
    fi
    codesign --force -s - "$ext_path" >/dev/null
  fi

  rm -f "$wheel_path"
  "$VENV_DIR/bin/python" -m wheel pack --dest "$WHEEL_DIR" "$unpacked_dir" >/dev/null
  rm -rf "$repair_dir"
}

if [ ! -d "$SOURCE_DIR" ] || [ ! -f "$SOURCE_DIR/setup.py" ]; then
  echo "run ./prepare-upstream-psycopg2.sh first" >&2
  exit 1
fi

mkdir -p "$WHEEL_DIR"
rm -f "$WHEEL_DIR"/psycopg2_pglite-*.whl

(
  cd "$SOURCE_DIR"
  PATH="$SDK_DIR/bin:$PATH" \
  "$VENV_DIR/bin/python" setup.py build_ext --pg-config "$SDK_DIR/bin/pg_config" bdist_wheel -d "$WHEEL_DIR" >/dev/null
)

wheel_path=$(find "$WHEEL_DIR" -maxdepth 1 -name 'psycopg2_pglite-*.whl' | head -n 1)
if [ -z "$wheel_path" ]; then
  echo "failed to find built psycopg2-pglite wheel in $WHEEL_DIR" >&2
  exit 1
fi

repair_macos_wheel "$wheel_path"

echo "built psycopg2-pglite wheel in $WHEEL_DIR"
