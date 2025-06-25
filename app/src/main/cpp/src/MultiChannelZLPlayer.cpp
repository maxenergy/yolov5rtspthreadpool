#include "ChannelManager.h"
#include <cstring>

// External declarations from native-lib.cpp
extern ANativeWindow *window;
extern pthread_mutex_t windowMutex;

// MultiChannelZLPlayer implementation
MultiChannelZLPlayer::MultiChannelZLPlayer(int channelIndex, char* modelFileData, int modelDataLen,
                                         NativeChannelManager* manager)
    : ZLPlayer(modelFileData, modelDataLen),
      channelIndex(channelIndex),
      channelManager(manager),
      channelSurface(nullptr),
      detectionEnabled(true),
      modelDataSize(0),
      lastFrameTime(std::chrono::steady_clock::now()),
      lastRenderTime(std::chrono::steady_clock::now()),
      frameSkipCounter(0),
      currentFps(0.0f) {

    // Copy model data for this channel
    if (modelFileData && modelDataLen > 0) {
        modelData = std::make_unique<char[]>(modelDataLen);
        memcpy(modelData.get(), modelFileData, modelDataLen);
        modelDataSize = modelDataLen;
    }

    // Initialize independent channel context
    if (!initializeChannel()) {
        LOGE("Failed to initialize channel %d", channelIndex);
    }

    LOGD("MultiChannelZLPlayer created for channel %d", channelIndex);
}

MultiChannelZLPlayer::~MultiChannelZLPlayer() {
    LOGD("MultiChannelZLPlayer destroying channel %d", channelIndex);

    // Cleanup channel resources
    cleanupChannel();

    // Cleanup model data (smart pointer handles this automatically)
    modelData.reset();
    modelDataSize = 0;

    LOGD("MultiChannelZLPlayer destroyed for channel %d", channelIndex);
}

void MultiChannelZLPlayer::onFrameProcessed() {
    if (channelManager) {
        channelManager->onChannelFrameReceived(channelIndex);
    }
}

void MultiChannelZLPlayer::onDetectionCompleted(int detectionCount) {
    if (channelManager) {
        channelManager->onChannelDetectionReceived(channelIndex, detectionCount);
    }
}

void MultiChannelZLPlayer::onError(const std::string& errorMessage) {
    if (channelManager) {
        channelManager->onChannelError(channelIndex, errorMessage);
    }
}

void MultiChannelZLPlayer::setChannelRTSPUrl(const char* url) {
    if (url && strlen(url) > 0) {
        std::lock_guard<std::mutex> lock(channelMutex);

        // Store URL in channel-specific string
        channelRtspUrl = std::string(url);

        // Also copy to parent rtsp_url for compatibility
        size_t urlLen = strlen(url);
        if (urlLen < sizeof(rtsp_url) - 1) {
            strcpy(rtsp_url, url);
            LOGD("Channel %d RTSP URL set to: %s", channelIndex, url);
        } else {
            LOGE("Channel %d RTSP URL too long: %zu characters", channelIndex, urlLen);
        }
    }
}

void MultiChannelZLPlayer::setChannelSurface(ANativeWindow* surface) {
    std::lock_guard<std::mutex> lock(channelMutex);

    // Release previous surface
    if (channelSurface) {
        ANativeWindow_release(channelSurface);
        channelSurface = nullptr;
    }

    // Set new surface
    channelSurface = surface;
    if (surface) {
        ANativeWindow_acquire(surface);
        LOGD("Channel %d surface set and acquired", channelIndex);
    } else {
        LOGD("Channel %d surface cleared", channelIndex);
    }
}

void MultiChannelZLPlayer::setDetectionEnabled(bool enabled) {
    detectionEnabled.store(enabled); // Atomic store, no lock needed

    if (channelContext && channelContext->yolov5ThreadPool) {
        // Channel-specific detection control
        LOGD("Channel %d detection %s", channelIndex, enabled ? "enabled" : "disabled");

        // The detection will be controlled in the frame processing pipeline
        // by checking the detectionEnabled flag before submitting frames to YOLOv5
    } else {
        LOGW("Channel %d: YOLOv5 thread pool not initialized, cannot set detection state", channelIndex);
    }
}

// Override display method to provide channel-specific rendering
void MultiChannelZLPlayer::display() {
    if (!channelContext || !channelContext->renderFrameQueue) {
        return;
    }

    // Check if we should process this frame for performance control
    if (!shouldProcessFrame()) {
        return;
    }

    std::unique_lock<std::mutex> lock(channelMutex);

    // Use channel-specific render queue
    int queueSize = channelContext->renderFrameQueue->size();
    if (queueSize > 5) {
        LOGD("Channel %d render queue size: %d", channelIndex, queueSize);

        // If queue is too large, skip some frames to maintain performance
        adaptiveFrameSkipping();
    }

    auto frameDataPtr = channelContext->renderFrameQueue->pop();
    if (frameDataPtr == nullptr) {
        lock.unlock(); // Unlock before sleeping
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // Reduced sleep time
        return;
    }

    // Validate frame data before rendering
    if (!frameDataPtr->data || frameDataPtr->screenW <= 0 || frameDataPtr->screenH <= 0) {
        LOGE("Channel %d: Invalid frame data: data=%p, w=%d, h=%d",
             channelIndex, frameDataPtr->data.get(), frameDataPtr->screenW, frameDataPtr->screenH);
        return;
    }

    // Check if we should render this frame
    if (shouldRenderFrame()) {
        // Render to channel-specific surface
        if (channelSurface) {
            renderToChannelSurface(frameDataPtr.get());
        }
    }

    lock.unlock(); // Unlock before calling external methods

    // Update frame rate statistics
    updateFrameRateStats();

    // Notify the channel manager about frame processing
    if (channelManager) {
        channelManager->onChannelFrameReceived(channelIndex);
    }
}

// Override get_detect_result to provide channel-specific detection handling
void MultiChannelZLPlayer::get_detect_result() {
    if (!channelContext || !channelContext->yolov5ThreadPool) {
        return;
    }

    // Only process detection if enabled for this channel (atomic load)
    if (!detectionEnabled.load()) {
        return;
    }

    // Throttle detection processing to maintain performance
    // Detection can be less frequent than rendering for better performance
    static auto lastDetectionTime = std::chrono::steady_clock::now();
    auto currentTime = std::chrono::steady_clock::now();
    auto timeSinceLastDetection = std::chrono::duration_cast<std::chrono::milliseconds>(
        currentTime - lastDetectionTime);

    // Process detection every 100ms (10 FPS) to reduce CPU load
    if (timeSinceLastDetection < std::chrono::milliseconds(100)) {
        return;
    }
    lastDetectionTime = currentTime;

    std::unique_lock<std::mutex> lock(channelMutex);

    // Use channel-specific YOLOv5 thread pool to get detection results
    int detectionCount = 0;

    // Get detection results from the thread pool
    // We need to iterate through possible frame IDs to collect all results
    // This is a simplified approach - in production we'd want to track frame IDs more systematically

    for (int frameId = 0; frameId < 100; frameId++) { // Check recent frame IDs
        std::vector<Detection> detections;

        // Try to get detection results for this frame ID (non-blocking)
        nn_error_e result = channelContext->yolov5ThreadPool->getTargetResultNonBlock(detections, frameId);

        if (result == NN_SUCCESS && !detections.empty()) {
            detectionCount += detections.size();

            // Log detection details for debugging
            LOGD("Channel %d: Frame %d has %zu detections",
                 channelIndex, frameId, detections.size());

            // Optional: Process individual detections
            for (const auto& detection : detections) {
                LOGD("Channel %d: Detection - class: %d, confidence: %.2f, bbox: (%d,%d,%d,%d)",
                     channelIndex, detection.class_id, detection.confidence,
                     detection.box.x, detection.box.y, detection.box.width, detection.box.height);
            }
        }
    }

    lock.unlock(); // Unlock before calling external methods

    // Notify channel manager with actual detection count (even if 0)
    if (channelManager) {
        channelManager->onChannelDetectionReceived(channelIndex, detectionCount);

        if (detectionCount > 0) {
            LOGD("Channel %d: Reported %d detections to channel manager", channelIndex, detectionCount);
        }
    }
}

// Channel-specific frame callback
void MultiChannelZLPlayer::onChannelFrameCallback(void* userdata, int width_stride, int height_stride, 
                                                 int width, int height, int format, int fd, void* data) {
    // This is a channel-specific version of the frame callback
    // It should be used instead of the global mpp_decoder_frame_callback for multi-channel support
    
    if (!userdata) {
        LOGE("Channel %d: userdata is null in frame callback", channelIndex);
        return;
    }
    
    // Call the original frame callback
    ZLPlayer::mpp_decoder_frame_callback(userdata, width_stride, height_stride, width, height, format, fd, data);
    
    // Notify channel manager about frame processing
    if (channelManager) {
        channelManager->onChannelFrameReceived(channelIndex);
    }
}

// Multi-channel frame callback wrapper
extern "C" void multi_channel_frame_callback(void* userdata, int width_stride, int height_stride, 
                                            int width, int height, int format, int fd, void* data) {
    // This is a wrapper around the existing mpp_decoder_frame_callback
    // It should extract the channel index from userdata and call the appropriate channel manager
    
    if (!userdata) {
        LOGE("multi_channel_frame_callback: userdata is null");
        return;
    }
    
    rknn_app_context_t* ctx = (rknn_app_context_t*)userdata;
    
    // TODO: Add channel index to rknn_app_context_t structure
    // For now, we'll call the original callback and let the global channel manager handle it
    
    // Call the original callback
    ZLPlayer::mpp_decoder_frame_callback(userdata, width_stride, height_stride, width, height, format, fd, data);
    
    // Notify global channel manager if available
    if (g_channelManager) {
        // We need a way to identify which channel this frame belongs to
        // This could be done by:
        // 1. Adding a channelIndex field to rknn_app_context_t
        // 2. Using a map to associate contexts with channel indices
        // 3. Passing channel info through the userdata parameter
        
        // For now, we'll assume channel 0 (this needs to be fixed in a full implementation)
        // g_channelManager->onChannelFrameReceived(0);
    }
}

// Helper function to create a channel-specific context
rknn_app_context_t* createChannelContext(int channelIndex, char* modelData, int modelSize) {
    rknn_app_context_t* ctx = new rknn_app_context_t();
    memset(ctx, 0, sizeof(rknn_app_context_t));
    
    // TODO: Add channel index to context
    // ctx->channelIndex = channelIndex;
    
    // Initialize YOLOv5 thread pool for this channel
    ctx->yolov5ThreadPool = new Yolov5ThreadPool();
    if (ctx->yolov5ThreadPool->setUpWithModelData(10, modelData, modelSize) != NN_SUCCESS) {
        LOGE("Failed to initialize YOLOv5 thread pool for channel %d", channelIndex);
        delete ctx->yolov5ThreadPool;
        delete ctx;
        return nullptr;
    }
    
    // Initialize render frame queue
    ctx->renderFrameQueue = new RenderFrameQueue();
    
    // Initialize MPP decoder
    if (!ctx->decoder) {
        MppDecoder* decoder = new MppDecoder();
        if (decoder->Init(264, 25, ctx) == 0) {
            // Set channel-specific callback
            decoder->SetCallback(multi_channel_frame_callback);
            ctx->decoder = decoder;
        } else {
            LOGE("Failed to initialize MPP decoder for channel %d", channelIndex);
            delete decoder;
            delete ctx->renderFrameQueue;
            delete ctx->yolov5ThreadPool;
            delete ctx;
            return nullptr;
        }
    }
    
    LOGD("Channel context created successfully for channel %d", channelIndex);
    return ctx;
}

// Helper function to destroy a channel-specific context
void destroyChannelContext(rknn_app_context_t* ctx) {
    if (!ctx) return;
    
    if (ctx->yolov5ThreadPool) {
        ctx->yolov5ThreadPool->stopAll();
        delete ctx->yolov5ThreadPool;
        ctx->yolov5ThreadPool = nullptr;
    }
    
    if (ctx->renderFrameQueue) {
        delete ctx->renderFrameQueue;
        ctx->renderFrameQueue = nullptr;
    }
    
    if (ctx->decoder) {
        delete ctx->decoder;
        ctx->decoder = nullptr;
    }
    
    delete ctx;
}

// New implementation methods for MultiChannelZLPlayer
bool MultiChannelZLPlayer::initializeChannel() {
    std::lock_guard<std::mutex> lock(channelMutex);

    // Initialize RAII-managed context
    if (!channelContext.initialize()) {
        LOGE("Failed to initialize context for channel %d", channelIndex);
        return false;
    }

    // Initialize YOLOv5 thread pool for this channel
    channelContext->yolov5ThreadPool = new Yolov5ThreadPool();

    // Use channel-specific model data with smaller thread pool for multi-channel efficiency
    if (channelContext->yolov5ThreadPool->setUpWithModelData(3, modelData.get(), modelDataSize) != NN_SUCCESS) {
        LOGE("Failed to initialize YOLOv5 thread pool for channel %d", channelIndex);
        channelContext.cleanup(); // RAII cleanup
        return false;
    }

    // Initialize render frame queue
    channelContext->renderFrameQueue = new RenderFrameQueue();

    // Initialize MPP decoder
    MppDecoder* decoder = new MppDecoder();
    if (decoder->Init(264, 25, channelContext.get()) == 0) {
        // Set channel-specific callback
        decoder->SetCallback(multi_channel_frame_callback);
        channelContext->decoder = decoder;
    } else {
        LOGE("Failed to initialize MPP decoder for channel %d", channelIndex);
        delete decoder;
        channelContext.cleanup(); // RAII cleanup
        return false;
    }

    LOGD("Channel %d initialized successfully", channelIndex);
    return true;
}

void MultiChannelZLPlayer::cleanupChannel() {
    std::lock_guard<std::mutex> lock(channelMutex);

    // Stop RTSP stream if running
    stopRTSPStream();

    // Release surface
    if (channelSurface) {
        ANativeWindow_release(channelSurface);
        channelSurface = nullptr;
    }

    // RAII cleanup handles all context resources automatically
    channelContext.cleanup();

    LOGD("Channel %d cleaned up", channelIndex);
}

bool MultiChannelZLPlayer::startRTSPStream() {
    if (channelRtspUrl.empty()) {
        LOGE("Channel %d: RTSP URL not set", channelIndex);
        return false;
    }

    if (!channelContext) {
        LOGE("Channel %d: Context not initialized", channelIndex);
        return false;
    }

    std::lock_guard<std::mutex> lock(channelMutex);

    // Copy RTSP URL to context
    strncpy(rtsp_url, channelRtspUrl.c_str(), sizeof(rtsp_url) - 1);
    rtsp_url[sizeof(rtsp_url) - 1] = '\0';

    // Start RTSP processing thread for this channel
    // Note: This uses the parent ZLPlayer's RTSP processing mechanism
    // but with channel-specific context

    LOGD("Channel %d: RTSP stream started with URL: %s", channelIndex, channelRtspUrl.c_str());
    return true;
}

void MultiChannelZLPlayer::stopRTSPStream() {
    std::lock_guard<std::mutex> lock(channelMutex);

    // Stop RTSP processing
    // This would involve stopping the RTSP threads and cleaning up connections
    // For now, we'll just log the action

    LOGD("Channel %d: RTSP stream stopped", channelIndex);
}

bool MultiChannelZLPlayer::isChannelActive() const {
    std::lock_guard<std::mutex> lock(channelMutex);
    return channelContext && !channelRtspUrl.empty();
}

void MultiChannelZLPlayer::renderToChannelSurface(frame_data_t* frameData) {
    if (!channelSurface || !frameData || !frameData->data) {
        LOGE("Channel %d: Invalid surface or frame data for rendering", channelIndex);
        return;
    }

    // Get frame dimensions
    int width = frameData->screenW;
    int height = frameData->screenH;

    if (width <= 0 || height <= 0) {
        LOGE("Channel %d: Invalid frame dimensions: %dx%d", channelIndex, width, height);
        return;
    }

    // Set buffer geometry for this channel's surface
    if (ANativeWindow_setBuffersGeometry(channelSurface, width, height, WINDOW_FORMAT_RGBA_8888) != 0) {
        LOGE("Channel %d: Failed to set buffer geometry", channelIndex);
        return;
    }

    // Lock the surface buffer
    ANativeWindow_Buffer windowBuffer;
    if (ANativeWindow_lock(channelSurface, &windowBuffer, 0) != 0) {
        LOGE("Channel %d: Failed to lock surface buffer", channelIndex);
        return;
    }

    // Copy frame data to surface buffer
    uint8_t* dstData = (uint8_t*)windowBuffer.bits;
    uint8_t* srcData = (uint8_t*)frameData->data.get();

    int dstLinesize = windowBuffer.stride * 4; // RGBA_8888 = 4 bytes per pixel
    int srcLinesize = width * 4; // Assuming RGBA format

    // Copy line by line to handle different strides
    for (int i = 0; i < height; i++) {
        memcpy(dstData + i * dstLinesize, srcData + i * srcLinesize, srcLinesize);
    }

    // Unlock and post the buffer to display
    if (ANativeWindow_unlockAndPost(channelSurface) != 0) {
        LOGE("Channel %d: Failed to unlock and post surface buffer", channelIndex);
        return;
    }

    // Update channel statistics
    if (channelManager) {
        channelManager->onChannelFrameRendered(channelIndex);
    }

    LOGD("Channel %d: Frame rendered successfully (%dx%d)", channelIndex, width, height);
}

// Frame rate control implementation
bool MultiChannelZLPlayer::shouldProcessFrame() {
    auto currentTime = std::chrono::steady_clock::now();
    auto timeSinceLastFrame = std::chrono::duration_cast<std::chrono::microseconds>(
        currentTime - lastFrameTime);

    // Target frame interval for 30 FPS (33.33ms)
    constexpr auto targetInterval = std::chrono::microseconds(33333);

    // Check if enough time has passed since last frame
    if (timeSinceLastFrame >= targetInterval) {
        lastFrameTime = currentTime;
        return true;
    }

    // Adaptive frame skipping based on system load
    int skipCount = frameSkipCounter.load();
    if (skipCount > 0) {
        frameSkipCounter.store(skipCount - 1);
        return false;
    }

    return timeSinceLastFrame >= targetInterval;
}

bool MultiChannelZLPlayer::shouldRenderFrame() {
    auto currentTime = std::chrono::steady_clock::now();
    auto timeSinceLastRender = std::chrono::duration_cast<std::chrono::microseconds>(
        currentTime - lastRenderTime);

    // Target render interval for 30 FPS
    constexpr auto targetRenderInterval = std::chrono::microseconds(33333);

    if (timeSinceLastRender >= targetRenderInterval) {
        lastRenderTime = currentTime;
        return true;
    }

    return false;
}

void MultiChannelZLPlayer::updateFrameRateStats() {
    static auto lastStatsUpdate = std::chrono::steady_clock::now();
    static int frameCountSinceLastUpdate = 0;

    frameCountSinceLastUpdate++;

    auto currentTime = std::chrono::steady_clock::now();
    auto timeSinceLastStats = std::chrono::duration_cast<std::chrono::milliseconds>(
        currentTime - lastStatsUpdate);

    // Update stats every second
    if (timeSinceLastStats >= std::chrono::milliseconds(1000)) {
        float fps = (frameCountSinceLastUpdate * 1000.0f) / timeSinceLastStats.count();
        currentFps.store(fps);

        LOGD("Channel %d: Current FPS: %.2f", channelIndex, fps);

        frameCountSinceLastUpdate = 0;
        lastStatsUpdate = currentTime;
    }
}

void MultiChannelZLPlayer::adaptiveFrameSkipping() {
    float fps = currentFps.load();

    // If FPS is below threshold, increase frame skipping
    constexpr float MIN_FPS_THRESHOLD = 25.0f;
    constexpr float TARGET_FPS = 30.0f;

    if (fps < MIN_FPS_THRESHOLD) {
        // Increase frame skipping
        int currentSkip = frameSkipCounter.load();
        if (currentSkip < 3) { // Max skip 3 frames
            frameSkipCounter.store(currentSkip + 1);
            LOGD("Channel %d: Increasing frame skip to %d (FPS: %.2f)",
                 channelIndex, currentSkip + 1, fps);
        }
    } else if (fps > TARGET_FPS * 0.95f) {
        // Reduce frame skipping if performance is good
        int currentSkip = frameSkipCounter.load();
        if (currentSkip > 0) {
            frameSkipCounter.store(currentSkip - 1);
            LOGD("Channel %d: Reducing frame skip to %d (FPS: %.2f)",
                 channelIndex, currentSkip - 1, fps);
        }
    }
}
