#include <jni.h>
#include <string>
#include <memory>
#include <android/native_window_jni.h> // ANativeWindow 用来渲染画面的 == Surface对象
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <unistd.h>
#include "log4c.h"
#include "ZLPlayer.h"
#include "ChannelManager.h"
#include <jni.h>

JavaVM *vm = nullptr;
ANativeWindow *window = nullptr;
pthread_mutex_t windowMutex = PTHREAD_COND_INITIALIZER;
AAssetManager *nativeAssetManager;

jint JNI_OnLoad(JavaVM *vm, void *args) {
    ::vm = vm;
    return JNI_VERSION_1_6;
}

void JNI_OnUnload(JavaVM *vm, void *args) {
    ::vm = nullptr;
}

void get_file_content(char *data, int *dataLen, const char *fileName) {

    // Initialize output parameters
    *dataLen = 0;

    // 获取文件内容
    if (nativeAssetManager == nullptr) {
        LOGE("AAssetManager is null");
        return;
    }

    LOGD("Opening fileName: %s", fileName);

    //打开指定文件
    AAsset *asset = AAssetManager_open(nativeAssetManager, fileName, AASSET_MODE_BUFFER);
    if (asset == nullptr) {
        LOGE("Failed to open asset file: %s", fileName);
        return;
    }

    //获取文件长度
    off_t fileSize = AAsset_getLength(asset);
    if (fileSize <= 0) {
        LOGE("Invalid file size for %s: %ld", fileName, fileSize);
        AAsset_close(asset);
        return;
    }

    LOGD("File size: %ld bytes", fileSize);

    char *buf = new char[fileSize];
    if (!buf) {
        LOGE("Failed to allocate buffer for file %s", fileName);
        AAsset_close(asset);
        return;
    }

    memset(buf, 0x00, fileSize);

    //读取文件内容
    int bytesRead = AAsset_read(asset, buf, fileSize);
    if (bytesRead != fileSize) {
        LOGE("Failed to read complete file %s: read %d, expected %ld", fileName, bytesRead, fileSize);
        delete[] buf;
        AAsset_close(asset);
        return;
    }

    memcpy(data, buf, fileSize);
    *dataLen = fileSize;

    // 释放内存
    delete[] buf;
    AAsset_close(asset);

    LOGD("Successfully loaded file %s: %d bytes", fileName, *dataLen);
}

extern "C"
JNIEXPORT jlong
JNICALL
Java_com_wulala_myyolov5rtspthreadpool_MainActivity_prepareNative(JNIEnv *env, jobject thiz) {

    LOGD("prepareNative: Starting native player initialization");

    // Check if asset manager is available
    if (nativeAssetManager == nullptr) {
        LOGE("prepareNative: AAssetManager is null, cannot load model");
        return 0;
    }

    char *data = nullptr;
    ZLPlayer *zlPlayer = nullptr;

    try {
        // Allocate memory for model data
        data = static_cast<char *>(malloc(1024 * 1024 * 50));
        if (!data) {
            LOGE("prepareNative: Failed to allocate memory for model data");
            return 0;
        }

        int dataLen = 0;
        // Try to load yolov5s_quant.rknn first, fallback to yolov5s.rknn
        get_file_content(data, &dataLen, "yolov5s_quant.rknn");

        if (dataLen <= 0) {
            LOGW("prepareNative: Failed to load yolov5s_quant.rknn, trying yolov5s.rknn");
            get_file_content(data, &dataLen, "yolov5s.rknn");
        }

        if (dataLen <= 0) {
            LOGE("prepareNative: Failed to load any model file");
            free(data);
            return 0;
        }

        LOGD("prepareNative: Model loaded successfully, size: %d bytes", dataLen);

        // Create ZLPlayer with error handling
        zlPlayer = new ZLPlayer(data, dataLen);

        if (!zlPlayer) {
            LOGE("prepareNative: Failed to create ZLPlayer instance");
            free(data);
            return 0;
        }

        LOGD("prepareNative: ZLPlayer created successfully");

        // Free the temporary data buffer (ZLPlayer has its own copy)
        free(data);

        return reinterpret_cast<jlong>(zlPlayer);

    } catch (const std::exception& e) {
        LOGE("prepareNative: Exception during initialization: %s", e.what());
        if (data) {
            free(data);
        }
        if (zlPlayer) {
            delete zlPlayer;
        }
        return 0;
    } catch (...) {
        LOGE("prepareNative: Unknown exception during initialization");
        if (data) {
            free(data);
        }
        if (zlPlayer) {
            delete zlPlayer;
        }
        return 0;
    }
}


extern "C"
JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_MainActivity_setNativeAssetManager(
        JNIEnv *env, jobject instance,
        jobject assetManager) {

    nativeAssetManager = AAssetManager_fromJava(env, assetManager);
    if (nativeAssetManager == nullptr) {
        LOGE("AAssetManager == null");
    }

    LOGD("AAssetManager been set");
}

extern "C"
JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_MainActivity_setNativeSurface(
        JNIEnv *env, jobject instance, jobject surface) {

    pthread_mutex_lock(&windowMutex);

    // Release previous window if exists
    if (window) {
        ANativeWindow_release(window);
        window = nullptr;
    }

    // Set new window from surface
    if (surface) {
        window = ANativeWindow_fromSurface(env, surface);
        if (window) {
            LOGD("ANativeWindow set successfully");
        } else {
            LOGE("Failed to create ANativeWindow from surface");
        }
    } else {
        LOGD("Surface is null, clearing ANativeWindow");
    }

    pthread_mutex_unlock(&windowMutex);
}

// Multi-channel support methods for MainActivity
extern "C"
JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_MainActivity_setMultiChannelMode(
        JNIEnv *env, jobject instance, jint channelCount) {

    LOGD("Setting multi-channel mode with %d channels", channelCount);

    // Initialize global channel manager if not already done
    if (!g_channelManager) {
        g_channelManager.reset(new NativeChannelManager());
        LOGD("Global channel manager created");
    }

    // The channel count is mainly used for UI layout
    // The actual channel management is handled by ChannelManager
    LOGD("Multi-channel mode set to %d channels", channelCount);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_MainActivity_setChannelRTSPUrl(
        JNIEnv *env, jobject instance, jint channelIndex, jstring rtspUrl) {

    if (!rtspUrl) {
        LOGE("RTSP URL is null for channel %d", channelIndex);
        return;
    }

    const char* urlStr = env->GetStringUTFChars(rtspUrl, nullptr);
    if (!urlStr) {
        LOGE("Failed to get RTSP URL string for channel %d", channelIndex);
        return;
    }

    LOGD("Setting RTSP URL for channel %d: %s", channelIndex, urlStr);

    // Use global channel manager if available
    if (g_channelManager) {
        g_channelManager->setChannelRTSPUrl(channelIndex, urlStr);
    } else {
        LOGW("Channel manager not initialized, cannot set RTSP URL for channel %d", channelIndex);
    }

    env->ReleaseStringUTFChars(rtspUrl, urlStr);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_MainActivity_setNativeActiveChannel(
        JNIEnv *env, jobject instance, jint channelIndex) {

    LOGD("Setting active channel to %d", channelIndex);

    // This method is used to focus on a specific channel
    // It could be used to prioritize rendering or processing for the selected channel

    if (g_channelManager) {
        // For now, we'll just log the active channel
        // In a full implementation, this could affect rendering priority or UI focus
        LOGD("Active channel set to %d", channelIndex);
    } else {
        LOGW("Channel manager not initialized, cannot set active channel");
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_com_wulala_myyolov5rtspthreadpool_MainActivity_setNativeChannelSurface(
        JNIEnv *env, jobject instance, jint channelIndex, jobject surface) {

    LOGD("Setting surface for channel %d", channelIndex);

    if (!g_channelManager) {
        LOGW("Channel manager not initialized, cannot set surface for channel %d", channelIndex);
        return;
    }

    // Get ANativeWindow from Surface
    ANativeWindow* window = nullptr;
    if (surface) {
        window = ANativeWindow_fromSurface(env, surface);
        if (window) {
            LOGD("ANativeWindow created successfully for channel %d", channelIndex);
        } else {
            LOGE("Failed to create ANativeWindow from surface for channel %d", channelIndex);
            return;
        }
    } else {
        LOGD("Surface is null, clearing surface for channel %d", channelIndex);
    }

    // Set the surface for the specific channel
    g_channelManager->setChannelSurface(channelIndex, window);
}
