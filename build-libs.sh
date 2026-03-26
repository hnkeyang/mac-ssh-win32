#!/bin/bash
# Build script for mbedtls and libssh for Windows cross-compilation

set -e

SCRIPT_DIR=$(dirname "$0")
cd "$SCRIPT_DIR"

MINGW_PREFIX=i686-w64-mingw32
BUILD_DIR="$SCRIPT_DIR/build"

echo "=== Building mbedtls ==="
# git clone https://github.com/Mbed-TLS/mbedtls.git
# git submodule update --init
# cp -a framework tf-psa-crypto/
mkdir -p mbedtls/build
cd mbedtls/build
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../../mingw-toolchain.cmake \
    -DENABLE_PROGRAMS=OFF \
    -DENABLE_TESTING=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
    -DCMAKE_INSTALL_PREFIX="$BUILD_DIR/mbedtls" \
    -G "Unix Makefiles"
make -j$(nproc)
make install

echo "=== Building libssh ==="
# wget https://www.libssh.org/files/0.12/libssh-0.12.0.tar.xz
cd "$SCRIPT_DIR/libssh-0.12.0"
patch -p1 < ../libssh-mingw.patch
mkdir -p build
cd build
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../../mingw-toolchain.cmake \
    -DWITH_MBEDTLS=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DWITH_EXAMPLES=OFF \
    -DWITH_TESTING=OFF \
    -DWITH_SERVER=OFF \
    -DWITH_ZLIB=OFF \
    -DCMAKE_INSTALL_PREFIX=$BUILD_DIR/libssh \
    -DMBEDTLS_INCLUDE_DIR=$BUILD_DIR/mbedtls/include \
    -DMBEDTLS_SSL_LIBRARY=$BUILD_DIR/mbedtls/lib/libmbedtls.a \
    -DMBEDTLS_CRYPTO_LIBRARY=$BUILD_DIR/mbedtls/lib/libmbedcrypto.a \
    -DMBEDTLS_X509_LIBRARY=$BUILD_DIR/mbedtls/lib/libmbedx509.a \
    -G "Unix Makefiles"
make -j$(nproc)
make install

echo "=== Build complete ==="
echo "mbedtls: $BUILD_DIR/mbedtls"
echo "libssh: $BUILD_DIR/libssh"
