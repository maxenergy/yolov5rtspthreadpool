#include "MultiStreamProcessor.h"
#include <algorithm>
#include <numeric>

MultiStreamProcessor::MultiStreamProcessor(int maxStreams, int threadCount)
    : maxConcurrentStreams(maxStreams), processingThreadCount(threadCount),
      cpuThreshold(80.0f), memoryThreshold(512 * 1024 * 1024), // 512MB
      loadBalanceInterval(5000), // 5 seconds
      shouldStop(false), eventListener(nullptr),
      systemCpuUsage(0.0f), systemMemoryUsage(0), activeStreamCount(0) {
    
    // Start processing threads
    for (int i = 0; i < processingThreadCount; ++i) {
        processingThreads.emplace_back(&MultiStreamProcessor::processingThreadLoop, this, i);
    }
    
    // Start load balancer thread
    loadBalancerThread = std::thread(&MultiStreamProcessor::loadBalancerLoop, this);
    
    // Start resource monitor thread
    resourceMonitorThread = std::thread(&MultiStreamProcessor::resourceMonitorLoop, this);
    
    LOGD("MultiStreamProcessor initialized with %d max streams, %d threads", 
         maxStreams, threadCount);
}

MultiStreamProcessor::~MultiStreamProcessor() {
    cleanup();
}

bool MultiStreamProcessor::addStream(const StreamConfig& config) {
    auto lock = lockStreams();
    
    if (streamConfigs.size() >= static_cast<size_t>(maxConcurrentStreams)) {
        LOGE("Cannot add stream: maximum concurrent streams (%d) reached", maxConcurrentStreams);
        return false;
    }
    
    // Remove existing stream if present
    auto it = streamConfigs.find(config.channelIndex);
    if (it != streamConfigs.end()) {
        LOGW("Replacing existing stream configuration for channel %d", config.channelIndex);
        removeStream(config.channelIndex);
    }
    
    // Add new stream configuration
    streamConfigs[config.channelIndex] = config;
    streamStats[config.channelIndex] = StreamStats(config.channelIndex);
    
    // Create stream manager
    auto streamManager = std::make_unique<RTSPStreamManager>();
    streamManager->addStream(config.channelIndex, config.rtspUrl);
    streamManager->setAutoReconnect(config.channelIndex, config.autoReconnect);
    streamManagers[config.channelIndex] = std::move(streamManager);
    
    LOGD("Added stream for channel %d: %s", config.channelIndex, config.rtspUrl.c_str());
    return true;
}

bool MultiStreamProcessor::removeStream(int channelIndex) {
    auto lock = lockStreams();
    
    // Stop stream if running
    stopStream(channelIndex);
    
    // Remove from collections
    streamConfigs.erase(channelIndex);
    streamStats.erase(channelIndex);
    streamManagers.erase(channelIndex);
    
    LOGD("Removed stream for channel %d", channelIndex);
    return true;
}

bool MultiStreamProcessor::startStream(int channelIndex) {
    auto lock = lockStreams();
    
    auto managerIt = streamManagers.find(channelIndex);
    if (managerIt == streamManagers.end()) {
        LOGE("Stream manager not found for channel %d", channelIndex);
        return false;
    }
    
    // Check system resources before starting
    if (isSystemOverloaded()) {
        LOGW("System overloaded, cannot start stream for channel %d", channelIndex);
        return false;
    }
    
    // Start the stream
    if (managerIt->second->startStream(channelIndex)) {
        activeStreamCount++;
        
        // Add to processing queue
        {
            auto queueLock = lockQueue();
            processingQueue.push(channelIndex);
            queueCv.notify_one();
        }
        
        // Update stats
        auto statsIt = streamStats.find(channelIndex);
        if (statsIt != streamStats.end()) {
            statsIt->second.startTime = std::chrono::steady_clock::now();
            statsIt->second.state = RTSPStreamManager::CONNECTING;
        }
        
        if (eventListener) {
            eventListener->onStreamProcessingStarted(channelIndex);
        }
        
        LOGD("Started stream processing for channel %d", channelIndex);
        return true;
    }
    
    return false;
}

bool MultiStreamProcessor::stopStream(int channelIndex) {
    auto lock = lockStreams();
    
    auto managerIt = streamManagers.find(channelIndex);
    if (managerIt == streamManagers.end()) {
        return false;
    }
    
    // Stop the stream
    managerIt->second->stopStream(channelIndex);
    activeStreamCount--;
    
    // Update stats
    auto statsIt = streamStats.find(channelIndex);
    if (statsIt != streamStats.end()) {
        statsIt->second.state = RTSPStreamManager::DISCONNECTED;
    }
    
    if (eventListener) {
        eventListener->onStreamProcessingStopped(channelIndex);
    }
    
    LOGD("Stopped stream processing for channel %d", channelIndex);
    return true;
}

bool MultiStreamProcessor::startAllStreams() {
    auto lock = lockStreams();
    bool allStarted = true;
    
    for (const auto& pair : streamConfigs) {
        if (!startStream(pair.first)) {
            allStarted = false;
            LOGW("Failed to start stream for channel %d", pair.first);
        }
    }
    
    LOGD("Started all streams, success: %s", allStarted ? "true" : "false");
    return allStarted;
}

bool MultiStreamProcessor::stopAllStreams() {
    auto lock = lockStreams();
    
    for (const auto& pair : streamManagers) {
        stopStream(pair.first);
    }
    
    LOGD("Stopped all streams");
    return true;
}

void MultiStreamProcessor::processingThreadLoop(int threadId) {
    LOGD("Processing thread %d started", threadId);
    
    while (!shouldStop) {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCv.wait(lock, [this] { return !processingQueue.empty() || shouldStop; });
        
        if (shouldStop) break;
        
        if (!processingQueue.empty()) {
            int channelIndex = processingQueue.front();
            processingQueue.pop();
            lock.unlock();
            
            processStream(channelIndex);
        }
    }
    
    LOGD("Processing thread %d stopped", threadId);
}

void MultiStreamProcessor::processStream(int channelIndex) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Check if stream should be processed based on priority and resources
    if (!shouldProcessStream(channelIndex)) {
        return;
    }
    
    auto lock = lockStreams();
    auto managerIt = streamManagers.find(channelIndex);
    if (managerIt == streamManagers.end()) {
        return;
    }
    
    RTSPStreamManager* manager = managerIt->second.get();
    lock.unlock();
    
    // Check stream health
    if (!manager->isStreamHealthy(channelIndex)) {
        LOGW("Stream unhealthy for channel %d, skipping processing", channelIndex);
        return;
    }
    
    // Simulate frame processing (in real implementation, this would process actual frames)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto processingTime = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    
    // Update statistics
    updateStreamStats(channelIndex, true, processingTime);
    
    // Re-queue for continuous processing if stream is still active
    if (manager->getStreamState(channelIndex) == RTSPStreamManager::STREAMING) {
        auto queueLock = lockQueue();
        processingQueue.push(channelIndex);
        queueCv.notify_one();
    }
}

void MultiStreamProcessor::updateStreamStats(int channelIndex, bool frameProcessed, double processingTime) {
    auto lock = lockStreams();
    auto statsIt = streamStats.find(channelIndex);
    if (statsIt == streamStats.end()) {
        return;
    }
    
    StreamStats& stats = statsIt->second;
    
    if (frameProcessed) {
        stats.frameCount++;
        stats.lastFrameTime = std::chrono::steady_clock::now();
        stats.totalProcessingTime += processingTime;
        stats.averageProcessingTime = stats.totalProcessingTime / stats.frameCount;
        
        // Update FPS calculation
        auto now = std::chrono::steady_clock::now();
        auto timeSinceStart = std::chrono::duration_cast<std::chrono::seconds>(now - stats.startTime);
        if (timeSinceStart.count() > 0) {
            stats.currentFps = static_cast<float>(stats.frameCount) / timeSinceStart.count();
        }
    }
    
    // Update stream state from manager
    auto managerIt = streamManagers.find(channelIndex);
    if (managerIt != streamManagers.end()) {
        stats.state = managerIt->second->getStreamState(channelIndex);
    }
}

void MultiStreamProcessor::loadBalancerLoop() {
    LOGD("Load balancer thread started");
    
    while (!shouldStop) {
        std::unique_lock<std::mutex> lock(loadBalancerMutex);
        loadBalancerCv.wait_for(lock, std::chrono::milliseconds(loadBalanceInterval));
        
        if (shouldStop) break;
        
        performLoadBalancing();
    }
    
    LOGD("Load balancer thread stopped");
}

void MultiStreamProcessor::performLoadBalancing() {
    if (isSystemOverloaded()) {
        LOGD("System overloaded, performing load balancing");
        
        auto overloadedStreams = identifyOverloadedStreams();
        if (!overloadedStreams.empty()) {
            redistributeLoad(overloadedStreams);
            
            if (eventListener) {
                eventListener->onLoadBalancingTriggered(overloadedStreams);
            }
        }
    }
}

std::vector<int> MultiStreamProcessor::identifyOverloadedStreams() {
    std::vector<int> overloadedStreams;
    auto lock = lockStreams();
    
    for (const auto& pair : streamStats) {
        const StreamStats& stats = pair.second;
        
        // Identify streams with poor performance
        if (stats.currentFps < 15.0f || stats.averageProcessingTime > 50.0) {
            overloadedStreams.push_back(pair.first);
        }
    }
    
    return overloadedStreams;
}

void MultiStreamProcessor::redistributeLoad(const std::vector<int>& overloadedStreams) {
    // Simple load redistribution: reduce processing frequency for overloaded streams
    for (int channelIndex : overloadedStreams) {
        auto configIt = streamConfigs.find(channelIndex);
        if (configIt != streamConfigs.end()) {
            // Reduce target FPS for overloaded streams
            configIt->second.targetFps = std::max(15.0f, configIt->second.targetFps * 0.8f);
            LOGD("Reduced target FPS for channel %d to %.1f", channelIndex, configIt->second.targetFps);
        }
    }
}

void MultiStreamProcessor::resourceMonitorLoop() {
    LOGD("Resource monitor thread started");
    
    while (!shouldStop) {
        updateSystemResources();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    LOGD("Resource monitor thread stopped");
}

void MultiStreamProcessor::updateSystemResources() {
    // Simplified resource monitoring
    // In a real implementation, this would query actual system resources
    
    // Simulate CPU usage based on active stream count
    float cpuUsage = std::min(100.0f, activeStreamCount.load() * 5.0f);
    systemCpuUsage.store(cpuUsage);
    
    // Simulate memory usage
    long memoryUsage = activeStreamCount.load() * 32 * 1024 * 1024; // 32MB per stream
    systemMemoryUsage.store(memoryUsage);
}

bool MultiStreamProcessor::isSystemOverloaded() const {
    return systemCpuUsage.load() > cpuThreshold || 
           systemMemoryUsage.load() > memoryThreshold;
}

bool MultiStreamProcessor::shouldProcessStream(int channelIndex) const {
    auto lock = const_cast<MultiStreamProcessor*>(this)->lockStreams();
    
    // Check if system is overloaded
    if (isSystemOverloaded()) {
        // Only process high priority streams when overloaded
        auto configIt = streamConfigs.find(channelIndex);
        if (configIt != streamConfigs.end()) {
            return configIt->second.priority >= HIGH;
        }
        return false;
    }
    
    return true;
}

void MultiStreamProcessor::cleanup() {
    LOGD("Cleaning up MultiStreamProcessor");
    
    // Stop all threads
    shouldStop = true;
    queueCv.notify_all();
    loadBalancerCv.notify_all();
    
    // Wait for processing threads
    for (auto& thread : processingThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    // Wait for other threads
    if (loadBalancerThread.joinable()) {
        loadBalancerThread.join();
    }
    
    if (resourceMonitorThread.joinable()) {
        resourceMonitorThread.join();
    }
    
    // Stop all streams
    stopAllStreams();
    
    // Clear all data
    {
        auto lock = lockStreams();
        streamManagers.clear();
        streamConfigs.clear();
        streamStats.clear();
    }
    
    LOGD("MultiStreamProcessor cleanup complete");
}

// Public interface implementations
bool MultiStreamProcessor::updateStreamConfig(int channelIndex, const StreamConfig& config) {
    auto lock = lockStreams();

    auto it = streamConfigs.find(channelIndex);
    if (it == streamConfigs.end()) {
        LOGE("Stream configuration not found for channel %d", channelIndex);
        return false;
    }

    // Update configuration
    it->second = config;

    // Update stream manager if URL changed
    auto managerIt = streamManagers.find(channelIndex);
    if (managerIt != streamManagers.end()) {
        managerIt->second->removeStream(channelIndex);
        managerIt->second->addStream(channelIndex, config.rtspUrl);
        managerIt->second->setAutoReconnect(channelIndex, config.autoReconnect);
    }

    LOGD("Updated stream configuration for channel %d", channelIndex);
    return true;
}

void MultiStreamProcessor::setStreamPriority(int channelIndex, ProcessingPriority priority) {
    auto lock = lockStreams();

    auto it = streamConfigs.find(channelIndex);
    if (it != streamConfigs.end()) {
        it->second.priority = priority;
        LOGD("Set priority for channel %d to %d", channelIndex, priority);
    }
}

MultiStreamProcessor::ProcessingPriority MultiStreamProcessor::getStreamPriority(int channelIndex) const {
    auto lock = const_cast<MultiStreamProcessor*>(this)->lockStreams();

    auto it = streamConfigs.find(channelIndex);
    return (it != streamConfigs.end()) ? it->second.priority : NORMAL;
}

void MultiStreamProcessor::setResourceLimits(float cpuThreshold, long memoryThreshold) {
    this->cpuThreshold = cpuThreshold;
    this->memoryThreshold = memoryThreshold;
    LOGD("Updated resource limits: CPU %.1f%%, Memory %ld bytes", cpuThreshold, memoryThreshold);
}

void MultiStreamProcessor::setMaxConcurrentStreams(int maxStreams) {
    maxConcurrentStreams = maxStreams;
    LOGD("Updated max concurrent streams to %d", maxStreams);
}

MultiStreamProcessor::StreamStats MultiStreamProcessor::getStreamStats(int channelIndex) const {
    auto lock = const_cast<MultiStreamProcessor*>(this)->lockStreams();

    auto it = streamStats.find(channelIndex);
    return (it != streamStats.end()) ? it->second : StreamStats(channelIndex);
}

std::vector<MultiStreamProcessor::StreamStats> MultiStreamProcessor::getAllStreamStats() const {
    auto lock = const_cast<MultiStreamProcessor*>(this)->lockStreams();

    std::vector<StreamStats> allStats;
    for (const auto& pair : streamStats) {
        allStats.push_back(pair.second);
    }
    return allStats;
}

void MultiStreamProcessor::setEventListener(ProcessingEventListener* listener) {
    eventListener = listener;
}

void MultiStreamProcessor::enableLoadBalancing(bool enabled) {
    // Implementation would enable/disable load balancing
    LOGD("Load balancing %s", enabled ? "enabled" : "disabled");
}

void MultiStreamProcessor::triggerLoadBalancing() {
    loadBalancerCv.notify_one();
}

// Utility method implementations
MultiStreamProcessor::StreamConfig* MultiStreamProcessor::getStreamConfig(int channelIndex) {
    auto it = streamConfigs.find(channelIndex);
    return (it != streamConfigs.end()) ? &it->second : nullptr;
}

const MultiStreamProcessor::StreamConfig* MultiStreamProcessor::getStreamConfig(int channelIndex) const {
    auto it = streamConfigs.find(channelIndex);
    return (it != streamConfigs.end()) ? &it->second : nullptr;
}

MultiStreamProcessor::StreamStats* MultiStreamProcessor::getStreamStatsInternal(int channelIndex) {
    auto it = streamStats.find(channelIndex);
    return (it != streamStats.end()) ? &it->second : nullptr;
}

RTSPStreamManager* MultiStreamProcessor::getStreamManager(int channelIndex) {
    auto it = streamManagers.find(channelIndex);
    return (it != streamManagers.end()) ? it->second.get() : nullptr;
}

void MultiStreamProcessor::sortStreamsByPriority(std::vector<int>& channels) {
    std::sort(channels.begin(), channels.end(), [this](int a, int b) {
        auto configA = getStreamConfig(a);
        auto configB = getStreamConfig(b);

        if (!configA || !configB) return false;

        return configA->priority > configB->priority;
    });
}

// StreamProcessingWorker implementation
StreamProcessingWorker::StreamProcessingWorker(int id) : workerId(id), isActive(false) {}

StreamProcessingWorker::~StreamProcessingWorker() {
    stop();
}

void StreamProcessingWorker::start() {
    if (!isActive.load()) {
        isActive = true;
        workerThread = std::thread(&StreamProcessingWorker::workerLoop, this);
        LOGD("Stream processing worker %d started", workerId);
    }
}

void StreamProcessingWorker::stop() {
    if (isActive.load()) {
        isActive = false;
        taskCv.notify_all();

        if (workerThread.joinable()) {
            workerThread.join();
        }

        LOGD("Stream processing worker %d stopped", workerId);
    }
}

void StreamProcessingWorker::addTask(std::function<void()> task) {
    if (isActive.load()) {
        std::lock_guard<std::mutex> lock(taskMutex);
        taskQueue.push(task);
        taskCv.notify_one();
    }
}

void StreamProcessingWorker::workerLoop() {
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
                LOGE("Worker %d task execution failed: %s", workerId, e.what());
            }
        }
    }
}

// StreamLoadBalancer implementation
void StreamLoadBalancer::updateMetrics(const LoadMetrics& metrics) {
    std::lock_guard<std::mutex> lock(balancerMutex);
    currentMetrics = metrics;
}

std::vector<int> StreamLoadBalancer::getOptimalStreamDistribution(
    const std::vector<int>& channels,
    const std::map<int, MultiStreamProcessor::ProcessingPriority>& priorities) {

    std::vector<int> sortedChannels = channels;

    // Sort by priority (high priority first)
    std::sort(sortedChannels.begin(), sortedChannels.end(),
              [&priorities](int a, int b) {
                  auto itA = priorities.find(a);
                  auto itB = priorities.find(b);

                  int priorityA = (itA != priorities.end()) ? itA->second : MultiStreamProcessor::NORMAL;
                  int priorityB = (itB != priorities.end()) ? itB->second : MultiStreamProcessor::NORMAL;

                  return priorityA > priorityB;
              });

    return sortedChannels;
}

bool StreamLoadBalancer::shouldThrottleStream(int channelIndex, const LoadMetrics& metrics) const {
    // Throttle if system is under high load
    return metrics.cpuUsage > 80.0f || metrics.averageFps < 20.0f;
}

void StreamLoadBalancer::rebalanceStreams(std::vector<int>& channels) {
    std::lock_guard<std::mutex> lock(balancerMutex);

    // Simple rebalancing: move high-load streams to the end
    std::stable_partition(channels.begin(), channels.end(),
                         [this](int channelIndex) {
                             return !shouldThrottleStream(channelIndex, currentMetrics);
                         });
}
