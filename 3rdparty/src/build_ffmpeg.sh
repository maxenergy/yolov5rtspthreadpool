#!/bin/bash

# FFmpeg Android Build Script
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
FFMPEG_SRC_DIR=$(pwd)/ffmpeg
PREFIX_DIR=$(pwd)/../target/ffmpeg

echo "Building FFmpeg for Android arm64-v8a..."
echo "Source: $FFMPEG_SRC_DIR"
echo "Install: $PREFIX_DIR"

cd $FFMPEG_SRC_DIR

# Configure FFmpeg
./configure \
    --prefix=$PREFIX_DIR \
    --enable-cross-compile \
    --target-os=android \
    --arch=$ARCH \
    --cpu=armv8-a \
    --cc=$CC \
    --cxx=$CXX \
    --ar=$AR \
    --strip=$STRIP \
    --ranlib=$RANLIB \
    --enable-shared \
    --disable-static \
    --disable-doc \
    --disable-programs \
    --disable-avdevice \
    --disable-avfilter \
    --enable-avformat \
    --enable-avcodec \
    --enable-swresample \
    --enable-swscale \
    --enable-protocol=file \
    --enable-protocol=rtsp \
    --enable-protocol=tcp \
    --enable-protocol=udp \
    --enable-demuxer=rtsp \
    --enable-demuxer=h264 \
    --enable-decoder=h264 \
    --enable-parser=h264

# Build and install
make clean
make -j$(nproc)
make install

echo "FFmpeg build completed successfully!"
echo "Libraries installed in: $PREFIX_DIR"
