#!/bin/bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$SCRIPT_DIR"

INSTALL_FOLDER=${INSTALL_FOLDER:-"$SCRIPT_DIR/pglite/out/native"}
MAKE_BIN=""
if [ "$(uname -s)" = "Darwin" ] && command -v brew >/dev/null 2>&1; then
  BREW_MAKE_PREFIX=$(brew --prefix make 2>/dev/null || true)
  if [ -n "$BREW_MAKE_PREFIX" ] && [ -x "$BREW_MAKE_PREFIX/bin/gmake" ]; then
    MAKE_BIN="$BREW_MAKE_PREFIX/bin/gmake"
  fi
fi
if [ -z "$MAKE_BIN" ] && command -v gmake >/dev/null 2>&1; then
  MAKE_BIN=$(command -v gmake)
elif [ -n "${MAKE:-}" ]; then
  MAKE_BIN="$MAKE"
else
  MAKE_BIN=$(command -v make)
fi
export MAKE="$MAKE_BIN"

detect_cpu_count() {
  if command -v sysctl >/dev/null 2>&1; then
    sysctl -n hw.ncpu 2>/dev/null && return
  fi
  if command -v getconf >/dev/null 2>&1; then
    getconf _NPROCESSORS_ONLN 2>/dev/null && return
  fi
  echo 4
}

compute_parallel_jobs() {
  local detected requested max_jobs
  detected=$(detect_cpu_count)
  requested=${PGLITE_BUILD_JOBS:-$detected}
  if [ "$(uname -s)" = "Darwin" ]; then
    max_jobs=2
  else
    max_jobs=8
  fi
  if [ "$requested" -gt "$max_jobs" ]; then
    requested=$max_jobs
  fi
  if [ "$requested" -lt 1 ]; then
    requested=1
  fi
  echo "$requested"
}

MAKE_JOBS=$(compute_parallel_jobs)
MAKE_JOB_ARGS=(-j "$MAKE_JOBS")

PGLITE_NATIVE_CFLAGS=${PGLITE_NATIVE_CFLAGS:-"-fPIC -D__PGLITE__ -Dexit=pgl_exit -Dfcntl=pgl_fcntl -Datexit=pgl_atexit -Dsetsockopt=pgl_setsockopt -Dgetsockopt=pgl_getsockopt -Dgetsockname=pgl_getsockname -Drecv=pgl_recv -Dsend=pgl_send -Dconnect=pgl_connect -Dpoll=pgl_poll -Dshmget=pgl_shmget -Dshmat=pgl_shmat -Dshmdt=pgl_shmdt -Dshmctl=pgl_shmctl -Dlongjmp=pgl_longjmp -Dsiglongjmp=pgl_siglongjmp"}

CONFIGURE_PARAMS=(
  --without-llvm
  --without-pam
  --without-readline
  --without-zlib
  --without-icu
  --with-openssl=no
  --prefix="$INSTALL_FOLDER"
)

if [ ! -f config.status ] || grep -q -- "-D__PGLITE__" src/Makefile.global 2>/dev/null; then
  if [ -f config.status ]; then
    "$MAKE_BIN" distclean >/dev/null 2>&1 || true
  fi
  ./configure "${CONFIGURE_PARAMS[@]}"
fi

BASE_CFLAGS=$(sed -n 's/^CFLAGS = //p' src/Makefile.global | head -n 1)
BUILD_CFLAGS="$BASE_CFLAGS $PGLITE_NATIVE_CFLAGS"
PG_MAJOR_VERSION=$(sed -n 's/^MAJORVERSION = //p' src/Makefile.global | head -n 1)
RUNTIME_RPATHDIR=""

if [ "$(uname -s)" != "Darwin" ]; then
  RUNTIME_RPATHDIR='$$ORIGIN'
fi

clean_runtime_dirs() {
  "$MAKE_BIN" -C src/backend clean
  "$MAKE_BIN" -C src/common clean
  "$MAKE_BIN" -C src/include clean
  "$MAKE_BIN" -C src/interfaces/libpq clean
  "$MAKE_BIN" -C src/port clean
  "$MAKE_BIN" -C src/timezone clean
}

generate_libpglite_exports() {
  local exports_path="$(pwd)/pglite/native/libpglite.generated.exports.txt"
  local symbol_list="$exports_path.symbols"
  local postgres_symbol_source="$INSTALL_FOLDER/bin/postgres"

  if [ "$(uname -s)" = "Darwin" ]; then
    nm -gU "$postgres_symbol_source" | awk '/ [A-Z] / { name=$3; sub(/^_/, "", name); print name }'
  else
    nm -g --defined-only "$postgres_symbol_source" | awk '$2 ~ /^[A-Z]$/ { print $3 }'
  fi | awk '
    {
      sub(/@.*/, "", $0)
    }
    NF && $0 !~ /^_?mh_/ && $0 != "main" && !seen[$0]++
  ' > "$symbol_list"

  cat "$symbol_list" "$(pwd)/pglite/native/libpglite.exports.txt" \
    | awk 'NF && !seen[$0]++' > "$exports_path"
  rm -f "$symbol_list"

  echo "$exports_path"
}

build_vector_extension() {
  local vector_dir="$(pwd)/pglite/other_extensions/vector"
  local pg_config_path="$(pwd)/src/bin/pg_config/pg_config"
  local pgxs_path="$(pwd)/src/makefiles/pgxs.mk"
  local vector_link_flags=""
  local extension_binary=""

  if [ -f "$INSTALL_FOLDER/lib/libpglite.0.dylib" ]; then
    vector_link_flags="-L$INSTALL_FOLDER/lib -lpglite"
  elif [ -f "$INSTALL_FOLDER/lib/libpglite.so.0.1" ]; then
    vector_link_flags="-L$INSTALL_FOLDER/lib -lpglite"
  fi

  "$MAKE_BIN" -C "$vector_dir" clean >/dev/null 2>&1 || true
  "$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C "$vector_dir" \
    PG_CONFIG="$pg_config_path" \
    PGXS="$pgxs_path" \
    bindir="$INSTALL_FOLDER/bin" \
    includedir_server="$(pwd)/src/include" \
    includedir_internal="$(pwd)/src/include" \
    libdir="$INSTALL_FOLDER/lib" \
    rpathdir="$RUNTIME_RPATHDIR" \
    SHLIB_LINK="$vector_link_flags"

  cp "$vector_dir/vector.control" "$INSTALL_FOLDER/share/extension/"
  cp "$vector_dir"/sql/vector--*.sql "$INSTALL_FOLDER/share/extension/"

  if [ -f "$vector_dir/vector.dylib" ]; then
    extension_binary="$vector_dir/vector.dylib"
  elif [ -f "$vector_dir/vector.so" ]; then
    extension_binary="$vector_dir/vector.so"
  fi

  if [ -z "$extension_binary" ]; then
    echo "failed to build vector extension binary" >&2
    exit 1
  fi

  cp "$extension_binary" "$INSTALL_FOLDER/lib/"
  "$MAKE_BIN" -C "$vector_dir" clean >/dev/null 2>&1 || true
}

rebuild_embedded_backend_modules() {
  local embedded_link_flags=""

  if [ "$(uname -s)" != "Darwin" ]; then
    return
  fi

  if [ -f "$INSTALL_FOLDER/lib/libpglite.0.dylib" ]; then
    embedded_link_flags="-L$INSTALL_FOLDER/lib -lpglite"
  else
    return
  fi

  "$MAKE_BIN" -C src/backend/snowball clean >/dev/null 2>&1 || true
  "$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/backend/snowball \
    BE_DLLLIBS="$embedded_link_flags" \
    all

  "$MAKE_BIN" -C src/pl/plpgsql/src clean >/dev/null 2>&1 || true
  "$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/pl/plpgsql/src \
    BE_DLLLIBS="$embedded_link_flags" \
    all

  cp "src/backend/snowball/dict_snowball.dylib" "$INSTALL_FOLDER/lib/"
  cp "src/pl/plpgsql/src/plpgsql.dylib" "$INSTALL_FOLDER/lib/"
}

sign_macos_runtime_artifacts() {
  if [ "$(uname -s)" != "Darwin" ] || ! command -v codesign >/dev/null 2>&1; then
    return
  fi

  while IFS= read -r artifact; do
    if file "$artifact" | grep -q "Mach-O"; then
      # Replace linker signatures with an explicit ad-hoc signature so the
      # bundle can be loaded by normal host processes via dlopen().
      codesign --force -s - "$artifact" >/dev/null
    fi
  done < <(find "$INSTALL_FOLDER/bin" "$INSTALL_FOLDER/lib" -type f)
}

normalize_macos_runtime_artifacts() {
  if [ "$(uname -s)" != "Darwin" ] || ! command -v install_name_tool >/dev/null 2>&1; then
    return
  fi

  local libpq_path="$INSTALL_FOLDER/lib/libpq.5.dylib"
  local libpglite_path="$INSTALL_FOLDER/lib/libpglite.0.dylib"
  local libpqpglite_path="$INSTALL_FOLDER/lib/libpqpglite.5.dylib"

  if [ -f "$libpq_path" ]; then
    install_name_tool -id "@loader_path/libpq.5.dylib" "$libpq_path"
  fi

  if [ -f "$libpglite_path" ]; then
    install_name_tool -id "@loader_path/libpglite.0.dylib" "$libpglite_path"
    if [ -f "$libpq_path" ]; then
      install_name_tool \
        -change "$libpq_path" "@loader_path/libpq.5.dylib" \
        "$libpglite_path"
    fi
  fi

  if [ -f "$libpqpglite_path" ]; then
    install_name_tool -id "@loader_path/libpqpglite.5.dylib" "$libpqpglite_path"
    if [ -f "$libpglite_path" ]; then
      install_name_tool \
        -change "$libpglite_path" "@loader_path/libpglite.0.dylib" \
        "$libpqpglite_path"
    fi
  fi
}

clean_runtime_dirs

"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/backend generated-headers
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/backend/utils fmgr-stamp errcodes.h
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/interfaces/libpq all
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/bin/pg_config pg_config
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/backend postgres
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/backend/snowball snowball_create.sql
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/pl/plpgsql/src all
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/common all
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/port all
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/timezone all
"$MAKE_BIN" -C src/timezone datadir="$INSTALL_FOLDER/share" install

mkdir -p "$INSTALL_FOLDER/bin" "$INSTALL_FOLDER/lib" "$INSTALL_FOLDER/share" "$INSTALL_FOLDER/share/extension" "$INSTALL_FOLDER/share/tsearch_data"
rm -f "$INSTALL_FOLDER/bin/initdb"
rm -rf "$INSTALL_FOLDER/share/pglite-template"
cp "src/backend/postgres" "$INSTALL_FOLDER/bin/"
cp "src/include/catalog/postgres.bki" "$INSTALL_FOLDER/share/"
cp "src/backend/libpq/pg_hba.conf.sample" "$INSTALL_FOLDER/share/"
cp "src/backend/libpq/pg_ident.conf.sample" "$INSTALL_FOLDER/share/"
cp "src/backend/utils/misc/postgresql.conf.sample" "$INSTALL_FOLDER/share/"
cp "src/backend/snowball/snowball_create.sql" "$INSTALL_FOLDER/share/"
cp src/backend/snowball/stopwords/* "$INSTALL_FOLDER/share/tsearch_data/"
cp "src/backend/catalog/information_schema.sql" "$INSTALL_FOLDER/share/"
cp "src/backend/catalog/sql_features.txt" "$INSTALL_FOLDER/share/"
cp "src/include/catalog/system_constraints.sql" "$INSTALL_FOLDER/share/"
cp "src/backend/catalog/system_functions.sql" "$INSTALL_FOLDER/share/"
cp "src/backend/catalog/system_views.sql" "$INSTALL_FOLDER/share/"
cp "src/pl/plpgsql/src/plpgsql.control" "$INSTALL_FOLDER/share/extension/"
cp "src/pl/plpgsql/src/plpgsql--1.0.sql" "$INSTALL_FOLDER/share/extension/"
if [ -f "src/interfaces/libpq/libpq.5.dylib" ]; then
  cp "src/interfaces/libpq/libpq.5.dylib" "$INSTALL_FOLDER/lib/"
  ln -sf "libpq.5.dylib" "$INSTALL_FOLDER/lib/libpq.dylib"
fi
if [ -f "src/interfaces/libpq/libpq.so.5" ]; then
  cp "src/interfaces/libpq/libpq.so.5" "$INSTALL_FOLDER/lib/"
  ln -sf "libpq.so.5" "$INSTALL_FOLDER/lib/libpq.so"
fi
cp "src/backend/snowball/dict_snowball.dylib" "$INSTALL_FOLDER/lib/" 2>/dev/null || true
cp "src/backend/snowball/dict_snowball.so" "$INSTALL_FOLDER/lib/" 2>/dev/null || true
cp "src/pl/plpgsql/src/plpgsql.dylib" "$INSTALL_FOLDER/lib/" 2>/dev/null || true
cp "src/pl/plpgsql/src/plpgsql.so" "$INSTALL_FOLDER/lib/" 2>/dev/null || true
sign_macos_runtime_artifacts

clean_runtime_dirs

PGLITE_GENERATED_EXPORTS="$(generate_libpglite_exports)"

"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/backend CFLAGS="$BUILD_CFLAGS" generated-headers
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/backend/utils CFLAGS="$BUILD_CFLAGS" fmgr-stamp errcodes.h
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/interfaces/libpq all
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/fe_utils all
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/common logging.o
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/common CFLAGS="$BUILD_CFLAGS" libpgcommon_srv.a
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/port CFLAGS="$BUILD_CFLAGS" libpgport_srv.a
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/timezone CFLAGS="$BUILD_CFLAGS" objfiles.txt
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C src/backend CFLAGS="$BUILD_CFLAGS" objfiles.txt
"$MAKE_BIN" -C pglite/native clean
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C pglite/native \
  PGLITE_BUILD_CFLAGS="$BUILD_CFLAGS" \
  PGLITE_EXPORTS="$PGLITE_GENERATED_EXPORTS" \
  rpathdir="$RUNTIME_RPATHDIR" \
  all
"$MAKE_BIN" "${MAKE_JOB_ARGS[@]}" -C pglite/libpq-pglite all
rm -f "$PGLITE_GENERATED_EXPORTS"

cp "pglite/native/libpglite.0.dylib" "$INSTALL_FOLDER/lib/" 2>/dev/null || true
if [ -f "$INSTALL_FOLDER/lib/libpglite.0.dylib" ]; then
  ln -sf "libpglite.0.dylib" "$INSTALL_FOLDER/lib/libpglite.dylib"
fi
cp "pglite/native/libpglite.so.0.1" "$INSTALL_FOLDER/lib/" 2>/dev/null || true
if [ -f "$INSTALL_FOLDER/lib/libpglite.so.0.1" ]; then
  ln -sf "libpglite.so.0.1" "$INSTALL_FOLDER/lib/libpglite.so.0"
  ln -sf "libpglite.so.0.1" "$INSTALL_FOLDER/lib/libpglite.so"
fi
cp "pglite/libpq-pglite/libpqpglite.5.dylib" "$INSTALL_FOLDER/lib/" 2>/dev/null || true
if [ -f "$INSTALL_FOLDER/lib/libpqpglite.5.dylib" ]; then
  ln -sf "libpqpglite.5.dylib" "$INSTALL_FOLDER/lib/libpqpglite.dylib"
fi
cp "pglite/libpq-pglite/libpqpglite.so.5.$PG_MAJOR_VERSION" "$INSTALL_FOLDER/lib/" 2>/dev/null || true
if [ -f "$INSTALL_FOLDER/lib/libpqpglite.so.5.$PG_MAJOR_VERSION" ]; then
  ln -sf "libpqpglite.so.5.$PG_MAJOR_VERSION" "$INSTALL_FOLDER/lib/libpqpglite.so.5"
  ln -sf "libpqpglite.so.5.$PG_MAJOR_VERSION" "$INSTALL_FOLDER/lib/libpqpglite.so"
fi

normalize_macos_runtime_artifacts
rebuild_embedded_backend_modules
build_vector_extension
sign_macos_runtime_artifacts

echo "native artifacts were copied to $INSTALL_FOLDER"
