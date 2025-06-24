#!/bin/bash

# ZLMediaKit Android Build Script
set -e

# Android NDK and SDK paths
export ANDROID_NDK_HOME=/home/rogers/Android/Sdk/ndk/25.1.8937393
export ANDROID_SDK_HOME=/home/rogers/Android/Sdk

# Build configuration
export API_LEVEL=31
export ARCH=arm64
export TARGET=aarch64-linux-android
export CC=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/${TARGET}${API_LEVEL}-clang
export CXX=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/${TARGET}${API_LEVEL}-clang++
export AR=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar
export STRIP=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
export RANLIB=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ranlib

# Paths
ZLMEDIAKIT_SRC_DIR=$(pwd)/zlmediakit
PREFIX_DIR=$(pwd)/../target/zlmediakit
BUILD_DIR=${ZLMEDIAKIT_SRC_DIR}/build_android

echo "Building ZLMediaKit for Android arm64-v8a..."
echo "Source: $ZLMEDIAKIT_SRC_DIR"
echo "Install: $PREFIX_DIR"

# Create build directory
mkdir -p $BUILD_DIR
cd $BUILD_DIR

# Configure with CMake
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-${API_LEVEL} \
    -DANDROID_NDK=$ANDROID_NDK_HOME \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=$PREFIX_DIR \
    -DENABLE_TESTS=OFF \
    -DENABLE_SERVER=OFF \
    -DENABLE_PLAYER=OFF \
    -DENABLE_PUSHER=OFF \
    -DENABLE_API=ON \
    -DENABLE_RTPPROXY=ON \
    -DENABLE_WEBRTC=OFF \
    -DENABLE_SRT=OFF \
    -DENABLE_FFMPEG=OFF \
    -DENABLE_HLS=OFF \
    -DENABLE_MP4=OFF

# Build and install
make -j$(nproc)
make install

echo "ZLMediaKit build completed successfully!"
echo "Libraries installed in: $PREFIX_DIR"
