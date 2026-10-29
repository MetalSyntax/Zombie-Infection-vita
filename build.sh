#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="/tmp/zi-build"
SRC_LINK="/tmp/zi-src"

# Detección de VITASDK (compatible con macOS y Linux)
if [ -z "$VITASDK" ]; then
  if [ -d "$HOME/vitasdk" ]; then
    export VITASDK="$HOME/vitasdk"
  elif [ -d "/usr/local/vitasdk" ]; then
    export VITASDK="/usr/local/vitasdk"
  elif [ -d "/home/metalsyntax/vitasdk" ]; then
    export VITASDK="/home/metalsyntax/vitasdk"
  elif [ -d "/Users/metalsyntax/vitasdk" ]; then
    export VITASDK="/Users/metalsyntax/vitasdk"
  fi
fi

if [ ! -d "$VITASDK" ]; then
  echo "[-] ERROR: VITASDK no encontrado en '$VITASDK'."
  echo "    Por favor exporta VITASDK antes de ejecutar este script:"
  echo "    export VITASDK=\"\$HOME/vitasdk\""
  exit 1
fi

export PATH="$VITASDK/bin:$PATH"

# Tipo de compilación (Debug o Release)
BUILD_TYPE="Release"
if [ "$1" = "debug" ] || [ "$1" = "Debug" ] || [ "$1" = "DEBUG" ]; then
  BUILD_TYPE="Debug"
fi

# Detección de núcleos para compilación en paralelo (macOS vs Linux)
if command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
  JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
else
  JOBS=4
fi

echo "===> VITASDK: $VITASDK"
echo "===> Build Type: $BUILD_TYPE"
echo "===> Jobs: $JOBS"

ln -sfn "$PROJECT_DIR" "$SRC_LINK"
cd "$PROJECT_DIR/vendor/vitaGL" && make clean
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$SRC_LINK" \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

make -j"$JOBS"

mkdir -p "$PROJECT_DIR/build"
cp -f "$BUILD_DIR/zombieinfection.vpk" "$PROJECT_DIR/build/"
cp -f "$BUILD_DIR/eboot.bin" "$PROJECT_DIR/build/"
cp -f "$BUILD_DIR/zombieinfection.velf" "$PROJECT_DIR/build/"
cp -f "$BUILD_DIR/zombieinfection" "$PROJECT_DIR/build/"

echo "===> Build completed successfully: build/zombieinfection.vpk"
