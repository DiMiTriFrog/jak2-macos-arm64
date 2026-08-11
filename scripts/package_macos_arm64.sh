#!/usr/bin/env bash
set -euo pipefail

# Build and package only the native Apple ARM64 application.  Game data is
# deliberately not copied: the user supplies legally dumped assets through
# --proj-path or the normal project data directory.

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${project_root}/build-arm64-release}"
output_app="${2:-${project_root}/dist/OpenGOAL Jak II.app}"
source_app="${build_dir}/game/gk.app"
source_executable="${source_app}/Contents/MacOS/gk"
frameworks_dir="${output_app}/Contents/Frameworks"
output_executable="${output_app}/Contents/MacOS/gk"
codesign_identity="${CODESIGN_IDENTITY:-}"

if [[ "$(uname -m)" != "arm64" ]]; then
  echo "error: packaging must run on an arm64 host; Rosetta is not accepted" >&2
  exit 2
fi
if [[ ! -x "${source_executable}" ]]; then
  echo "error: missing ARM64 build: ${source_executable}" >&2
  exit 2
fi
if [[ -z "${codesign_identity}" || "${codesign_identity}" == "-" ]]; then
  echo "error: CODESIGN_IDENTITY must name one Apple signing identity; ad-hoc signing is incompatible with hardened runtime and bundled dylibs" >&2
  exit 5
fi

rm -rf "${output_app}"
mkdir -p "$(dirname "${output_app}")"
cp -R "${source_app}" "${output_app}"
mkdir -p "${frameworks_dir}"

declare -a search_roots=(
  "${build_dir}"
  "/opt/homebrew/opt/openssl@3/lib"
  "/opt/homebrew/opt/libnghttp2/lib"
)

find_dependency() {
  local dependency="$1"
  local name="${dependency##*/}"
  local candidate

  if [[ "${dependency}" == /* && -f "${dependency}" ]]; then
    printf '%s\n' "${dependency}"
    return 0
  fi

  for root in "${search_roots[@]}"; do
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
    if [[ "$(file -b "${dependency_source}")" != *"arm64"* ]]; then
      echo "error: dependency is not ARM64: ${dependency_source}" >&2
      exit 4
    fi

    bundled_dependency="${frameworks_dir}/${dependency_name}"
    cp -f "${dependency_source}" "${bundled_dependency}"
    chmod u+w "${bundled_dependency}"
    install_name_tool -id "@rpath/${dependency_name}" "${bundled_dependency}"
    install_name_tool -change "${dependency}" "@rpath/${dependency_name}" "${source_binary}"
    copied_dependencies="${copied_dependencies}${dependency_name};"
    bundle_binary "${bundled_dependency}"
  done < <(otool -L "${source_binary}" | tail -n +2 | sed -E 's/^[[:space:]]+([^[:space:]]+).*/\1/')
}

bundle_binary "${output_executable}"

while IFS= read -r nested_binary; do
  codesign --force --options runtime --timestamp=none \
    --sign "${codesign_identity}" "${nested_binary}"
done < <(find "${frameworks_dir}" -type f -name '*.dylib' -print | sort)

# The main executable is signed twice intentionally: signing the enclosing app
# also signs its main code object, so the final app signing must carry the JIT
# entitlement or MAP_JIT will fail under Hardened Runtime.
codesign --force --options runtime --timestamp=none \
  --sign "${codesign_identity}" \
  --entitlements "${project_root}/game/macos/jak-jit.entitlements" \
  "${output_executable}"
codesign --force --options runtime --timestamp=none \
  --sign "${codesign_identity}" \
  --entitlements "${project_root}/game/macos/jak-jit.entitlements" \
  "${output_app}"
codesign --verify --deep --strict --verbose=2 "${output_app}"

echo "Packaged native ARM64 app: ${output_app}"
