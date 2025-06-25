#include "MultiSurfaceRenderer.h"
#include <algorithm>

MultiSurfaceRenderer::MultiSurfaceRenderer(int maxSurfaces, int threadCount)
    : maxSurfaces(maxSurfaces), renderThreadCount(threadCount),
      maxRenderLoad(80.0f), shouldStop(false), eventListener(nullptr),
      systemRenderLoad(0.0f), activeSurfaceCount(0) {
    
    // Start render threads
    for (int i = 0; i < renderThreadCount; ++i) {
        renderThreads.emplace_back(&MultiSurfaceRenderer::renderThreadLoop, this, i);
    }
    
    // Start performance monitor thread
    performanceMonitorThread = std::thread(&MultiSurfaceRenderer::performanceMonitorLoop, this);
    
    LOGD("MultiSurfaceRenderer initialized with %d max surfaces, %d threads", 
         maxSurfaces, threadCount);
}

MultiSurfaceRenderer::~MultiSurfaceRenderer() {
    cleanup();
}

bool MultiSurfaceRenderer::addSurface(int channelIndex, ANativeWindow* surface) {
    if (!surface) {
        LOGE("Cannot add null surface for channel %d", channelIndex);
        return false;
    }
    
    auto lock = lockSurfaces();
    
    if (surfaces.size() >= static_cast<size_t>(maxSurfaces)) {
        LOGE("Cannot add surface: maximum surfaces (%d) reached", maxSurfaces);
        return false;
    }
    
    // Remove existing surface if present
    auto it = surfaces.find(channelIndex);
    if (it != surfaces.end()) {
        LOGW("Replacing existing surface for channel %d", channelIndex);
        removeSurface(channelIndex);
    }
    
    // Create new surface info
    auto surfaceInfo = std::make_unique<SurfaceInfo>(channelIndex, surface);
    
    // Get surface properties
    int width = ANativeWindow_getWidth(surface);
    int height = ANativeWindow_getHeight(surface);
    int format = ANativeWindow_getFormat(surface);
    
    surfaceInfo->width = width;
    surfaceInfo->height = height;
    surfaceInfo->format = format;
    
    surfaces[channelIndex] = std::move(surfaceInfo);
    updateSurfaceState(channelIndex, ACTIVE);
    activeSurfaceCount++;
    
    if (eventListener) {
        eventListener->onSurfaceReady(channelIndex);
    }
    
    LOGD("Added surface for channel %d (%dx%d, format: %d)", channelIndex, width, height, format);
    return true;
}

bool MultiSurfaceRenderer::removeSurface(int channelIndex) {
    auto lock = lockSurfaces();
    
    auto it = surfaces.find(channelIndex);
    if (it == surfaces.end()) {
        return false;
    }
    
    updateSurfaceState(channelIndex, INACTIVE);
    surfaces.erase(it);
    activeSurfaceCount--;
    
    if (eventListener) {
        eventListener->onSurfaceDestroyed(channelIndex);
    }
    
    LOGD("Removed surface for channel %d", channelIndex);
    return true;
}

bool MultiSurfaceRenderer::queueFrame(int channelIndex, std::shared_ptr<frame_data_t> frameData) {
    if (!frameData) {
        return false;
    }
    
    auto lock = lockSurfaces();
    
    SurfaceInfo* surfaceInfo = getSurfaceInfo(channelIndex);
    if (!surfaceInfo || surfaceInfo->state != ACTIVE) {
        return false;
    }
    
    // Check if we should render this frame based on FPS control
    if (!shouldRenderFrame(surfaceInfo)) {
        surfaceInfo->droppedFrames++;
        return true; // Frame dropped but operation successful
    }
    
    // Queue frame for rendering
    if (surfaceInfo->renderQueue->size() > 5) {
        // Queue is full, drop oldest frame
        surfaceInfo->renderQueue->pop();
        surfaceInfo->droppedFrames++;
    }
    
    surfaceInfo->renderQueue->push(frameData);
    surfaceInfo->frameCount++;
    
    // Add to render queue
    lock.unlock();
    auto queueLock = lockRenderQueue();
    renderQueue.push(channelIndex);
    renderQueueCv.notify_one();
    
    return true;
}

bool MultiSurfaceRenderer::renderFrame(int channelIndex) {
    auto lock = lockSurfaces();
    
    SurfaceInfo* surfaceInfo = getSurfaceInfo(channelIndex);
    if (!surfaceInfo || surfaceInfo->state != ACTIVE) {
        return false;
    }
    
    auto frameData = surfaceInfo->renderQueue->pop();
    if (!frameData) {
        return false;
    }
    
    lock.unlock();
    
    // Render frame to surface
    bool success = renderFrameToSurface(surfaceInfo, frameData.get());
    
    if (success) {
        surfaceInfo->renderCount++;
        surfaceInfo->lastRenderTime = std::chrono::steady_clock::now();
        
        if (eventListener) {
            eventListener->onFrameRendered(channelIndex, surfaceInfo->width, surfaceInfo->height);
        }
    } else {
        handleRenderError(channelIndex, "Frame rendering failed");
    }
    
    return success;
}

bool MultiSurfaceRenderer::renderFrameToSurface(SurfaceInfo* surfaceInfo, frame_data_t* frameData) {
    if (!surfaceInfo || !frameData || !surfaceInfo->surface) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(surfaceInfo->surfaceMutex);
    
    ANativeWindow* window = surfaceInfo->surface;
    
    // Set buffer geometry
    int ret = ANativeWindow_setBuffersGeometry(window, frameData->screenW, frameData->screenH, 
                                              WINDOW_FORMAT_RGBA_8888);
    if (ret != 0) {
        LOGE("Failed to set buffer geometry for channel %d: %d", surfaceInfo->channelIndex, ret);
        return false;
    }
    
    // Lock buffer
    ANativeWindow_Buffer buffer;
    ret = ANativeWindow_lock(window, &buffer, nullptr);
    if (ret != 0) {
        LOGE("Failed to lock window buffer for channel %d: %d", surfaceInfo->channelIndex, ret);
        return false;
    }
    
    // Copy frame data to buffer
    if (buffer.bits && frameData->data) {
        int copyWidth = std::min(buffer.width, frameData->screenW);
        int copyHeight = std::min(buffer.height, frameData->screenH);
        
        uint8_t* src = reinterpret_cast<uint8_t*>(frameData->data.get());
        uint8_t* dst = static_cast<uint8_t*>(buffer.bits);
        
        for (int y = 0; y < copyHeight; y++) {
            memcpy(dst + y * buffer.stride * 4, 
                   src + y * frameData->screenW * 4, 
                   copyWidth * 4);
        }
    }
    
    // Unlock and post buffer
    ret = ANativeWindow_unlockAndPost(window);
    if (ret != 0) {
        LOGE("Failed to unlock and post buffer for channel %d: %d", surfaceInfo->channelIndex, ret);
        return false;
    }
    
    return true;
}

void MultiSurfaceRenderer::renderThreadLoop(int threadId) {
    LOGD("Render thread %d started", threadId);
    
    while (!shouldStop) {
        std::unique_lock<std::mutex> lock(renderQueueMutex);
        renderQueueCv.wait(lock, [this] { return !renderQueue.empty() || shouldStop; });
        
        if (shouldStop) break;
        
        if (!renderQueue.empty()) {
            int channelIndex = renderQueue.front();
            renderQueue.pop();
            lock.unlock();
            
            processSurfaceRender(channelIndex);
        }
    }
    
    LOGD("Render thread %d stopped", threadId);
}

void MultiSurfaceRenderer::processSurfaceRender(int channelIndex) {
    renderFrame(channelIndex);
}

void MultiSurfaceRenderer::performanceMonitorLoop() {
    LOGD("Performance monitor thread started");
    
    while (!shouldStop) {
        updateSystemLoad();
        
        auto lock = lockSurfaces();
        for (auto& pair : surfaces) {
            updateSurfaceStats(pair.second.get());
        }
        lock.unlock();
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    LOGD("Performance monitor thread stopped");
}

void MultiSurfaceRenderer::updateSurfaceStats(SurfaceInfo* surfaceInfo) {
    if (!surfaceInfo) return;
    
    auto now = std::chrono::steady_clock::now();
    auto timeSinceCreation = std::chrono::duration_cast<std::chrono::seconds>(
        now - surfaceInfo->creationTime);
    
    if (timeSinceCreation.count() > 0) {
        surfaceInfo->currentFps = static_cast<float>(surfaceInfo->renderCount) / timeSinceCreation.count();
    }
}

void MultiSurfaceRenderer::updateSystemLoad() {
    // Calculate system render load based on active surfaces and their performance
    float totalLoad = 0.0f;
    int activeSurfaces = 0;
    
    auto lock = lockSurfaces();
    for (const auto& pair : surfaces) {
        if (pair.second->state == ACTIVE) {
            activeSurfaces++;
            // Estimate load based on FPS and dropped frames
            float surfaceLoad = (pair.second->currentFps / pair.second->targetFps) * 100.0f;
            if (pair.second->droppedFrames > 0) {
                surfaceLoad += 20.0f; // Penalty for dropped frames
            }
            totalLoad += surfaceLoad;
        }
    }
    
    systemRenderLoad.store(activeSurfaces > 0 ? totalLoad / activeSurfaces : 0.0f);
}

bool MultiSurfaceRenderer::shouldRenderFrame(SurfaceInfo* surfaceInfo) const {
    if (!surfaceInfo) return false;
    
    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastRender = std::chrono::duration_cast<std::chrono::microseconds>(
        now - surfaceInfo->lastRenderTime);
    
    // Calculate target frame interval
    float targetInterval = 1000000.0f / surfaceInfo->targetFps; // microseconds
    
    return timeSinceLastRender.count() >= targetInterval;
}

void MultiSurfaceRenderer::adaptiveFrameSkipping(SurfaceInfo* surfaceInfo) {
    if (!surfaceInfo) return;
    
    // Implement adaptive frame skipping based on performance
    if (systemRenderLoad.load() > maxRenderLoad) {
        // Reduce target FPS when system is overloaded
        surfaceInfo->targetFps = std::max(15.0f, surfaceInfo->targetFps * 0.9f);
    } else if (systemRenderLoad.load() < maxRenderLoad * 0.7f) {
        // Increase target FPS when system has capacity
        surfaceInfo->targetFps = std::min(30.0f, surfaceInfo->targetFps * 1.1f);
    }
}

// Public interface implementations
bool MultiSurfaceRenderer::isSurfaceReady(int channelIndex) const {
    auto lock = const_cast<MultiSurfaceRenderer*>(this)->lockSurfaces();
    
    const SurfaceInfo* surfaceInfo = getSurfaceInfo(channelIndex);
    return surfaceInfo && surfaceInfo->state == ACTIVE;
}

void MultiSurfaceRenderer::setSurfaceFormat(int channelIndex, int width, int height, int format) {
    auto lock = lockSurfaces();
    
    SurfaceInfo* surfaceInfo = getSurfaceInfo(channelIndex);
    if (surfaceInfo) {
        surfaceInfo->width = width;
        surfaceInfo->height = height;
        surfaceInfo->format = format;
        LOGD("Updated surface format for channel %d: %dx%d, format: %d", 
             channelIndex, width, height, format);
    }
}

void MultiSurfaceRenderer::setTargetFps(int channelIndex, float fps) {
    auto lock = lockSurfaces();
    
    SurfaceInfo* surfaceInfo = getSurfaceInfo(channelIndex);
    if (surfaceInfo) {
        surfaceInfo->targetFps = fps;
        LOGD("Set target FPS for channel %d: %.1f", channelIndex, fps);
    }
}

void MultiSurfaceRenderer::pauseSurface(int channelIndex) {
    updateSurfaceState(channelIndex, PAUSED);
}

void MultiSurfaceRenderer::resumeSurface(int channelIndex) {
    updateSurfaceState(channelIndex, ACTIVE);
}

MultiSurfaceRenderer::RenderState MultiSurfaceRenderer::getSurfaceState(int channelIndex) const {
    auto lock = const_cast<MultiSurfaceRenderer*>(this)->lockSurfaces();
    
    const SurfaceInfo* surfaceInfo = getSurfaceInfo(channelIndex);
    return surfaceInfo ? surfaceInfo->state : INACTIVE;
}

float MultiSurfaceRenderer::getSurfaceFps(int channelIndex) const {
    auto lock = const_cast<MultiSurfaceRenderer*>(this)->lockSurfaces();
    
    const SurfaceInfo* surfaceInfo = getSurfaceInfo(channelIndex);
    return surfaceInfo ? surfaceInfo->currentFps : 0.0f;
}

// Utility methods
MultiSurfaceRenderer::SurfaceInfo* MultiSurfaceRenderer::getSurfaceInfo(int channelIndex) {
    auto it = surfaces.find(channelIndex);
    return (it != surfaces.end()) ? it->second.get() : nullptr;
}

const MultiSurfaceRenderer::SurfaceInfo* MultiSurfaceRenderer::getSurfaceInfo(int channelIndex) const {
    auto it = surfaces.find(channelIndex);
    return (it != surfaces.end()) ? it->second.get() : nullptr;
}

void MultiSurfaceRenderer::updateSurfaceState(int channelIndex, RenderState newState) {
    auto lock = lockSurfaces();
    
    SurfaceInfo* surfaceInfo = getSurfaceInfo(channelIndex);
    if (surfaceInfo) {
        surfaceInfo->state = newState;
    }
}

void MultiSurfaceRenderer::handleRenderError(int channelIndex, const std::string& error) {
    auto lock = lockSurfaces();
    
    SurfaceInfo* surfaceInfo = getSurfaceInfo(channelIndex);
    if (surfaceInfo) {
        surfaceInfo->lastError = error;
        updateSurfaceState(channelIndex, ERROR);
    }
    
    if (eventListener) {
        eventListener->onRenderError(channelIndex, error);
    }
    
    LOGE("Render error for channel %d: %s", channelIndex, error.c_str());
}

void MultiSurfaceRenderer::setEventListener(RenderEventListener* listener) {
    eventListener = listener;
}

void MultiSurfaceRenderer::cleanup() {
    LOGD("Cleaning up MultiSurfaceRenderer");
    
    // Stop all threads
    shouldStop = true;
    renderQueueCv.notify_all();
    
    // Wait for render threads
    for (auto& thread : renderThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    // Wait for performance monitor thread
    if (performanceMonitorThread.joinable()) {
        performanceMonitorThread.join();
    }
    
    // Clear all surfaces
    auto lock = lockSurfaces();
    surfaces.clear();
    activeSurfaceCount = 0;
    
    LOGD("MultiSurfaceRenderer cleanup complete");
}

// Additional public interface implementations
int MultiSurfaceRenderer::getFrameCount(int channelIndex) const {
    auto lock = const_cast<MultiSurfaceRenderer*>(this)->lockSurfaces();

    const SurfaceInfo* surfaceInfo = getSurfaceInfo(channelIndex);
    return surfaceInfo ? surfaceInfo->frameCount.load() : 0;
}

int MultiSurfaceRenderer::getRenderCount(int channelIndex) const {
    auto lock = const_cast<MultiSurfaceRenderer*>(this)->lockSurfaces();

    const SurfaceInfo* surfaceInfo = getSurfaceInfo(channelIndex);
    return surfaceInfo ? surfaceInfo->renderCount.load() : 0;
}

int MultiSurfaceRenderer::getDroppedFrames(int channelIndex) const {
    auto lock = const_cast<MultiSurfaceRenderer*>(this)->lockSurfaces();

    const SurfaceInfo* surfaceInfo = getSurfaceInfo(channelIndex);
    return surfaceInfo ? surfaceInfo->droppedFrames.load() : 0;
}

std::vector<int> MultiSurfaceRenderer::getActiveSurfaces() const {
    auto lock = const_cast<MultiSurfaceRenderer*>(this)->lockSurfaces();

    std::vector<int> activeSurfaces;
    for (const auto& pair : surfaces) {
        if (pair.second->state == ACTIVE) {
            activeSurfaces.push_back(pair.first);
        }
    }
    return activeSurfaces;
}

void MultiSurfaceRenderer::optimizeRenderPerformance() {
    auto lock = lockSurfaces();

    LOGD("Optimizing render performance");

    for (auto& pair : surfaces) {
        SurfaceInfo* surfaceInfo = pair.second.get();
        if (surfaceInfo->state == ACTIVE) {
            adaptiveFrameSkipping(surfaceInfo);
        }
    }
}

bool MultiSurfaceRenderer::updateSurface(int channelIndex, ANativeWindow* surface) {
    auto lock = lockSurfaces();

    SurfaceInfo* surfaceInfo = getSurfaceInfo(channelIndex);
    if (!surfaceInfo) {
        return false;
    }

    // Release old surface
    if (surfaceInfo->surface) {
        ANativeWindow_release(surfaceInfo->surface);
    }

    // Set new surface
    surfaceInfo->surface = surface;
    if (surface) {
        ANativeWindow_acquire(surface);

        // Update surface properties
        surfaceInfo->width = ANativeWindow_getWidth(surface);
        surfaceInfo->height = ANativeWindow_getHeight(surface);
        surfaceInfo->format = ANativeWindow_getFormat(surface);

        updateSurfaceState(channelIndex, ACTIVE);
    } else {
        updateSurfaceState(channelIndex, INACTIVE);
    }

    LOGD("Updated surface for channel %d", channelIndex);
    return true;
}

// SurfaceRenderWorker implementation
SurfaceRenderWorker::SurfaceRenderWorker(int id) : workerId(id), isActive(false) {}

SurfaceRenderWorker::~SurfaceRenderWorker() {
    stop();
}

void SurfaceRenderWorker::start() {
    if (!isActive.load()) {
        isActive = true;
        workerThread = std::thread(&SurfaceRenderWorker::workerLoop, this);
        LOGD("Surface render worker %d started", workerId);
    }
}

void SurfaceRenderWorker::stop() {
    if (isActive.load()) {
        isActive = false;
        taskCv.notify_all();

        if (workerThread.joinable()) {
            workerThread.join();
        }

        LOGD("Surface render worker %d stopped", workerId);
    }
}

void SurfaceRenderWorker::addRenderTask(std::function<void()> task) {
    if (isActive.load()) {
        std::lock_guard<std::mutex> lock(taskMutex);
        taskQueue.push(task);
        taskCv.notify_one();
    }
}

void SurfaceRenderWorker::workerLoop() {
    while (isActive.load()) {
        std::unique_lock<std::mutex> lock(taskMutex);
        taskCv.wait(lock, [this] { return !taskQueue.empty() || !isActive.load(); });

        if (!isActive.load()) break;

        if (!taskQueue.empty()) {
            auto task = taskQueue.front();
            taskQueue.pop();
            lock.unlock();

            try {
                task();
            } catch (const std::exception& e) {
                LOGE("Render worker %d task execution failed: %s", workerId, e.what());
            }
        }
    }
}

// RenderLoadBalancer implementation
void RenderLoadBalancer::updateMetrics(const LoadMetrics& metrics) {
    std::lock_guard<std::mutex> lock(balancerMutex);
    currentMetrics = metrics;
}

std::vector<int> RenderLoadBalancer::getOptimalRenderOrder(const std::vector<int>& surfaces) {
    std::vector<int> sortedSurfaces = surfaces;

    // Sort by render priority (could be based on FPS, importance, etc.)
    std::sort(sortedSurfaces.begin(), sortedSurfaces.end());

    return sortedSurfaces;
}

bool RenderLoadBalancer::shouldThrottleRender(int channelIndex, const LoadMetrics& metrics) const {
    // Throttle if system is under high load
    return metrics.totalRenderLoad > 80.0f || metrics.averageFps < 20.0f;
}

void RenderLoadBalancer::rebalanceRenderLoad(std::vector<int>& surfaces) {
    std::lock_guard<std::mutex> lock(balancerMutex);

    // Simple rebalancing: prioritize surfaces with better performance
    std::stable_partition(surfaces.begin(), surfaces.end(),
                         [this](int channelIndex) {
                             return !shouldThrottleRender(channelIndex, currentMetrics);
                         });
}

float RenderLoadBalancer::calculateOptimalFps(int channelIndex, const LoadMetrics& metrics) const {
    // Calculate optimal FPS based on system load
    float baseFps = 30.0f;

    if (metrics.totalRenderLoad > 80.0f) {
        baseFps = 20.0f;
    } else if (metrics.totalRenderLoad > 60.0f) {
        baseFps = 25.0f;
    }

    return baseFps;
}

// EnhancedMultiSurfaceManager implementation
EnhancedMultiSurfaceManager::EnhancedMultiSurfaceManager(int maxSurfaces)
    : currentLayout(QUAD) {

    renderer = std::make_unique<MultiSurfaceRenderer>(maxSurfaces);

    LOGD("EnhancedMultiSurfaceManager initialized for %d surfaces", maxSurfaces);
}

EnhancedMultiSurfaceManager::~EnhancedMultiSurfaceManager() {
    cleanup();
}

bool EnhancedMultiSurfaceManager::addChannelSurface(int channelIndex, ANativeWindow* surface) {
    return renderer->addSurface(channelIndex, surface);
}

bool EnhancedMultiSurfaceManager::removeChannelSurface(int channelIndex) {
    std::lock_guard<std::mutex> lock(callbacksMutex);

    renderCallbacks.erase(channelIndex);
    return renderer->removeSurface(channelIndex);
}

bool EnhancedMultiSurfaceManager::renderChannelFrame(int channelIndex, std::shared_ptr<frame_data_t> frameData) {
    bool success = renderer->queueFrame(channelIndex, frameData);

    if (success) {
        // Trigger callback if frame was rendered
        std::lock_guard<std::mutex> lock(callbacksMutex);
        auto it = renderCallbacks.find(channelIndex);
        if (it != renderCallbacks.end()) {
            it->second(channelIndex, frameData->screenW, frameData->screenH);
        }
    }

    return success;
}

bool EnhancedMultiSurfaceManager::isChannelSurfaceReady(int channelIndex) const {
    return renderer->isSurfaceReady(channelIndex);
}

void EnhancedMultiSurfaceManager::setLayout(LayoutMode layout) {
    currentLayout = layout;
    updateLayoutConfiguration();
    LOGD("Set layout mode to %d", layout);
}

void EnhancedMultiSurfaceManager::setVisibleChannels(const std::vector<int>& channels) {
    visibleChannels = channels;
    updateLayoutConfiguration();
}

void EnhancedMultiSurfaceManager::setRenderCallback(int channelIndex,
                                                   std::function<void(int, int, int)> callback) {
    std::lock_guard<std::mutex> lock(callbacksMutex);
    renderCallbacks[channelIndex] = callback;
}

void EnhancedMultiSurfaceManager::removeRenderCallback(int channelIndex) {
    std::lock_guard<std::mutex> lock(callbacksMutex);
    renderCallbacks.erase(channelIndex);
}

int EnhancedMultiSurfaceManager::getActiveSurfaceCount() const {
    return renderer->getActiveSurfaceCount();
}

std::vector<int> EnhancedMultiSurfaceManager::getActiveSurfaces() const {
    return renderer->getActiveSurfaces();
}

void EnhancedMultiSurfaceManager::optimizeRenderingPerformance() {
    renderer->optimizeRenderPerformance();
}

void EnhancedMultiSurfaceManager::updateLayoutConfiguration() {
    // Adjust rendering parameters based on layout
    float targetFps = 30.0f;

    switch (currentLayout) {
        case SINGLE:
            targetFps = 30.0f;
            break;
        case QUAD:
            targetFps = 25.0f;
            break;
        case NINE:
            targetFps = 20.0f;
            break;
        case SIXTEEN:
            targetFps = 15.0f;
            break;
    }

    // Apply FPS settings to visible channels
    for (int channelIndex : visibleChannels) {
        renderer->setTargetFps(channelIndex, targetFps);
    }
}

void EnhancedMultiSurfaceManager::cleanup() {
    std::lock_guard<std::mutex> lock(callbacksMutex);

    renderCallbacks.clear();
    renderer.reset();

    LOGD("EnhancedMultiSurfaceManager cleanup complete");
}
