#include "MultiChannelFrameCompositor.h"
#include <algorithm>
#include <cstring>
#include <cmath>

MultiChannelFrameCompositor::MultiChannelFrameCompositor()
    : compositionRunning(false), eventListener(nullptr), 
      gpuAccelerationEnabled(false), gpuContext(nullptr) {
    
    // Initialize default configuration
    config.mode = INDIVIDUAL_SURFACES;
    config.layout = QUAD;
    config.outputWidth = 1920;
    config.outputHeight = 1080;
    config.outputFormat = MPP_FMT_RGBA8888;
    config.enableBlending = true;
    config.enableScaling = true;
    config.backgroundColor = 0xFF000000;
    
    lastMetricsUpdate = std::chrono::steady_clock::now();
    
    LOGD("MultiChannelFrameCompositor created");
}

MultiChannelFrameCompositor::~MultiChannelFrameCompositor() {
    cleanup();
    LOGD("MultiChannelFrameCompositor destroyed");
}

bool MultiChannelFrameCompositor::initialize(const CompositionConfig& newConfig) {
    std::lock_guard<std::mutex> lock(configMutex);
    
    config = newConfig;
    
    // Initialize buffer pool
    initializeBufferPool();
    
    // Initialize GPU acceleration if enabled
    if (config.mode == UNIFIED_COMPOSITION || config.mode == HYBRID_COMPOSITION) {
        if (gpuAccelerationEnabled) {
            initializeGpuAcceleration();
        }
    }
    
    // Calculate initial viewports
    calculateViewportsForLayout(config.layout);
    
    LOGD("MultiChannelFrameCompositor initialized: mode=%d, layout=%d, size=%dx%d",
         config.mode, config.layout, config.outputWidth, config.outputHeight);
    
    return true;
}

void MultiChannelFrameCompositor::cleanup() {
    stopComposition();
    
    // Cleanup GPU resources
    if (gpuAccelerationEnabled) {
        cleanupGpuAcceleration();
    }
    
    // Cleanup buffer pool
    cleanupBufferPool();
    
    // Clear queues
    {
        std::lock_guard<std::mutex> lock(inputQueueMutex);
        while (!inputQueue.empty()) {
            inputQueue.pop();
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(outputQueueMutex);
        while (!outputQueue.empty()) {
            outputQueue.pop();
        }
    }
    
    // Clear channel data
    {
        std::lock_guard<std::mutex> lock(channelsMutex);
        channelViewports.clear();
        latestChannelFrames.clear();
    }
    
    LOGD("MultiChannelFrameCompositor cleanup completed");
}

void MultiChannelFrameCompositor::startComposition() {
    if (compositionRunning) {
        LOGW("Composition already running");
        return;
    }
    
    compositionRunning = true;
    compositionThread = std::thread(&MultiChannelFrameCompositor::compositionLoop, this);
    
    LOGD("Composition started");
}

void MultiChannelFrameCompositor::stopComposition() {
    if (!compositionRunning) {
        return;
    }
    
    compositionRunning = false;
    compositionCv.notify_all();
    
    if (compositionThread.joinable()) {
        compositionThread.join();
    }
    
    LOGD("Composition stopped");
}

bool MultiChannelFrameCompositor::addChannel(int channelIndex, const ChannelViewport& viewport) {
    if (!validateViewport(viewport)) {
        LOGE("Invalid viewport for channel %d", channelIndex);
        return false;
    }
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    channelViewports[channelIndex] = viewport;
    
    LOGD("Added channel %d to compositor", channelIndex);
    return true;
}

bool MultiChannelFrameCompositor::removeChannel(int channelIndex) {
    std::lock_guard<std::mutex> lock(channelsMutex);
    
    auto it = channelViewports.find(channelIndex);
    if (it == channelViewports.end()) {
        LOGW("Channel %d not found in compositor", channelIndex);
        return false;
    }
    
    channelViewports.erase(it);
    latestChannelFrames.erase(channelIndex);
    
    LOGD("Removed channel %d from compositor", channelIndex);
    return true;
}

bool MultiChannelFrameCompositor::submitChannelFrame(int channelIndex, std::shared_ptr<frame_data_t> frameData) {
    if (!frameData || !frameData->data) {
        LOGE("Invalid frame data for channel %d", channelIndex);
        return false;
    }
    
    // Update latest frame for this channel
    {
        std::lock_guard<std::mutex> lock(channelsMutex);
        latestChannelFrames[channelIndex] = frameData;
    }
    
    // Add to input queue for processing
    {
        std::lock_guard<std::mutex> lock(inputQueueMutex);
        
        // Limit queue size to prevent memory buildup
        if (inputQueue.size() > 20) {
            inputQueue.pop(); // Remove oldest frame
            metrics.framesDropped++;
        }
        
        inputQueue.push(std::make_pair(channelIndex, frameData));
    }
    
    // Notify composition thread
    compositionCv.notify_one();
    
    return true;
}

void MultiChannelFrameCompositor::compositionLoop() {
    LOGD("Composition loop started");
    
    while (compositionRunning) {
        std::unique_lock<std::mutex> lock(compositionMutex);
        
        // Wait for frames or stop signal
        compositionCv.wait_for(lock, std::chrono::milliseconds(33), // ~30 FPS
                              [this] { return !compositionRunning || !inputQueue.empty(); });
        
        if (!compositionRunning) break;
        
        lock.unlock();
        
        // Process composition based on mode
        bool composed = false;
        auto startTime = std::chrono::steady_clock::now();
        
        switch (config.mode) {
            case INDIVIDUAL_SURFACES:
                composed = composeIndividualSurfaces();
                break;
            case UNIFIED_COMPOSITION:
                composed = composeUnifiedFrame();
                break;
            case HYBRID_COMPOSITION:
                composed = composeHybridFrame();
                break;
        }
        
        if (composed) {
            auto endTime = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
            
            // Update metrics
            metrics.framesComposed++;
            float frameTime = duration.count() / 1000.0f; // Convert to milliseconds
            metrics.averageCompositionTime = (metrics.averageCompositionTime * 0.9f) + (frameTime * 0.1f);
            
            // Update FPS
            auto now = std::chrono::steady_clock::now();
            auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastMetricsUpdate);
            if (timeSinceLastUpdate.count() >= 1000) { // Update every second
                metrics.compositionFps = metrics.framesComposed.load();
                metrics.framesComposed = 0;
                lastMetricsUpdate = now;
                
                if (eventListener) {
                    notifyPerformanceUpdate();
                }
            }
        }
    }
    
    LOGD("Composition loop ended");
}

bool MultiChannelFrameCompositor::composeIndividualSurfaces() {
    // In individual surfaces mode, we just process frames for each channel separately
    // This is the most efficient mode for multi-surface rendering
    
    std::lock_guard<std::mutex> inputLock(inputQueueMutex);
    
    if (inputQueue.empty()) {
        return false;
    }
    
    // Process all available frames
    int processedFrames = 0;
    while (!inputQueue.empty() && processedFrames < 16) { // Limit processing per cycle
        auto frameInfo = inputQueue.front();
        inputQueue.pop();
        
        int channelIndex = frameInfo.first;
        auto frameData = frameInfo.second;
        
        if (processChannelFrame(channelIndex, frameData)) {
            processedFrames++;
        }
    }
    
    return processedFrames > 0;
}

bool MultiChannelFrameCompositor::composeUnifiedFrame() {
    // Create a single composite frame containing all visible channels
    
    auto compositeBuffer = acquireBuffer();
    if (!compositeBuffer) {
        LOGE("Failed to acquire composite buffer");
        return false;
    }
    
    // Clear background
    int bufferSize = calculateBufferSize(config.outputWidth, config.outputHeight, config.outputFormat);
    clearBuffer(compositeBuffer.get(), bufferSize, config.backgroundColor);
    
    CompositeFrame compositeFrame;
    compositeFrame.data = compositeBuffer;
    compositeFrame.width = config.outputWidth;
    compositeFrame.height = config.outputHeight;
    compositeFrame.stride = config.outputWidth * 4; // Assuming RGBA
    compositeFrame.format = config.outputFormat;
    
    // Compose all visible channels
    std::lock_guard<std::mutex> lock(channelsMutex);
    
    for (const auto& pair : channelViewports) {
        int channelIndex = pair.first;
        const ChannelViewport& viewport = pair.second;
        
        if (!viewport.visible) continue;
        
        auto it = latestChannelFrames.find(channelIndex);
        if (it == latestChannelFrames.end()) continue;
        
        auto frameData = it->second;
        if (!frameData || !frameData->data) continue;
        
        // Scale and blend channel frame into composite
        if (config.enableScaling) {
            if (gpuAccelerationEnabled) {
                gpuScaleFrame(frameData.get(), compositeBuffer.get(), viewport);
            } else {
                scaleFrame(frameData.get(), compositeBuffer.get(), viewport);
            }
        } else {
            // Direct copy without scaling
            copyFrameData(frameData.get(), compositeBuffer.get(), compositeFrame.stride);
        }
        
        compositeFrame.includedChannels.push_back(channelIndex);
    }
    
    // Add to output queue
    {
        std::lock_guard<std::mutex> outputLock(outputQueueMutex);
        
        if (outputQueue.size() > 5) {
            outputQueue.pop(); // Remove oldest frame
        }
        
        outputQueue.push(compositeFrame);
    }
    
    // Notify listener
    if (eventListener) {
        notifyCompositeFrameReady(compositeFrame);
    }
    
    return true;
}

bool MultiChannelFrameCompositor::composeHybridFrame() {
    // Hybrid mode: use individual surfaces for primary channels,
    // unified composition for secondary channels
    
    // For now, implement as unified composition
    // TODO: Implement actual hybrid logic based on channel priorities
    return composeUnifiedFrame();
}

bool MultiChannelFrameCompositor::processChannelFrame(int channelIndex, std::shared_ptr<frame_data_t> frameData) {
    // Process individual channel frame
    // This could involve scaling, color correction, etc.
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    
    auto it = channelViewports.find(channelIndex);
    if (it == channelViewports.end()) {
        return false;
    }
    
    const ChannelViewport& viewport = it->second;
    if (!viewport.visible) {
        return false;
    }
    
    // Apply any channel-specific processing here
    // For now, just validate the frame
    if (!frameData || !frameData->data) {
        return false;
    }
    
    LOGD("Processed frame for channel %d: %dx%d", channelIndex, frameData->screenW, frameData->screenH);
    return true;
}

void MultiChannelFrameCompositor::calculateViewportsForLayout(LayoutMode layout) {
    std::lock_guard<std::mutex> lock(channelsMutex);
    
    int rows, cols;
    switch (layout) {
        case SINGLE:
            rows = cols = 1;
            break;
        case QUAD:
            rows = cols = 2;
            break;
        case NINE:
            rows = cols = 3;
            break;
        case SIXTEEN:
            rows = cols = 4;
            break;
        default:
            rows = cols = 2;
            break;
    }
    
    int cellWidth = config.outputWidth / cols;
    int cellHeight = config.outputHeight / rows;
    
    // Update viewports for existing channels
    for (auto& pair : channelViewports) {
        int channelIndex = pair.first;
        ChannelViewport& viewport = pair.second;
        
        int row = channelIndex / cols;
        int col = channelIndex % cols;
        
        viewport.x = col * cellWidth;
        viewport.y = row * cellHeight;
        viewport.width = cellWidth;
        viewport.height = cellHeight;
        viewport.scaleX = static_cast<float>(cellWidth) / config.outputWidth;
        viewport.scaleY = static_cast<float>(cellHeight) / config.outputHeight;
        viewport.needsUpdate = true;
    }
    
    LOGD("Calculated viewports for layout %d: %dx%d grid, cell size %dx%d",
         layout, rows, cols, cellWidth, cellHeight);
}

std::shared_ptr<uint8_t> MultiChannelFrameCompositor::acquireBuffer() {
    std::lock_guard<std::mutex> lock(bufferPoolMutex);
    
    if (!bufferPool.empty()) {
        auto buffer = bufferPool.back();
        bufferPool.pop_back();
        return buffer;
    }
    
    // Create new buffer if pool is empty
    int bufferSize = calculateBufferSize(config.outputWidth, config.outputHeight, config.outputFormat);
    auto buffer = std::shared_ptr<uint8_t>(new uint8_t[bufferSize], std::default_delete<uint8_t[]>());
    
    return buffer;
}

void MultiChannelFrameCompositor::releaseBuffer(std::shared_ptr<uint8_t> buffer) {
    if (!buffer) return;
    
    std::lock_guard<std::mutex> lock(bufferPoolMutex);
    
    if (bufferPool.size() < BUFFER_POOL_SIZE) {
        bufferPool.push_back(buffer);
    }
    // If pool is full, buffer will be automatically deleted when shared_ptr goes out of scope
}

void MultiChannelFrameCompositor::initializeBufferPool() {
    std::lock_guard<std::mutex> lock(bufferPoolMutex);
    
    bufferPool.clear();
    bufferPool.reserve(BUFFER_POOL_SIZE);
    
    int bufferSize = calculateBufferSize(config.outputWidth, config.outputHeight, config.outputFormat);
    
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        auto buffer = std::shared_ptr<uint8_t>(new uint8_t[bufferSize], std::default_delete<uint8_t[]>());
        bufferPool.push_back(buffer);
    }
    
    LOGD("Initialized buffer pool with %d buffers of %d bytes each", BUFFER_POOL_SIZE, bufferSize);
}

void MultiChannelFrameCompositor::cleanupBufferPool() {
    std::lock_guard<std::mutex> lock(bufferPoolMutex);
    bufferPool.clear();
    LOGD("Buffer pool cleaned up");
}

int MultiChannelFrameCompositor::calculateBufferSize(int width, int height, int format) const {
    int bytesPerPixel = 4; // Assuming RGBA format
    return width * height * bytesPerPixel;
}

void MultiChannelFrameCompositor::clearBuffer(uint8_t* buffer, int size, uint32_t color) {
    if (!buffer) return;
    
    uint32_t* pixels = reinterpret_cast<uint32_t*>(buffer);
    int pixelCount = size / 4;
    
    for (int i = 0; i < pixelCount; i++) {
        pixels[i] = color;
    }
}

bool MultiChannelFrameCompositor::validateViewport(const ChannelViewport& viewport) const {
    return viewport.width > 0 && viewport.height > 0 &&
           viewport.x >= 0 && viewport.y >= 0 &&
           viewport.x + viewport.width <= config.outputWidth &&
           viewport.y + viewport.height <= config.outputHeight;
}

// Event notification methods
void MultiChannelFrameCompositor::notifyCompositeFrameReady(const CompositeFrame& frame) {
    if (eventListener) {
        eventListener->onCompositeFrameReady(frame);
    }
}

void MultiChannelFrameCompositor::notifyPerformanceUpdate() {
    if (eventListener) {
        eventListener->onPerformanceUpdate(metrics);
    }
}

void MultiChannelFrameCompositor::handleCompositionError(int errorCode, const std::string& message) {
    LOGE("Composition error %d: %s", errorCode, message.c_str());
    if (eventListener) {
        eventListener->onCompositionError(errorCode, message);
    }
}

// Additional methods implementation
bool MultiChannelFrameCompositor::scaleFrame(const frame_data_t* src, uint8_t* dst, const ChannelViewport& viewport) {
    if (!src || !src->data || !dst) {
        return false;
    }

    // Simple bilinear scaling implementation
    uint8_t* srcData = reinterpret_cast<uint8_t*>(src->data.get());
    int srcWidth = src->screenW;
    int srcHeight = src->screenH;
    int dstWidth = viewport.width;
    int dstHeight = viewport.height;

    float xRatio = static_cast<float>(srcWidth) / dstWidth;
    float yRatio = static_cast<float>(srcHeight) / dstHeight;

    // Calculate destination offset
    uint8_t* dstPtr = dst + (viewport.y * config.outputWidth + viewport.x) * 4;

    for (int y = 0; y < dstHeight; y++) {
        for (int x = 0; x < dstWidth; x++) {
            int srcX = static_cast<int>(x * xRatio);
            int srcY = static_cast<int>(y * yRatio);

            // Clamp to source bounds
            srcX = std::min(srcX, srcWidth - 1);
            srcY = std::min(srcY, srcHeight - 1);

            // Copy pixel (assuming RGBA format)
            uint8_t* srcPixel = srcData + (srcY * srcWidth + srcX) * 4;
            uint8_t* dstPixel = dstPtr + (y * config.outputWidth + x) * 4;

            dstPixel[0] = srcPixel[0]; // R
            dstPixel[1] = srcPixel[1]; // G
            dstPixel[2] = srcPixel[2]; // B
            dstPixel[3] = srcPixel[3]; // A
        }
    }

    return true;
}

bool MultiChannelFrameCompositor::blendFrame(const frame_data_t* src, uint8_t* dst, const ChannelViewport& viewport, float alpha) {
    if (!src || !src->data || !dst || alpha <= 0.0f) {
        return false;
    }

    uint8_t* srcData = reinterpret_cast<uint8_t*>(src->data.get());
    int srcWidth = src->screenW;
    int srcHeight = src->screenH;

    // Calculate scaling factors
    float xScale = static_cast<float>(srcWidth) / viewport.width;
    float yScale = static_cast<float>(srcHeight) / viewport.height;

    uint8_t alphaValue = static_cast<uint8_t>(alpha * 255);
    uint8_t invAlpha = 255 - alphaValue;

    for (int y = 0; y < viewport.height; y++) {
        for (int x = 0; x < viewport.width; x++) {
            int srcX = static_cast<int>(x * xScale);
            int srcY = static_cast<int>(y * yScale);

            if (srcX >= srcWidth || srcY >= srcHeight) continue;

            uint8_t* srcPixel = srcData + (srcY * srcWidth + srcX) * 4;
            uint8_t* dstPixel = dst + ((viewport.y + y) * config.outputWidth + (viewport.x + x)) * 4;

            // Alpha blending
            dstPixel[0] = (srcPixel[0] * alphaValue + dstPixel[0] * invAlpha) / 255;
            dstPixel[1] = (srcPixel[1] * alphaValue + dstPixel[1] * invAlpha) / 255;
            dstPixel[2] = (srcPixel[2] * alphaValue + dstPixel[2] * invAlpha) / 255;
            dstPixel[3] = std::max(srcPixel[3], dstPixel[3]); // Max alpha
        }
    }

    return true;
}

bool MultiChannelFrameCompositor::copyFrameData(const frame_data_t* src, uint8_t* dst, int dstStride) {
    if (!src || !src->data || !dst) {
        return false;
    }

    uint8_t* srcData = reinterpret_cast<uint8_t*>(src->data.get());
    int srcWidth = src->screenW;
    int srcHeight = src->screenH;
    int srcStride = srcWidth * 4; // Assuming RGBA

    int copyWidth = std::min(srcWidth * 4, dstStride);
    int copyHeight = std::min(srcHeight, config.outputHeight);

    for (int y = 0; y < copyHeight; y++) {
        memcpy(dst + y * dstStride, srcData + y * srcStride, copyWidth);
    }

    return true;
}

MultiChannelFrameCompositor::CompositeFrame MultiChannelFrameCompositor::getCompositeFrame() {
    std::lock_guard<std::mutex> lock(outputQueueMutex);

    if (outputQueue.empty()) {
        return CompositeFrame(); // Return empty frame
    }

    CompositeFrame frame = outputQueue.front();
    outputQueue.pop();

    return frame;
}

bool MultiChannelFrameCompositor::hasCompositeFrame() const {
    std::lock_guard<std::mutex> lock(outputQueueMutex);
    return !outputQueue.empty();
}

void MultiChannelFrameCompositor::setLayoutMode(LayoutMode layout) {
    std::lock_guard<std::mutex> lock(configMutex);

    if (config.layout != layout) {
        config.layout = layout;
        calculateViewportsForLayout(layout);
        LOGD("Layout mode changed to %d", layout);
    }
}

void MultiChannelFrameCompositor::setCompositionMode(CompositionMode mode) {
    std::lock_guard<std::mutex> lock(configMutex);

    if (config.mode != mode) {
        config.mode = mode;
        LOGD("Composition mode changed to %d", mode);
    }
}

MultiChannelFrameCompositor::CompositionMetrics MultiChannelFrameCompositor::getMetrics() const {
    CompositionMetrics result;
    result.framesComposed = metrics.framesComposed.load();
    result.framesDropped = metrics.framesDropped.load();
    result.averageCompositionTime = metrics.averageCompositionTime.load();
    result.compositionFps = metrics.compositionFps.load();
    result.memoryUsage = metrics.memoryUsage.load();
    return result;
}

void MultiChannelFrameCompositor::resetMetrics() {
    metrics.framesComposed = 0;
    metrics.framesDropped = 0;
    metrics.averageCompositionTime = 0.0f;
    metrics.compositionFps = 0.0f;
    metrics.lastUpdate = std::chrono::steady_clock::now();

    LOGD("Composition metrics reset");
}

std::string MultiChannelFrameCompositor::generatePerformanceReport() const {
    std::ostringstream report;

    report << "=== Multi-Channel Frame Compositor Performance Report ===\n";
    report << "Composition Mode: " << config.mode << "\n";
    report << "Layout Mode: " << config.layout << "\n";
    report << "Output Resolution: " << config.outputWidth << "x" << config.outputHeight << "\n";
    report << "Frames Composed: " << metrics.framesComposed.load() << "\n";
    report << "Frames Dropped: " << metrics.framesDropped.load() << "\n";
    report << "Composition FPS: " << metrics.compositionFps.load() << "\n";
    report << "Average Composition Time: " << metrics.averageCompositionTime.load() << "ms\n";
    report << "Memory Usage: " << metrics.memoryUsage.load() / (1024 * 1024) << "MB\n";
    report << "GPU Acceleration: " << (gpuAccelerationEnabled ? "Enabled" : "Disabled") << "\n";

    {
        std::lock_guard<std::mutex> lock(channelsMutex);
        report << "Active Channels: " << channelViewports.size() << "\n";

        for (const auto& pair : channelViewports) {
            const ChannelViewport& viewport = pair.second;
            report << "  Channel " << pair.first << ": "
                   << viewport.width << "x" << viewport.height
                   << " at (" << viewport.x << "," << viewport.y << ")"
                   << " visible=" << (viewport.visible ? "yes" : "no") << "\n";
        }
    }

    return report.str();
}

// GPU acceleration stubs (platform-specific implementation needed)
bool MultiChannelFrameCompositor::initializeGpuAcceleration() {
    // TODO: Implement platform-specific GPU initialization
    LOGD("GPU acceleration initialization (stub)");
    return false;
}

void MultiChannelFrameCompositor::cleanupGpuAcceleration() {
    // TODO: Implement platform-specific GPU cleanup
    LOGD("GPU acceleration cleanup (stub)");
}

bool MultiChannelFrameCompositor::gpuScaleFrame(const frame_data_t* src, uint8_t* dst, const ChannelViewport& viewport) {
    // TODO: Implement GPU-accelerated scaling
    // For now, fall back to CPU scaling
    return scaleFrame(src, dst, viewport);
}

bool MultiChannelFrameCompositor::gpuBlendFrame(const frame_data_t* src, uint8_t* dst, const ChannelViewport& viewport, float alpha) {
    // TODO: Implement GPU-accelerated blending
    // For now, fall back to CPU blending
    return blendFrame(src, dst, viewport, alpha);
}

// FrameCompositionUtils implementation
bool FrameCompositionUtils::bilinearScale(const uint8_t* src, uint8_t* dst,
                                         int srcW, int srcH, int dstW, int dstH, int channels) {
    if (!src || !dst || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
        return false;
    }

    float xRatio = static_cast<float>(srcW) / dstW;
    float yRatio = static_cast<float>(srcH) / dstH;

    for (int y = 0; y < dstH; y++) {
        for (int x = 0; x < dstW; x++) {
            float srcX = x * xRatio;
            float srcY = y * yRatio;

            int x1 = static_cast<int>(srcX);
            int y1 = static_cast<int>(srcY);
            int x2 = std::min(x1 + 1, srcW - 1);
            int y2 = std::min(y1 + 1, srcH - 1);

            float dx = srcX - x1;
            float dy = srcY - y1;

            for (int c = 0; c < channels; c++) {
                float p1 = src[(y1 * srcW + x1) * channels + c];
                float p2 = src[(y1 * srcW + x2) * channels + c];
                float p3 = src[(y2 * srcW + x1) * channels + c];
                float p4 = src[(y2 * srcW + x2) * channels + c];

                float interpolated = p1 * (1 - dx) * (1 - dy) +
                                   p2 * dx * (1 - dy) +
                                   p3 * (1 - dx) * dy +
                                   p4 * dx * dy;

                dst[(y * dstW + x) * channels + c] = static_cast<uint8_t>(interpolated);
            }
        }
    }

    return true;
}

bool FrameCompositionUtils::alphaBlend(const uint8_t* src, uint8_t* dst, int width, int height, float alpha) {
    if (!src || !dst || alpha < 0.0f || alpha > 1.0f) {
        return false;
    }

    uint8_t alphaValue = static_cast<uint8_t>(alpha * 255);
    uint8_t invAlpha = 255 - alphaValue;

    for (int i = 0; i < width * height * 4; i += 4) {
        dst[i] = (src[i] * alphaValue + dst[i] * invAlpha) / 255;         // R
        dst[i + 1] = (src[i + 1] * alphaValue + dst[i + 1] * invAlpha) / 255; // G
        dst[i + 2] = (src[i + 2] * alphaValue + dst[i + 2] * invAlpha) / 255; // B
        dst[i + 3] = std::max(src[i + 3], dst[i + 3]);                   // A
    }

    return true;
}
