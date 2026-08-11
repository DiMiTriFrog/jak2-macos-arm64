#!/usr/bin/env bash
set -euo pipefail

# Stage an unsigned native Apple ARM64 application. Game data is deliberately
# not copied: the user supplies legally dumped assets through the normal
# project data directory. Signing/notarization is a separate release step.

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${project_root}/build-arm64-release}"
output_app="${2:-${project_root}/dist/OpenGOAL Jak II.app}"
source_app="${build_dir}/game/gk.app"
source_executable="${source_app}/Contents/MacOS/gk"
frameworks_dir="${output_app}/Contents/Frameworks"
output_executable="${output_app}/Contents/MacOS/gk"

if [[ "$(uname -m)" != "arm64" ]]; then
  echo "error: packaging must run on an arm64 host; Rosetta is not accepted" >&2
  exit 2
fi
if [[ ! -x "${source_executable}" ]]; then
  echo "error: missing ARM64 build: ${source_executable}" >&2
  exit 2
fi
if [[ "$(lipo -archs "${source_executable}")" != "arm64" ]]; then
  echo "error: source executable is not a single-architecture arm64 Mach-O" >&2
  exit 4
fi
if [[ "${output_app}" != *.app ]]; then
  echo "error: output path must end in .app" >&2
  exit 2
fi

if [[ -e "${output_app}" ]]; then
  rm -rf "${output_app}"
fi
mkdir -p "$(dirname "${output_app}")"
cp -R "${source_app}" "${output_app}"
mkdir -p "${frameworks_dir}"

# Dependencies referenced by the build are first resolved from the build tree.
# Absolute Homebrew paths are accepted only when they exist on this machine;
# no developer-specific path is written into the final bundle.
declare -a search_roots=("${build_dir}")
if command -v brew >/dev/null 2>&1; then
  for formula in openssl@3 libnghttp2 zstd; do
    formula_prefix="$(brew --prefix "${formula}" 2>/dev/null || true)"
    if [[ -n "${formula_prefix}" && -d "${formula_prefix}/lib" ]]; then
      search_roots+=("${formula_prefix}/lib")
    fi
  done
fi

find_dependency() {
  local dependency="$1"
  local name="${dependency##*/}"
  local candidate

  if [[ "${dependency}" == /* && -f "${dependency}" ]]; then
    printf '%s\n' "${dependency}"
    return 0
  fi

  for root in "${search_roots[@]}"; do
    # SDL/fmt and other CMake targets commonly expose unversioned or
    # compatibility-name symlinks. Keep the requested install name, but copy
    # the symlink target's bytes into the bundle.
    candidate="$(find "${root}" -name "${name}" -print -quit 2>/dev/null || true)"
    if [[ -n "${candidate}" ]]; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done
  return 1
}

read_rpaths() {
  local binary="$1"
  otool -l "${binary}" | awk '
    /LC_RPATH/ { capture = 1; next }
    capture && /path / { sub(/^.*path /, ""); sub(/ \(offset.*$/, ""); print; capture = 0 }
  '
}

normalise_rpaths() {
  local binary="$1"
  local rpath
  while IFS= read -r rpath; do
    [[ -z "${rpath}" || "${rpath}" == "@loader_path/../Frameworks" ]] && continue
    install_name_tool -delete_rpath "${rpath}" "${binary}"
  done < <(read_rpaths "${binary}")

  if ! read_rpaths "${binary}" | grep -Fxq '@loader_path/../Frameworks'; then
    install_name_tool -add_rpath '@loader_path/../Frameworks' "${binary}"
  fi
}

remove_existing_signature() {
  # install_name_tool refuses signed binaries on some Xcode releases. The
  # output is intentionally unsigned, so discard any build-time signature
  # before changing load commands.
  codesign --remove-signature "$1" 2>/dev/null || true
}

copied_dependencies=""

already_copied() {
  local dependency_name="$1"
  case ";${copied_dependencies};" in
    *";${dependency_name};"*) return 0 ;;
    *) return 1 ;;
  esac
}

bundle_binary() {
  local source_binary="$1"
  local dependency dependency_name dependency_source bundled_dependency

  remove_existing_signature "${source_binary}"
  normalise_rpaths "${source_binary}"
  while IFS= read -r dependency; do
    [[ -z "${dependency}" ]] && continue
    case "${dependency}" in
      /System/*|/usr/lib/*|/usr/lib)
        continue
        ;;
    esac

    dependency_name="${dependency##*/}"
    if already_copied "${dependency_name}"; then
      install_name_tool -change "${dependency}" "@rpath/${dependency_name}" "${source_binary}"
      continue
    fi

    dependency_source="$(find_dependency "${dependency}" || true)"
    if [[ -z "${dependency_source}" ]]; then
      echo "error: cannot resolve non-system dependency ${dependency} of ${source_binary}" >&2
      exit 3
    fi
    if [[ "$(lipo -archs "${dependency_source}")" != "arm64" ]]; then
      echo "error: dependency is not a single-architecture ARM64 Mach-O: ${dependency_source}" >&2
      exit 4
    fi

    bundled_dependency="${frameworks_dir}/${dependency_name}"
    cp -L -f "${dependency_source}" "${bundled_dependency}"
    chmod u+w "${bundled_dependency}"
    remove_existing_signature "${bundled_dependency}"
    install_name_tool -id "@rpath/${dependency_name}" "${bundled_dependency}"
    install_name_tool -change "${dependency}" "@rpath/${dependency_name}" "${source_binary}"
    copied_dependencies="${copied_dependencies}${dependency_name};"
    bundle_binary "${bundled_dependency}"
  done < <(otool -L "${source_binary}" | tail -n +2 | sed -E 's/^[[:space:]]+([^[:space:]]+).*/\1/')
}

bundle_binary "${output_executable}"

verify_binary() {
  local binary="$1"
  if [[ "$(lipo -archs "${binary}")" != "arm64" ]]; then
    echo "error: packaged binary is not pure ARM64: ${binary}" >&2
    exit 4
  fi

  while IFS= read -r dependency; do
    [[ -z "${dependency}" ]] && continue
    case "${dependency}" in
      @*|/System/*|/usr/lib/*|/usr/lib)
        ;;
      *)
        echo "error: unresolved absolute dependency ${dependency} in ${binary}" >&2
        exit 3
        ;;
    esac
  done < <(otool -L "${binary}" | tail -n +2 | sed -E 's/^[[:space:]]+([^[:space:]]+).*/\1/')
}

verify_binary "${output_executable}"
while IFS= read -r bundled_binary; do
  verify_binary "${bundled_binary}"
done < <(find "${frameworks_dir}" -type f -name '*.dylib' -print | sort)

if otool -l "${output_executable}" | grep -Fq '/Users/'; then
  echo "error: packaged executable still contains a developer-local path" >&2
  exit 3
fi

echo "Staged unsigned native ARM64 app: ${output_app}"
echo "Signing and notarization are intentionally not performed by this script."
