#include <mk_common.h>
#include <mk_player.h>
#include <android/native_window_jni.h>
#include <thread>
#include <chrono>
#include "ZLPlayer.h"
#include "mpp_err.h"
#include "cv_draw.h"
// Yolov8ThreadPool *yolov8_thread_pool;   // 线程池

extern pthread_mutex_t windowMutex;     // 静态初始化 所
extern ANativeWindow *window;

void *rtps_process(void *arg) {
    ZLPlayer *player = (ZLPlayer *) arg;
    if (player) {
        player->process_video_rtsp();
    } else {
        LOGE("player is null");
    }
    return nullptr;
}

void *desplay_process(void *arg) {
    ZLPlayer *player = (ZLPlayer *) arg;
    if (player) {
        while (player->isStreaming) {
            try {
                player->display();
                // Add small delay to prevent excessive CPU usage
                std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
            } catch (...) {
                LOGE("Exception in display process for channel %d", player->channelIndex);
                break;
            }
        }
        LOGD("Display process exiting for channel %d", player->channelIndex);
    } else {
        LOGE("player is null");
    }
    return nullptr;
}

void ZLPlayer::setModelFile(char *data, int dataLen) {
    // 申请内存
    this->modelFileContent = new char[dataLen];
    // 复制内存
    memcpy(this->modelFileContent, data, dataLen);
    this->modelFileSize = dataLen;
}

ZLPlayer::ZLPlayer(char *modelFileData, int modelDataLen) {

    // this->data_source = new char[strlen(data_source) + 1];
    // strcpy(this->data_source, data_source); // 把源 Copy给成员
    // this->isStreaming = false;

    // Initialize rtsp_url with default value
    strcpy(rtsp_url, "rtsp://admin:sharpi1688@192.168.1.127");

    // Validate input parameters
    if (!modelFileData || modelDataLen <= 0) {
        LOGE("Invalid model data parameters: data=%p, size=%d", modelFileData, modelDataLen);
        throw std::runtime_error("Invalid model data parameters");
    }

    this->modelFileSize = modelDataLen;
    this->modelFileContent = (char *) malloc(modelDataLen);
    if (!this->modelFileContent) {
        LOGE("Failed to allocate memory for model data: %d bytes", modelDataLen);
        throw std::runtime_error("Failed to allocate memory for model data");
    }
    memcpy(this->modelFileContent, modelFileData, modelDataLen);

    LOGD("create mpp for model size: %d bytes", modelDataLen);
    // 创建上下文
    memset(&app_ctx, 0, sizeof(rknn_app_context_t)); // 初始化上下文

    try {
        // 创建YOLOv5线程池
        app_ctx.yolov5ThreadPool = new Yolov5ThreadPool(); // 创建线程池
        if (!app_ctx.yolov5ThreadPool) {
            throw std::runtime_error("Failed to create YOLOv5 thread pool");
        }

        // 设置模型数据，减少线程池大小以节省内存（多通道模式下使用更小的线程池）
        int result = app_ctx.yolov5ThreadPool->setUpWithModelData(5, this->modelFileContent, this->modelFileSize);
        if (result != NN_SUCCESS) {
            LOGE("Failed to setup YOLOv5 thread pool with model data, error: %d", result);
            throw std::runtime_error("Failed to setup YOLOv5 thread pool");
        }

        // Initialize render frame queue
        app_ctx.renderFrameQueue = new RenderFrameQueue();
        if (!app_ctx.renderFrameQueue) {
            throw std::runtime_error("Failed to create render frame queue");
        }

        // MPP 解码器
        if (app_ctx.decoder == nullptr) {
            LOGD("create decoder");
            MppDecoder *decoder = new MppDecoder();           // 创建解码器
            if (!decoder) {
                throw std::runtime_error("Failed to create MPP decoder");
            }

            int initResult = decoder->Init(264, 25, &app_ctx);  // 初始化解码器
            if (initResult != 0) {
                LOGE("Failed to initialize MPP decoder, error: %d", initResult);
                delete decoder;
                throw std::runtime_error("Failed to initialize MPP decoder");
            }

            decoder->SetCallback(mpp_decoder_frame_callback); // 设置回调函数，用来处理解码后的数据
            app_ctx.decoder = decoder;                        // 将解码器赋值给上下文
        } else {
            LOGD("decoder is not null");
        }

        // 启动rtsp线程
        int rtspResult = pthread_create(&pid_rtsp, nullptr, rtps_process, this);
        if (rtspResult != 0) {
            LOGE("Failed to create RTSP thread, error: %d", rtspResult);
            throw std::runtime_error("Failed to create RTSP thread");
        }

        // 启动显示线程
        int renderResult = pthread_create(&pid_render, nullptr, desplay_process, this);
        if (renderResult != 0) {
            LOGE("Failed to create render thread, error: %d", renderResult);
            throw std::runtime_error("Failed to create render thread");
        }

        LOGD("ZLPlayer initialized successfully");

    } catch (const std::exception& e) {
        LOGE("Exception during ZLPlayer initialization: %s", e.what());
        // Cleanup on failure
        if (app_ctx.yolov5ThreadPool) {
            delete app_ctx.yolov5ThreadPool;
            app_ctx.yolov5ThreadPool = nullptr;
        }
        if (app_ctx.renderFrameQueue) {
            delete app_ctx.renderFrameQueue;
            app_ctx.renderFrameQueue = nullptr;
        }
        if (app_ctx.decoder) {
            delete app_ctx.decoder;
            app_ctx.decoder = nullptr;
        }
        if (this->modelFileContent) {
            free(this->modelFileContent);
            this->modelFileContent = nullptr;
        }
        throw; // Re-throw the exception
    }
}

void API_CALL

on_track_frame_out(void *user_data, mk_frame frame) {
    rknn_app_context_t *ctx = (rknn_app_context_t *) user_data;
    // LOGD("on_track_frame_out ctx=%p\n", ctx);
    const char *data = mk_frame_get_data(frame);
    ctx->dts = mk_frame_get_dts(frame);
    ctx->pts = mk_frame_get_pts(frame);
    size_t size = mk_frame_get_data_size(frame);
    if (mk_frame_get_flags(frame) & MK_FRAME_FLAG_IS_KEY) {
        LOGD("Key frame size: %zu", size);
    } else if (MK_FRAME_FLAG_DROP_ABLE & mk_frame_get_flags(frame)) {
        LOGD("Drop able: %zu", size);
    } else if (MK_FRAME_FLAG_IS_CONFIG & mk_frame_get_flags(frame)) {
        LOGD("Config frame: %zu", size);
    } else if (MK_FRAME_FLAG_NOT_DECODE_ABLE & mk_frame_get_flags(frame)) {
        LOGD("Not decode able: %zu", size);
    } else {
        // LOGD("P-frame: %zu", size);
    }

    // LOGD("ctx->dts :%ld, ctx->pts :%ld", ctx->dts, ctx->pts);
    // LOGD("decoder=%p\n", ctx->decoder);
    ctx->decoder->Decode((uint8_t *) data, size, 0);
}

void API_CALL

on_mk_play_event_func(void *user_data, int err_code, const char *err_msg, mk_track tracks[],
                      int track_count) {
    rknn_app_context_t *ctx = (rknn_app_context_t *) user_data;
    if (err_code == 0) {
        // success
        LOGD("play success!");
        int i;
        // ctx->push_url = "rtmp://localhost/live/stream";
        // ctx->media = mk_media_create("__defaultVhost__", "live", "test", 0, 0, 0);
        for (i = 0; i < track_count; ++i) {
            if (mk_track_is_video(tracks[i])) {
                LOGD("got video track: %s", mk_track_codec_name(tracks[i]));
                // 监听track数据回调
                mk_track_add_delegate(tracks[i], on_track_frame_out, user_data);
            }
        }
    } else {
        printf("play failed: %d %s", err_code, err_msg);
    }
}

void API_CALL

on_mk_shutdown_func(void *user_data, int err_code, const char *err_msg, mk_track tracks[], int track_count) {
    printf("play interrupted: %d %s", err_code, err_msg);
}

// Channel-specific frame rendering using channel's own surface
void ZLPlayer::renderFrame(uint8_t *src_data, int width, int height, int src_line_size) {

    pthread_mutex_lock(&surfaceMutex);
    if (!channelSurface) {
        LOGW("Channel %d: ANativeWindow is null, cannot render frame. Surface was not set properly.", channelIndex);
        LOGW("Channel %d: Frame data - width: %d, height: %d, src_line_size: %d", channelIndex, width, height, src_line_size);
        pthread_mutex_unlock(&surfaceMutex);
        return;
    }

    // Enhanced Surface validity check
    int32_t surfaceWidth = ANativeWindow_getWidth(channelSurface);
    int32_t surfaceHeight = ANativeWindow_getHeight(channelSurface);
    int32_t surfaceFormat = ANativeWindow_getFormat(channelSurface);

    if (surfaceWidth <= 0 || surfaceHeight <= 0) {
        LOGE("Channel %d: Surface appears to be invalid - width: %d, height: %d, format: %d",
             channelIndex, surfaceWidth, surfaceHeight, surfaceFormat);
        LOGE("Channel %d: Surface pointer: %p, requesting surface recovery", channelIndex, channelSurface);

        // Mark surface as invalid and request recovery
        surfaceInvalidCount++;
        if (surfaceInvalidCount > MAX_SURFACE_INVALID_COUNT) {
            LOGE("Channel %d: Surface invalid count exceeded limit (%d), clearing surface",
                 channelIndex, MAX_SURFACE_INVALID_COUNT);
            ANativeWindow_release(channelSurface);
            channelSurface = nullptr;
            surfaceInvalidCount = 0;

            // Request recovery with timestamp
            if (!surfaceRecoveryRequested) {
                struct timeval currentTime;
                gettimeofday(&currentTime, NULL);
                surfaceRecoveryRequestTime = currentTime.tv_sec * 1000 + currentTime.tv_usec / 1000;
                surfaceRecoveryRequested = true;
                LOGW("Channel %d: Surface recovery requested at timestamp: %ld", channelIndex, surfaceRecoveryRequestTime);
            }
        }

        pthread_mutex_unlock(&surfaceMutex);
        return;
    } else {
        // Reset invalid count on successful validation
        surfaceInvalidCount = 0;
    }

    LOGD("Channel %d: Rendering frame to surface %p, size: %dx%d (surface: %dx%d)",
         channelIndex, channelSurface, width, height, surfaceWidth, surfaceHeight);

    // 设置窗口的大小，各个属性
    int setBuffersResult = ANativeWindow_setBuffersGeometry(channelSurface, width, height, WINDOW_FORMAT_RGBA_8888);
    if (setBuffersResult != 0) {
        LOGE("Channel %d: Failed to set buffer geometry, result: %d", channelIndex, setBuffersResult);
        pthread_mutex_unlock(&surfaceMutex);
        return;
    }

    // 他自己有个缓冲区 buffer
    ANativeWindow_Buffer window_buffer;

    // 如果我在渲染的时候，是被锁住的，那我就无法渲染，我需要释放 ，防止出现死锁
    int lockResult = ANativeWindow_lock(channelSurface, &window_buffer, 0);
    if (lockResult != 0) {
        LOGE("Channel %d: Failed to lock surface buffer, result: %d", channelIndex, lockResult);

        // Track consecutive lock failures
        surfaceLockFailCount++;
        if (surfaceLockFailCount > MAX_SURFACE_LOCK_FAIL_COUNT) {
            LOGE("Channel %d: Surface lock failures exceeded limit (%d), requesting surface recovery",
                 channelIndex, MAX_SURFACE_LOCK_FAIL_COUNT);
            surfaceLockFailCount = 0;

            // Request recovery with timestamp
            if (!surfaceRecoveryRequested) {
                struct timeval currentTime;
                gettimeofday(&currentTime, NULL);
                surfaceRecoveryRequestTime = currentTime.tv_sec * 1000 + currentTime.tv_usec / 1000;
                surfaceRecoveryRequested = true;
                LOGW("Channel %d: Surface recovery requested due to lock failures at timestamp: %ld",
                     channelIndex, surfaceRecoveryRequestTime);
            }
        }

        pthread_mutex_unlock(&surfaceMutex);
        return;
    } else {
        // Reset lock fail count on successful lock
        surfaceLockFailCount = 0;
    }

    // 填充[window_buffer]  画面就出来了  ==== 【目标 window_buffer】
    uint8_t *dst_data = static_cast<uint8_t *>(window_buffer.bits);
    int dst_linesize = window_buffer.stride * 4;

    for (int i = 0; i < window_buffer.height; ++i) {
        // 图：一行一行显示 [高度不用管，用循环了，遍历高度]
        // 通用的
        memcpy(dst_data + i * dst_linesize, src_data + i * src_line_size, dst_linesize); // OK的
    }

    // 数据刷新
    int unlockResult = ANativeWindow_unlockAndPost(channelSurface);
    if (unlockResult != 0) {
        LOGE("Channel %d: Failed to unlock and post surface buffer, result: %d", channelIndex, unlockResult);
    } else {
        // Log successful frame rendering periodically
        static int frameCounter = 0;
        frameCounter++;
        if (frameCounter % 300 == 0) { // Log every 300 frames (about every 10 seconds at 30fps)
            struct timeval currentTime;
            gettimeofday(&currentTime, NULL);
            long timestamp = currentTime.tv_sec * 1000 + currentTime.tv_usec / 1000;

            LOGD("Channel %d: Successfully rendered frame #%d at timestamp: %ld (surface: %p, size: %dx%d)",
                 channelIndex, frameCounter, timestamp, channelSurface, width, height);
        }
    }

    pthread_mutex_unlock(&surfaceMutex);
}

void ZLPlayer::display() {
    // Check if surface recovery is needed with timeout mechanism
    if (surfaceRecoveryRequested) {
        struct timeval currentTime;
        gettimeofday(&currentTime, NULL);
        long currentTimeMs = currentTime.tv_sec * 1000 + currentTime.tv_usec / 1000;

        // Check if recovery has timed out
        if (surfaceRecoveryRequestTime > 0 &&
            (currentTimeMs - surfaceRecoveryRequestTime) > SURFACE_RECOVERY_TIMEOUT_MS) {

            LOGE("Channel %d: Surface recovery timed out after %ld ms, attempt %d/%d",
                 channelIndex, (currentTimeMs - surfaceRecoveryRequestTime),
                 surfaceRecoveryAttempts, MAX_SURFACE_RECOVERY_ATTEMPTS);

            surfaceRecoveryAttempts++;

            if (surfaceRecoveryAttempts >= MAX_SURFACE_RECOVERY_ATTEMPTS) {
                LOGE("Channel %d: Maximum surface recovery attempts reached, forcing reset", channelIndex);
                // Force reset the recovery state to prevent permanent blocking
                surfaceRecoveryRequested = false;
                surfaceRecoveryRequestTime = 0;
                surfaceRecoveryAttempts = 0;
                surfaceInvalidCount = 0;
                surfaceLockFailCount = 0;

                // Continue with normal rendering attempt
            } else {
                // Reset recovery request time for next attempt
                surfaceRecoveryRequestTime = currentTimeMs;
                LOGW("Channel %d: Surface recovery timeout, retrying (attempt %d/%d)",
                     channelIndex, surfaceRecoveryAttempts, MAX_SURFACE_RECOVERY_ATTEMPTS);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                return;
            }
        } else {
            LOGW("Channel %d: Surface recovery requested, skipping frame rendering (elapsed: %ld ms)",
                 channelIndex, surfaceRecoveryRequestTime > 0 ? (currentTimeMs - surfaceRecoveryRequestTime) : 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return;
        }
    }

    int queueSize = app_ctx.renderFrameQueue->size();
    if (queueSize > 5) {  // Only log when queue is getting full
        LOGD("app_ctx.renderFrameQueue.size() :%d", queueSize);
    }

    auto frameDataPtr = app_ctx.renderFrameQueue->pop();
    if (frameDataPtr == nullptr) {
        // No frame available, this is normal with timeout-based pop
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }

    // Validate frame data before rendering
    if (!frameDataPtr->data || frameDataPtr->screenW <= 0 || frameDataPtr->screenH <= 0) {
        LOGE("Invalid frame data: data=%p, w=%d, h=%d",
             frameDataPtr->data.get(), frameDataPtr->screenW, frameDataPtr->screenH);
        return;
    }

    // Draw detection results on the frame if available
    if (frameDataPtr->hasDetections && !frameDataPtr->detections.empty()) {
        LOGD("Drawing %zu detections on frame %d", frameDataPtr->detections.size(), frameDataPtr->frameId);

        // Use enhanced detection rendering for better multi-channel performance
        if (enhancedDetectionRenderer) {
            enhancedDetectionRenderer->renderDetections(
                channelIndex,
                (uint8_t*)frameDataPtr->data.get(),
                frameDataPtr->screenW,
                frameDataPtr->screenH,
                frameDataPtr->screenStride,
                frameDataPtr->detections
            );
        } else {
            // Fallback to adaptive rendering
            DrawDetectionsAdaptive((uint8_t*)frameDataPtr->data.get(),
                                 frameDataPtr->screenW,
                                 frameDataPtr->screenH,
                                 frameDataPtr->screenStride,
                                 frameDataPtr->detections,
                                 channelIndex,
                                 isActiveChannel,
                                 getCurrentSystemLoad());
        }
    }

    // Render the frame using the smart pointer's get() method
    renderFrame((uint8_t *) frameDataPtr->data.get(), frameDataPtr->screenW,
                frameDataPtr->screenH, frameDataPtr->screenStride);

    // Frame data is managed by shared_ptr, no manual deletion needed
    LOGD("Rendered frame %d: %dx%d with %zu detections", frameDataPtr->frameId,
         frameDataPtr->screenW, frameDataPtr->screenH,
         frameDataPtr->hasDetections ? frameDataPtr->detections.size() : 0);

    // Control frame rate - limit to ~30 FPS
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
}

// Enhanced detection rendering methods implementation
void ZLPlayer::setEnhancedDetectionRenderer(std::shared_ptr<EnhancedDetectionRenderer> renderer) {
    enhancedDetectionRenderer = renderer;
    LOGD("Enhanced detection renderer set for ZLPlayer");
}

void ZLPlayer::setRenderingMonitor(std::shared_ptr<DetectionRenderingMonitor> monitor) {
    renderingMonitor = monitor;
    LOGD("Rendering monitor set for ZLPlayer");
}

void ZLPlayer::setChannelIndex(int index) {
    channelIndex = index;
    LOGD("Channel index set to %d", index);
}

void ZLPlayer::setActiveChannel(bool active) {
    isActiveChannel = active;
    if (enhancedDetectionRenderer) {
        enhancedDetectionRenderer->setChannelActive(channelIndex, active);
    }
    LOGD("Channel %d active state set to %s", channelIndex, active ? "true" : "false");
}

void ZLPlayer::updateSystemLoad(float load) {
    currentSystemLoad = load;
    if (enhancedDetectionRenderer) {
        enhancedDetectionRenderer->updateSystemLoad(load);
    }
}

float ZLPlayer::getCurrentSystemLoad() const {
    return currentSystemLoad;
}

// Channel-specific surface management
void ZLPlayer::setChannelSurface(ANativeWindow* surface) {
    struct timeval currentTime;
    gettimeofday(&currentTime, NULL);
    long timestamp = currentTime.tv_sec * 1000 + currentTime.tv_usec / 1000;

    LOGD("setChannelSurface called for channel %d, surface: %p at timestamp: %ld", channelIndex, surface, timestamp);

    pthread_mutex_lock(&surfaceMutex);

    // Release previous surface
    if (channelSurface) {
        // Log detailed surface information before release
        int32_t oldWidth = ANativeWindow_getWidth(channelSurface);
        int32_t oldHeight = ANativeWindow_getHeight(channelSurface);
        int32_t oldFormat = ANativeWindow_getFormat(channelSurface);

        LOGD("Channel %d: Releasing previous surface: %p (size: %dx%d, format: %d) at timestamp: %ld",
             channelIndex, channelSurface, oldWidth, oldHeight, oldFormat, timestamp);

        ANativeWindow_release(channelSurface);
        channelSurface = nullptr;

        // Reset error counters and recovery state when surface is changed
        surfaceInvalidCount = 0;
        surfaceLockFailCount = 0;
        surfaceRecoveryRequested = false;
        surfaceRecoveryRequestTime = 0;
        surfaceRecoveryAttempts = 0;
    }

    // Set new surface
    channelSurface = surface;
    if (surface) {
        ANativeWindow_acquire(surface);

        // Log detailed new surface information
        int32_t newWidth = ANativeWindow_getWidth(surface);
        int32_t newHeight = ANativeWindow_getHeight(surface);
        int32_t newFormat = ANativeWindow_getFormat(surface);

        LOGD("Channel %d surface set and acquired: %p (size: %dx%d, format: %d) at timestamp: %ld",
             channelIndex, surface, newWidth, newHeight, newFormat, timestamp);

        // Validate new surface immediately
        if (newWidth <= 0 || newHeight <= 0) {
            LOGE("Channel %d: WARNING - New surface has invalid dimensions: %dx%d",
                 channelIndex, newWidth, newHeight);
        }
    } else {
        LOGD("Channel %d surface cleared at timestamp: %ld", channelIndex, timestamp);
    }

    pthread_mutex_unlock(&surfaceMutex);
}

ANativeWindow* ZLPlayer::getChannelSurface() const {
    return channelSurface;
}

// Surface recovery and health monitoring methods
bool ZLPlayer::isSurfaceRecoveryRequested() const {
    return surfaceRecoveryRequested;
}

void ZLPlayer::clearSurfaceRecoveryRequest() {
    surfaceRecoveryRequested = false;
    surfaceRecoveryRequestTime = 0;
    surfaceRecoveryAttempts = 0;
    surfaceInvalidCount = 0;
    surfaceLockFailCount = 0;
    LOGD("Channel %d: Surface recovery request cleared completely", channelIndex);
}

void ZLPlayer::requestSurfaceRecovery() {
    surfaceRecoveryRequested = true;
    LOGW("Channel %d: Surface recovery requested", channelIndex);
}

bool ZLPlayer::validateSurfaceHealth() {
    pthread_mutex_lock(&surfaceMutex);

    if (!channelSurface) {
        pthread_mutex_unlock(&surfaceMutex);
        return false;
    }

    // Check surface dimensions and format
    int32_t width = ANativeWindow_getWidth(channelSurface);
    int32_t height = ANativeWindow_getHeight(channelSurface);
    int32_t format = ANativeWindow_getFormat(channelSurface);

    bool isHealthy = (width > 0 && height > 0 && format > 0);

    if (!isHealthy) {
        LOGW("Channel %d: Surface health check failed - width: %d, height: %d, format: %d",
             channelIndex, width, height, format);
    } else {
        LOGD("Channel %d: Surface health check passed - %dx%d, format: %d",
             channelIndex, width, height, format);
    }

    pthread_mutex_unlock(&surfaceMutex);
    return isHealthy;
}

void ZLPlayer::forceSurfaceReset() {
    pthread_mutex_lock(&surfaceMutex);

    LOGW("Channel %d: Force resetting surface state", channelIndex);

    // Clear all surface-related state
    surfaceRecoveryRequested = false;
    surfaceRecoveryRequestTime = 0;
    surfaceRecoveryAttempts = 0;
    surfaceInvalidCount = 0;
    surfaceLockFailCount = 0;

    // If we have a surface, release it
    if (channelSurface) {
        LOGW("Channel %d: Releasing surface during force reset: %p", channelIndex, channelSurface);
        ANativeWindow_release(channelSurface);
        channelSurface = nullptr;
    }

    pthread_mutex_unlock(&surfaceMutex);

    LOGW("Channel %d: Surface force reset completed", channelIndex);
}

void ZLPlayer::get_detect_result() {
    // 添加空指针检查和线程池初始化检查
    if (!app_ctx.yolov5ThreadPool) {
        LOGE("yolov5ThreadPool is null, skipping result retrieval");
        return;
    }

    // 检查渲染队列大小，防止内存积累
    int queueSize = app_ctx.renderFrameQueue->size();
    if (queueSize > DISPLAY_QUEUE_MAX_SIZE / 2) {
        LOGW("Render queue getting full (%d), clearing old frames", queueSize);
        app_ctx.renderFrameQueue->clear();
    }

    // 检查线程池是否已正确初始化
    try {
        std::vector<Detection> objects;

        // 安全地调用getTargetResultNonBlock
        auto ret_code = app_ctx.yolov5ThreadPool->getTargetResultNonBlock(objects, app_ctx.result_cnt);

        if (ret_code == NN_SUCCESS) {
            LOGD("Successfully got detection results, count: %zu", objects.size());

            // Only log detection details in debug builds to reduce log spam
            #ifdef DEBUG
            for (size_t idx = 0; idx < objects.size() && idx < 5; idx++) {  // Limit to first 5 objects
                LOGD("objects[%zu].classId: %d, prop: %f, class: %s",
                     idx, objects[idx].class_id, objects[idx].confidence, objects[idx].className.c_str());
            }
            #endif

            // 安全地获取图像结果
            auto frameData = app_ctx.yolov5ThreadPool->getTargetImgResult(app_ctx.result_cnt);
            if (frameData && frameData->data) {
                app_ctx.result_cnt++;
                LOGD("Get detect result counter:%d start display", app_ctx.result_cnt);

                // 将检测结果存储到frame数据中
                frameData->detections = objects;
                frameData->hasDetections = true;

                LOGD("Stored %zu detections in frame %d", objects.size(), frameData->frameId);

                // 加入渲染队列
                app_ctx.renderFrameQueue->push(frameData);
                LOGD("Frame %d pushed to render queue, queue size: %d",
                     frameData->frameId, app_ctx.renderFrameQueue->size());

            } else {
                LOGW("frameData is null or frameData->data is null for result %d", app_ctx.result_cnt);
            }

        } else if (NN_RESULT_NOT_READY == ret_code) {
            // Normal case - no result ready yet
        } else {
            LOGW("getTargetResultNonBlock returned error code: %d", ret_code);
        }
    } catch (const std::exception& e) {
        LOGE("Exception in get_detect_result: %s", e.what());
    } catch (...) {
        LOGE("Unknown exception in get_detect_result");
    }
}

int ZLPlayer::process_video_rtsp() {

    mk_config config;
    memset(&config, 0, sizeof(mk_config));
    config.log_mask = LOG_CONSOLE;
    mk_env_init(&config);
    mk_player player = mk_player_create();
    mk_player_set_on_result(player, on_mk_play_event_func, &app_ctx);
    mk_player_set_on_shutdown(player, on_mk_shutdown_func, &app_ctx);
    mk_player_play(player, rtsp_url);

    while (1) {
        // sleep(1);
        // LOGD("Running");
        // display();
        // display();
        get_detect_result();
    }

#if 0
    std::vector<Detection> objects;
    cv::Mat origin_mat = cv::Mat::zeros(height, width, CV_8UC3);

    // LOGD("enter any key to exit\n");
    while (1) {
        // LOGD("running\n");
        // sleep(1);
        usleep(1000);

        // 获取推理结果
        auto ret_code = yolov8_thread_pool->getTargetResultNonBlock(objects, result_cnt);
        auto ret_code = yolov8_thread_pool-> getTargetImgResult(objects, result_cnt);
        if (ret_code == NN_SUCCESS) {
            result_cnt++;
        }else{
            continue;
        }

        DrawDetections(origin_mat, objects);

        cv::cvtColor(origin_mat, origin_mat, cv::COLOR_RGB2RGBA);
        this->renderFrame(origin_mat.data, width, height, width * get_bpp_from_format(RK_FORMAT_RGBA_8888));

        gettimeofday(&end, NULL);

        double timeused = 1000 * (end.tv_sec - now.tv_sec) + (end.tv_usec - now.tv_usec) / 1000;
        // LOGD("Spent:%f", timeused);

        long frameGap = end.tv_sec * 1000 + end.tv_usec / 1000 - lastRenderTime.tv_usec / 1000 - lastRenderTime.tv_sec * 1000;

        LOGD("Frame gap :%ld\n", frameGap);

        gettimeofday(&lastRenderTime, NULL);

    }
#endif

    if (player) {
        mk_player_release(player);
    }
    return 0;
}

ZLPlayer::~ZLPlayer() {
    LOGD("ZLPlayer destructor called");

    // Stop threads gracefully
    if (app_ctx.yolov5ThreadPool) {
        app_ctx.yolov5ThreadPool->stopAll();
    }

    // Wait for threads to finish
    // Note: pthread_cancel is not available on Android, threads should be stopped gracefully
    if (pid_rtsp != 0) {
        pthread_join(pid_rtsp, nullptr);
        pid_rtsp = 0;
    }

    if (pid_render != 0) {
        pthread_join(pid_render, nullptr);
        pid_render = 0;
    }

    // Clean up resources
    if (app_ctx.renderFrameQueue) {
        delete app_ctx.renderFrameQueue;
        app_ctx.renderFrameQueue = nullptr;
    }

    if (app_ctx.yolov5ThreadPool) {
        delete app_ctx.yolov5ThreadPool;
        app_ctx.yolov5ThreadPool = nullptr;
    }

    if (app_ctx.decoder) {
        delete app_ctx.decoder;
        app_ctx.decoder = nullptr;
    }

    if (modelFileContent) {
        free(modelFileContent);
        modelFileContent = nullptr;
    }

    // Release channel surface
    if (channelSurface) {
        ANativeWindow_release(channelSurface);
        channelSurface = nullptr;
    }

    LOGD("ZLPlayer destructor completed");
}

static struct timeval lastRenderTime;

void ZLPlayer::mpp_decoder_frame_callback(void *userdata, int width_stride, int height_stride, int width, int height, int format, int fd, void *data) {
    rknn_app_context_t *ctx = (rknn_app_context_t *) userdata;
    struct timeval start;
    struct timeval end;
    struct timeval memCpyEnd;
    gettimeofday(&start, NULL);
    long frameGap = start.tv_sec * 1000 + start.tv_usec / 1000 - lastRenderTime.tv_usec / 1000 - lastRenderTime.tv_sec * 1000;
    // LOGD("decoded frame ctx->dts:%ld", ctx->dts);
    LOGD("mpp_decoder_frame_callback Frame gap :%ld\n", frameGap);
    gettimeofday(&lastRenderTime, NULL);

    // 12,441,600 3840x2160x3/2
    // int imgSize = width * height * get_bpp_from_format(RK_FORMAT_RGBA_8888);
#if 0
    auto frameData = std::make_shared<frame_data_t>();

    char *dstBuf = new char[imgSize]();
    memcpy(dstBuf, data, imgSize);

    gettimeofday(&memCpyEnd, NULL);
    frameGap = memCpyEnd.tv_sec * 1000 + memCpyEnd.tv_usec / 1000 - start.tv_usec / 1000 - start.tv_sec * 1000;
    LOGD("mpp_decoder_frame_callback Frame mem cpy spent :%ld\n", frameGap);

    frameData->dataSize = imgSize;
    frameData->screenStride = width * get_bpp_from_format(RK_FORMAT_YCbCr_420_SP);  // 解码出来的格式就是nv12
    frameData->data = dstBuf;
    frameData->screenW = width;
    frameData->screenH = height;
    frameData->heightStride = height_stride;
    frameData->widthStride = width_stride;
    frameData->frameFormat = RK_FORMAT_YCbCr_420_SP;
    frameData->frameId = ctx->job_cnt;

    ctx->job_cnt++;

    // 放入显示队列
    ctx->renderFrameQueue->push(frameData);

    LOGD("mpp_decoder_frame_callback task list size :%d", ctx->mppDataThreadPool->get_task_size());

    // 放入线程池, 进行并行推理
    ctx->mppDataThreadPool->submitTask(frameData);

    gettimeofday(&end, NULL);
    frameGap = end.tv_sec * 1000 + end.tv_usec / 1000 - start.tv_usec / 1000 - start.tv_sec * 1000;
    LOGD("mpp_decoder_frame_callback Frame spent :%ld\n", frameGap);

    return;
#endif

    int dstImgSize = width_stride * height_stride * get_bpp_from_format(RK_FORMAT_RGBA_8888);
    LOGD("img size is %d", dstImgSize);
    // img size is 33177600 1080p: 8355840

    // Use smart pointer for automatic memory management (C++11 compatible)
    std::unique_ptr<char[]> dstBuf(new char[dstImgSize]());

    rga_change_color(width_stride, height_stride, RK_FORMAT_YCbCr_420_SP, (char *) data,
                     width_stride, height_stride, RK_FORMAT_RGBA_8888, dstBuf.get());

    auto frameData = std::make_shared<frame_data_t>();
    frameData->dataSize = dstImgSize;
    frameData->screenStride = width * get_bpp_from_format(RK_FORMAT_RGBA_8888);
    frameData->data = std::move(dstBuf);  // Transfer ownership to frameData
    frameData->screenW = width;
    frameData->screenH = height;
    frameData->heightStride = height_stride;
    frameData->widthStride = width_stride;
    frameData->frameFormat = RK_FORMAT_RGBA_8888;

    // LOGD(">>>>>  frame id:%d", frameData->frameId);
    // LOGD("mpp_decoder_frame_callback task list size :%d", ctx->mppDataThreadPool->get_task_size());

    // 放入显示队列
    // ctx->renderFrameQueue->push(frameData);

    frameData->frameId = ctx->job_cnt;
    int detectPoolSize = ctx->yolov5ThreadPool->get_task_size();
    LOGD("detectPoolSize :%d", detectPoolSize);

    // ctx->mppDataThreadPool->submitTask(frameData);
    // ctx->job_cnt++;
    // 如果frameData->frameId为奇数
    ctx->frame_cnt++;
    ctx->yolov5ThreadPool->submitTask(frameData);
    ctx->job_cnt++;

    //    if (ctx->frame_cnt % 2 == 1) {
    //        // if (detectPoolSize < MAX_TASK) {
    //        // 放入线程池, 进行并行推理
    //        ctx->yolov5ThreadPool->submitTask(frameData);
    //        ctx->job_cnt++;
    //    } else {
    //        // 直接释放
    //        delete frameData->data;
    //        frameData->data = nullptr;
    //    }

}

#if 0

void mpp_decoder_frame_callback_good_display(void *userdata, int width_stride, int height_stride, int width, int height, int format, int fd, void *data) {
    rknn_app_context_t *ctx = (rknn_app_context_t *) userdata;
    struct timeval end;
    gettimeofday(&end, NULL);
    long frameGap = end.tv_sec * 1000 + end.tv_usec / 1000 -
                    lastRenderTime.tv_usec / 1000 - lastRenderTime.tv_sec * 1000;
    // LOGD("decoded frame ctx->dts:%ld", ctx->dts);
    LOGD("mpp_decoder_frame_callback Frame gap :%ld\n", frameGap);
    gettimeofday(&lastRenderTime, NULL);

    rga_buffer_t origin;

    int imgSize = width * height * get_bpp_from_format(RK_FORMAT_RGBA_8888);
    // char *dstBuf = (char *) malloc(imgSize);
    // memset(dstBuf, 0, imgSize);
    // std::unique_ptr<char[]> dstBuf(new char[imgSize]());

    char *dstBuf = new char[imgSize]();
    rga_change_color_async(width_stride, height_stride, RK_FORMAT_YCbCr_420_SP, (char *) data,
                           width, height, RK_FORMAT_RGBA_8888, dstBuf);

    // usleep(1000 * 80);

#if 0

    origin = wrapbuffer_fd(fd, width, height, RK_FORMAT_YCbCr_420_SP, width_stride, height_stride);
    cv::Mat origin_mat = cv::Mat::zeros(height, width, CV_8UC3);
    rga_buffer_t rgb_img = wrapbuffer_virtualaddr((void *) origin_mat.data, width, height, RK_FORMAT_RGB_888);
    imcopy(origin, rgb_img);


    // yolov8_thread_pool->submitTask(origin_mat, ctx->job_cnt++);
    yolov8_thread_pool->submitTask(origin_mat, ctx->job_cnt);
    std::vector<Detection> objects;

    // 获取推理结果
    // auto ret_code = yolov8_thread_pool->getTargetResultNonBlock(objects, ctx->result_cnt);
    auto ret_code = yolov8_thread_pool->getTargetResultNonBlock(objects, ctx->job_cnt);
    if (ret_code == NN_SUCCESS) {
        ctx->result_cnt++;
    }
    LOGD("ctx->result_cnt:%d", ctx->result_cnt);



    detect_result_group_t detect_result_group;
    memset(&detect_result_group, 0, sizeof(detect_result_group_t));
    detect_result_group.count = objects.size();
    uint8_t idx;
    for (idx = 0; idx < objects.size(); idx++) {
        LOGD("objects[%d].classId: %d\n", idx, objects[idx].class_id);
        LOGD("objects[%d].prop: %f\n", idx, objects[idx].confidence);
        LOGD("objects[%d].class name: %s\n", idx, objects[idx].className.c_str());
        // int left;
        // int right;
        // int top;
        // int bottom;
        detect_result_group.results[idx].box.left = objects[idx].box.x;
        detect_result_group.results[idx].box.right = objects[idx].box.x + objects[idx].box.width;
        detect_result_group.results[idx].box.top = objects[idx].box.y;
        detect_result_group.results[idx].box.bottom = objects[idx].box.y + objects[idx].box.height;
        detect_result_group.results[idx].classId = objects[idx].class_id;
        detect_result_group.results[idx].prop = objects[idx].confidence;
        strcpy(detect_result_group.results[idx].name, objects[idx].className.c_str());
    }
#endif

    // frame_data_t *frameData = new frame_data_t();
    auto frameData = std::make_shared<frame_data_t>();
    frameData->dataSize = imgSize;
    frameData->screenStride = width * get_bpp_from_format(RK_FORMAT_RGBA_8888);
    frameData->data = dstBuf;
    frameData->screenW = width;
    frameData->screenH = height;
    frameData->heightStride = height_stride;
    frameData->widthStride = width_stride;

    ctx->renderFrameQueue->push(frameData);
    LOGD("Now render frame queue size is :%d", ctx->renderFrameQueue->size());

    //
    //    rga_buffer_t origin;
    //    rga_buffer_t src;
    //    int mpp_frame_fd = 0;
    //
    //    // 复制到另一个缓冲区，避免修改mpp解码器缓冲区
    //    // 使用的是RK RGA的格式转换：YUV420SP -> RGB888
    //    origin = wrapbuffer_fd(fd, width, height, RK_FORMAT_YCbCr_420_SP, width_stride, height_stride);
    //    src = wrapbuffer_fd(mpp_frame_fd, width, height, RK_FORMAT_YCbCr_420_SP, width_stride, height_stride);
    //    cv::Mat origin_mat = cv::Mat::zeros(height, width, CV_8UC3);
    //    rga_buffer_t rgb_img = wrapbuffer_virtualaddr((void *) origin_mat.data, width, height, RK_FORMAT_RGB_888);
    //    imcopy(origin, rgb_img);
    //
    //    LOGD("task size is:%d\n", yolov8_thread_pool->get_task_size());
    //    // 提交推理任务给线程池
    //    yolov8_thread_pool->submitTask(origin_mat, job_cnt++);
    //    std::vector<Detection> objects;
    // yolov8_thread_pool->submitTask(origin_mat, job_cnt++);
}

// 解码后的数据回调函数
void ZLPlayer::mpp_decoder_frame_callback(void *userdata, int width_stride, int height_stride, int width, int height, int format, int fd, void *data) {

    //    LOGD("width_stride :%d height_stride :%d width :%d height :%d format :%d\n",
    //         width_stride, height_stride, width, height, format);
    // LOGD("mpp_decoder_frame_callback\n");
    struct timeval now;
    struct timeval end;
    gettimeofday(&now, NULL);
    rknn_app_context_t *ctx = (rknn_app_context_t *) userdata;

    int ret = 0;
    static int frame_index = 0;
    frame_index++;

    void *mpp_frame = NULL;
    int mpp_frame_fd = 0;
    void *mpp_frame_addr = NULL;
    int enc_data_size;

    rga_buffer_t origin;
    rga_buffer_t src;

    // 复制到另一个缓冲区，避免修改mpp解码器缓冲区
    // 使用的是RK RGA的格式转换：YUV420SP -> RGB888
    origin = wrapbuffer_fd(fd, width, height, RK_FORMAT_YCbCr_420_SP, width_stride, height_stride);
    src = wrapbuffer_fd(mpp_frame_fd, width, height, RK_FORMAT_YCbCr_420_SP, width_stride, height_stride);
    cv::Mat origin_mat = cv::Mat::zeros(height, width, CV_8UC3);
    rga_buffer_t rgb_img = wrapbuffer_virtualaddr((void *) origin_mat.data, width, height, RK_FORMAT_RGB_888);
    imcopy(origin, rgb_img);

    static int job_cnt = 0;
    static int result_cnt = 0;
    LOGD("task size is:%d\n", yolov8_thread_pool->get_task_size());
    // 提交推理任务给线程池
    yolov8_thread_pool->submitTask(origin_mat, job_cnt++);
    std::vector<Detection> objects;
    // 获取推理结果
    auto ret_code = yolov8_thread_pool->getTargetResultNonBlock(objects, result_cnt);
    if (ret_code == NN_SUCCESS) {
        result_cnt++;
    }

    uint8_t idx;
    for (idx = 0; idx < objects.size(); idx++) {
        LOGD("objects[%d].classId: %d\n", idx, objects[idx].class_id);
        LOGD("objects[%d].prop: %f\n", idx, objects[idx].confidence);
        LOGD("objects[%d].class name: %s\n", idx, objects[idx].className.c_str());
        // int left;
        // int right;
        // int top;
        // int bottom;
    }

    DrawDetections(origin_mat, objects);
    // imcopy(rgb_img, src);

    // LOGD("result_cnt: %d\n", result_cnt);
    cv::cvtColor(origin_mat, origin_mat, cv::COLOR_RGB2RGBA);
    this->renderFrame(origin_mat.data, width, height, width * get_bpp_from_format(RK_FORMAT_RGBA_8888));

    gettimeofday(&end, NULL);

    double timeused = 1000 * (end.tv_sec - now.tv_sec) + (end.tv_usec - now.tv_usec) / 1000;
    // LOGD("Spent:%f", timeused);

    long frameGap = end.tv_sec * 1000 + end.tv_usec / 1000 - lastRenderTime.tv_usec / 1000 - lastRenderTime.tv_sec * 1000;

    LOGD("Frame gap :%ld\n", frameGap);

    gettimeofday(&lastRenderTime, NULL);

}
#endif

//void ZLPlayer::setRenderCallback(RenderCallback renderCallback_) {
//    this->renderCallback = renderCallback_;
//}
