#!/usr/bin/env bash
set -eu

CUR_DIR="${PWD}"
cd "$(dirname "${BASH_SOURCE:-$0}")"

INSTALL_TOOLS=1
REBUILD=0
SKIP_TESTS=0
CREATE_ZIP=0
CMAKE_BUILD_TYPE=Release

while [[ $# -gt 0 ]]; do
  case $1 in
    --no-install-tools)
      INSTALL_TOOLS=0
      shift
      ;;
    -d | --debug)
      CMAKE_BUILD_TYPE=Debug
      shift
      ;;
    -s | --skip-tests)
      SKIP_TESTS=1
      shift
      ;;
    -r | --rebuild)
      REBUILD=1
      shift
      ;;
    -z | --zip)
      CREATE_ZIP=1
      shift
      ;;
    -* | --*)
      echo "Unknown option $1"
      exit 1
      ;;
    *)
      shift
      ;;
  esac
done

# ovbase's setup script is downloaded to the project root and fetches the toolchain
# into ./build/tools.  Sourcing it puts cmake / ninja / llvm-mingw / gettext into this
# shell's PATH, and it writes ./env-<platform>.sh next to itself.
if [ "${INSTALL_TOOLS}" -eq 1 ]; then
  mkdir -p "build/tools"
  if [ ! -e "${PWD}/setup-llvm-mingw.sh" ]; then
    OVBASE_COMMIT_HASH=$(grep 'set(OVBASE_COMMIT_HASH' "${PWD}/src/CMakeLists.txt" | sed -E 's/.*"([^"]+)".*/\1/')
    curl -o "${PWD}/setup-llvm-mingw.sh" -sOL "https://raw.githubusercontent.com/oov/ovbase/${OVBASE_COMMIT_HASH}/setup-llvm-mingw.sh"
  fi
  . "${PWD}/setup-llvm-mingw.sh" --dir "${PWD}/build/tools"
fi

ARCHDIR="${ARCHDIR:-$(uname -m)}"
destdir="${PWD}/build/${CMAKE_BUILD_TYPE}/${ARCHDIR}"

if [ "${REBUILD}" -eq 1 ] || [ ! -e "${destdir}/CMakeCache.txt" ]; then
  rm -rf "${destdir}"
  cmake -S "${PWD}" -B "${destdir}" --preset default \
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
    -DCMAKE_TOOLCHAIN_FILE="${PWD}/cmake/llvm-mingw.cmake" \
    -DCMAKE_C_COMPILER="${CMAKE_C_COMPILER:-clang}" \
    -DCMAKE_C_COMPILER_TARGET="${CMAKE_C_COMPILER_TARGET:-x86_64-w64-mingw32}"
fi
# ovbase has no switch to disable its own test targets, so its tests are built and run
# together with ours.  Everything is built: the application, the tests, the diagnostic
# tools and the clang-format pass.  A narrow --target build would silently skip the rest.
cmake --build "${destdir}"
if [ "${SKIP_TESTS}" -eq 0 ]; then
  ctest --test-dir "${destdir}" --output-on-failure
fi

echo "built: ${destdir}/bin/audient-device-closer.exe"

if [ "${CREATE_ZIP}" -eq 1 ]; then
  distdir="${PWD}/build/${CMAKE_BUILD_TYPE}/dist"
  mkdir -p "${distdir}"
  zippath="${distdir}/audient-device-closer_${ADC_VERSION:-dev}.zip"
  rm -f "${zippath}"
  (cd "${destdir}/bin" && cmake -E tar cf "${zippath}" --format=zip .)
  echo "zip: ${zippath}"
fi

cd "${CUR_DIR}"
