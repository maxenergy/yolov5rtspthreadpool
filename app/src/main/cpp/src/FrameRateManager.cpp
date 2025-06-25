#include "FrameRateManager.h"
#include "logging.h"
#include <algorithm>
#include <numeric>

FrameRateManager::FrameRateManager() 
    : systemStartTime(std::chrono::steady_clock::now()) {
    LOGD("FrameRateManager created");
}

FrameRateManager::~FrameRateManager() {
    stopMonitoring();
    std::lock_guard<std::mutex> lock(statesMutex);
    channelStates.clear();
    LOGD("FrameRateManager destroyed");
}

bool FrameRateManager::addChannel(int channelIndex, float targetFps, int priority) {
    if (!validateChannelIndex(channelIndex)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(statesMutex);
    
    if (channelStates.find(channelIndex) != channelStates.end()) {
        LOGW("Channel %d already exists", channelIndex);
        return false;
    }

    auto state = std::make_unique<ChannelFrameState>();
    state->channelIndex = channelIndex;
    state->targetFps = targetFps;
    state->priority = priority;
    
    channelStates[channelIndex] = std::move(state);
    
    LOGD("Added channel %d with target FPS %.2f and priority %d", channelIndex, targetFps, priority);
    return true;
}

bool FrameRateManager::removeChannel(int channelIndex) {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    auto it = channelStates.find(channelIndex);
    if (it == channelStates.end()) {
        return false;
    }
    
    channelStates.erase(it);
    LOGD("Removed channel %d", channelIndex);
    return true;
}

bool FrameRateManager::setChannelTargetFps(int channelIndex, float targetFps) {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return false;
    }
    
    state->targetFps = targetFps;
    LOGD("Set target FPS for channel %d: %.2f", channelIndex, targetFps);
    return true;
}

bool FrameRateManager::setChannelPriority(int channelIndex, int priority) {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return false;
    }
    
    state->priority = priority;
    LOGD("Set priority for channel %d: %d", channelIndex, priority);
    return true;
}

bool FrameRateManager::setChannelActive(int channelIndex, bool active) {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return false;
    }
    
    state->isActive = active;
    LOGD("Set channel %d active state: %s", channelIndex, active ? "true" : "false");
    return true;
}

bool FrameRateManager::setChannelVisible(int channelIndex, bool visible) {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return false;
    }
    
    state->isVisible = visible;
    return true;
}

bool FrameRateManager::shouldProcessFrame(int channelIndex) {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    auto state = getChannelStateInternal(channelIndex);
    if (!state || !state->isVisible) {
        return false;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastFrame = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state->lastFrameTime);
    
    // Calculate target frame interval based on current target FPS
    float targetInterval = 1000.0f / state->targetFps; // milliseconds
    
    // Check if enough time has passed
    if (timeSinceLastFrame.count() >= targetInterval) {
        return true;
    }
    
    // Apply adaptive frame skipping if enabled
    if (adaptiveFrameSkippingEnabled.load() && shouldSkipFrame(channelIndex)) {
        return false;
    }
    
    return timeSinceLastFrame.count() >= targetInterval;
}

void FrameRateManager::recordFrameProcessed(int channelIndex) {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto frameTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state->lastFrameTime);
    
    state->lastFrameTime = now;
    state->frameCount++;
    
    // Update average frame time (exponential moving average)
    float alpha = 0.1f;
    state->averageFrameTime = (state->averageFrameTime * (1.0f - alpha)) + 
                             (frameTime.count() * alpha);
    
    // Update FPS calculation every second
    auto timeSinceLastFpsUpdate = std::chrono::duration_cast<std::chrono::seconds>(
        now - state->lastFpsUpdate);
    
    if (timeSinceLastFpsUpdate.count() >= 1) {
        state->actualFps = state->frameCount / static_cast<float>(timeSinceLastFpsUpdate.count());
        state->lastFpsUpdate = now;
        state->frameCount = 0;
    }
}

void FrameRateManager::recordFrameDropped(int channelIndex) {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    auto state = getChannelStateInternal(channelIndex);
    if (state) {
        state->droppedFrames++;
    }
}

float FrameRateManager::getChannelFrameInterval(int channelIndex) const {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return 33.33f; // Default 30 FPS
    }
    
    return 1000.0f / state->targetFps;
}

void FrameRateManager::updateSystemLoad(float load) {
    currentSystemLoad.store(load);
    
    // Trigger optimization if load is high
    if (load > systemLoadThreshold.load()) {
        optimizeFrameRates();
    }
}

void FrameRateManager::optimizeFrameRates() {
    std::lock_guard<std::mutex> lock(statesMutex);
    
    LOGD("Optimizing frame rates (strategy: %d, system load: %.2f)", 
         strategy.load(), currentSystemLoad.load());
    
    switch (strategy.load()) {
        case FIXED_30FPS:
            // No optimization needed for fixed strategy
            break;
        case ADAPTIVE:
            applyAdaptiveOptimization();
            break;
        case PRIORITY_BASED:
            applyPriorityBasedOptimization();
            break;
        case LOAD_BALANCED:
            applyLoadBalancedOptimization();
            break;
    }
    
    updateSystemMetrics();
}

void FrameRateManager::startMonitoring() {
    if (monitoringActive.load()) {
        return;
    }
    
    monitoringActive.store(true);
    monitoringThread = std::thread(&FrameRateManager::monitoringLoop, this);
    LOGD("Frame rate monitoring started");
}

void FrameRateManager::stopMonitoring() {
    if (!monitoringActive.load()) {
        return;
    }
    
    monitoringActive.store(false);
    monitoringCv.notify_all();
    
    if (monitoringThread.joinable()) {
        monitoringThread.join();
    }
    
    LOGD("Frame rate monitoring stopped");
}

FrameRateManager::ChannelFrameState* FrameRateManager::getChannelStateInternal(int channelIndex) {
    auto it = channelStates.find(channelIndex);
    return (it != channelStates.end()) ? it->second.get() : nullptr;
}

const FrameRateManager::ChannelFrameState* FrameRateManager::getChannelStateInternal(int channelIndex) const {
    auto it = channelStates.find(channelIndex);
    return (it != channelStates.end()) ? it->second.get() : nullptr;
}

void FrameRateManager::updateChannelMetrics(int channelIndex) {
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return;
    }
    
    // Calculate frame time variance
    float variance = 0.0f;
    if (state->frameCount > 1) {
        float expectedFrameTime = 1000.0f / state->targetFps;
        float diff = state->averageFrameTime - expectedFrameTime;
        variance = diff * diff;
    }
    state->frameTimeVariance = variance;
}

void FrameRateManager::updateSystemMetrics() {
    std::lock_guard<std::mutex> lock(metricsMutex);
    
    systemMetrics.activeChannels = 0;
    systemMetrics.totalFramesProcessed = 0;
    systemMetrics.totalFramesDropped = 0;
    float totalFps = 0.0f;
    float totalVariance = 0.0f;
    
    for (const auto& pair : channelStates) {
        const auto& state = pair.second;
        if (state->isActive) {
            systemMetrics.activeChannels++;
            totalFps += state->actualFps;
        }
        systemMetrics.totalFramesProcessed += state->frameCount;
        systemMetrics.totalFramesDropped += state->droppedFrames;
        totalVariance += state->frameTimeVariance;
    }
    
    systemMetrics.averageSystemFps = (systemMetrics.activeChannels > 0) ? 
        totalFps / systemMetrics.activeChannels : 0.0f;
    systemMetrics.systemFrameTimeVariance = (channelStates.size() > 0) ?
        totalVariance / channelStates.size() : 0.0f;
    systemMetrics.totalSystemLoad = currentSystemLoad.load();
    systemMetrics.lastUpdate = std::chrono::steady_clock::now();
}

void FrameRateManager::applyAdaptiveOptimization() {
    float systemLoad = currentSystemLoad.load();
    
    for (auto& pair : channelStates) {
        auto& state = pair.second;
        
        if (systemLoad > 0.9f) {
            // Very high load - aggressive reduction
            state->targetFps = state->isActive ? 15.0f : 5.0f;
        } else if (systemLoad > 0.7f) {
            // High load - moderate reduction
            state->targetFps = state->isActive ? 20.0f : 10.0f;
        } else if (systemLoad > 0.5f) {
            // Medium load - slight reduction
            state->targetFps = state->isActive ? 25.0f : 15.0f;
        } else {
            // Low load - restore target FPS
            state->targetFps = state->isActive ? 30.0f : 20.0f;
        }
    }
}

void FrameRateManager::applyPriorityBasedOptimization() {
    // Sort channels by priority
    std::vector<std::pair<int, ChannelFrameState*>> prioritizedChannels;
    for (auto& pair : channelStates) {
        prioritizedChannels.push_back({pair.first, pair.second.get()});
    }
    
    std::sort(prioritizedChannels.begin(), prioritizedChannels.end(),
              [](const auto& a, const auto& b) {
                  return a.second->priority > b.second->priority;
              });
    
    // Allocate FPS based on priority
    float systemLoad = currentSystemLoad.load();
    float baseFps = (systemLoad > 0.8f) ? 20.0f : 30.0f;
    
    for (size_t i = 0; i < prioritizedChannels.size(); i++) {
        auto& state = prioritizedChannels[i].second;
        
        if (i < prioritizedChannels.size() / 3) {
            // Top third - full FPS
            state->targetFps = state->isActive ? baseFps : baseFps * 0.5f;
        } else if (i < 2 * prioritizedChannels.size() / 3) {
            // Middle third - reduced FPS
            state->targetFps = state->isActive ? baseFps * 0.7f : baseFps * 0.3f;
        } else {
            // Bottom third - minimal FPS
            state->targetFps = state->isActive ? baseFps * 0.5f : baseFps * 0.2f;
        }
    }
}

void FrameRateManager::applyLoadBalancedOptimization() {
    float totalFpsBudget = targetSystemFps.load() * channelStates.size();
    float systemLoad = currentSystemLoad.load();
    
    // Adjust budget based on system load
    if (systemLoad > 0.8f) {
        totalFpsBudget *= 0.6f;
    } else if (systemLoad > 0.6f) {
        totalFpsBudget *= 0.8f;
    }
    
    // Count active and visible channels
    int activeChannels = 0;
    int visibleChannels = 0;
    for (const auto& pair : channelStates) {
        if (pair.second->isActive) activeChannels++;
        if (pair.second->isVisible) visibleChannels++;
    }
    
    // Distribute FPS budget
    float activeFps = (activeChannels > 0) ? totalFpsBudget * 0.7f / activeChannels : 0.0f;
    float inactiveFps = (visibleChannels - activeChannels > 0) ? 
        totalFpsBudget * 0.3f / (visibleChannels - activeChannels) : 0.0f;
    
    for (auto& pair : channelStates) {
        auto& state = pair.second;
        if (state->isActive) {
            state->targetFps = std::min(30.0f, activeFps);
        } else if (state->isVisible) {
            state->targetFps = std::min(15.0f, inactiveFps);
        } else {
            state->targetFps = 5.0f;
        }
    }
}

bool FrameRateManager::shouldSkipFrame(int channelIndex) const {
    auto state = getChannelStateInternal(channelIndex);
    if (!state) {
        return false;
    }
    
    float systemLoad = currentSystemLoad.load();
    
    // Skip frames for inactive channels under high load
    if (!state->isActive && systemLoad > 0.7f) {
        return true;
    }
    
    // Skip frames if channel is significantly behind target FPS
    if (state->actualFps > state->targetFps * 1.2f) {
        return true;
    }
    
    return false;
}

void FrameRateManager::monitoringLoop() {
    while (monitoringActive.load()) {
        std::unique_lock<std::mutex> lock(monitoringMutex);
        monitoringCv.wait_for(lock, std::chrono::seconds(1), 
                             [this] { return !monitoringActive.load(); });
        
        if (!monitoringActive.load()) break;
        
        // Update metrics and optimize if needed
        updateSystemMetrics();
        
        if (currentSystemLoad.load() > systemLoadThreshold.load()) {
            optimizeFrameRates();
        }
    }
}

bool FrameRateManager::validateChannelIndex(int channelIndex) const {
    return channelIndex >= 0 && channelIndex < 16; // Support up to 16 channels
}

void FrameRateManager::setFrameRateStrategy(FrameRateStrategy newStrategy) {
    strategy.store(newStrategy);
    LOGD("Frame rate strategy changed to %d", newStrategy);
}

void FrameRateManager::setTargetSystemFps(float fps) {
    targetSystemFps.store(fps);
    LOGD("Target system FPS set to %.2f", fps);
}

void FrameRateManager::setSystemLoadThreshold(float threshold) {
    systemLoadThreshold.store(threshold);
    LOGD("System load threshold set to %.2f", threshold);
}

FrameRateManager::SystemFrameMetrics FrameRateManager::getSystemMetrics() const {
    std::lock_guard<std::mutex> lock(metricsMutex);
    return systemMetrics;
}

FrameRateManager::ChannelFrameState FrameRateManager::getChannelState(int channelIndex) const {
    std::lock_guard<std::mutex> lock(statesMutex);
    auto state = getChannelStateInternal(channelIndex);
    return state ? *state : ChannelFrameState();
}

std::vector<int> FrameRateManager::getActiveChannels() const {
    std::lock_guard<std::mutex> lock(statesMutex);
    std::vector<int> activeChannels;

    for (const auto& pair : channelStates) {
        if (pair.second->isActive) {
            activeChannels.push_back(pair.first);
        }
    }

    return activeChannels;
}

std::vector<int> FrameRateManager::getSlowChannels(float thresholdFps) const {
    std::lock_guard<std::mutex> lock(statesMutex);
    std::vector<int> slowChannels;

    for (const auto& pair : channelStates) {
        if (pair.second->actualFps < thresholdFps) {
            slowChannels.push_back(pair.first);
        }
    }

    return slowChannels;
}

void FrameRateManager::setAdaptiveFrameSkippingEnabled(bool enabled) {
    adaptiveFrameSkippingEnabled.store(enabled);
    LOGD("Adaptive frame skipping %s", enabled ? "enabled" : "disabled");
}

void FrameRateManager::setLoadBalancingEnabled(bool enabled) {
    loadBalancingEnabled.store(enabled);
    LOGD("Load balancing %s", enabled ? "enabled" : "disabled");
}

void FrameRateManager::resetAllChannels() {
    std::lock_guard<std::mutex> lock(statesMutex);

    for (auto& pair : channelStates) {
        auto& state = pair.second;
        state->targetFps = 30.0f;
        state->actualFps = 0.0f;
        state->frameCount = 0;
        state->droppedFrames = 0;
        state->lastFrameTime = std::chrono::steady_clock::now();
        state->lastFpsUpdate = state->lastFrameTime;
    }

    LOGD("Reset all channel frame states");
}

// AdaptiveFrameSkipper implementation
AdaptiveFrameSkipper::AdaptiveFrameSkipper() {
    LOGD("AdaptiveFrameSkipper created");
}

AdaptiveFrameSkipper::~AdaptiveFrameSkipper() {
    LOGD("AdaptiveFrameSkipper destroyed");
}

bool AdaptiveFrameSkipper::shouldSkipFrame(int channelIndex, float systemLoad,
                                          bool isActiveChannel, float channelFps) {
    std::lock_guard<std::mutex> lock(skipperMutex);

    // Don't skip if system load is low
    if (systemLoad < config.loadThreshold) {
        return false;
    }

    // Check consecutive skips limit
    int currentSkips = consecutiveSkips[channelIndex];
    if (currentSkips >= config.maxConsecutiveSkips) {
        return false;
    }

    // Prioritize active channels
    if (config.prioritizeActiveChannels && isActiveChannel) {
        // Only skip active channels under very high load
        return systemLoad > 0.9f && currentSkips < config.maxConsecutiveSkips / 2;
    }

    // Skip inactive channels more aggressively
    if (!isActiveChannel) {
        return systemLoad > config.loadThreshold;
    }

    // Skip based on channel performance
    if (channelFps > 35.0f) { // Channel running too fast
        return true;
    }

    return false;
}

void AdaptiveFrameSkipper::recordFrameSkipped(int channelIndex) {
    std::lock_guard<std::mutex> lock(skipperMutex);
    consecutiveSkips[channelIndex]++;
}

void AdaptiveFrameSkipper::recordFrameProcessed(int channelIndex) {
    std::lock_guard<std::mutex> lock(skipperMutex);
    consecutiveSkips[channelIndex] = 0;
}

void AdaptiveFrameSkipper::setSkippingConfig(const SkippingConfig& newConfig) {
    std::lock_guard<std::mutex> lock(skipperMutex);
    config = newConfig;
    LOGD("Frame skipping configuration updated");
}

AdaptiveFrameSkipper::SkippingConfig AdaptiveFrameSkipper::getSkippingConfig() const {
    std::lock_guard<std::mutex> lock(skipperMutex);
    return config;
}

void AdaptiveFrameSkipper::resetSkippingState() {
    std::lock_guard<std::mutex> lock(skipperMutex);
    consecutiveSkips.clear();
    LOGD("Frame skipping state reset");
}

int AdaptiveFrameSkipper::getConsecutiveSkips(int channelIndex) const {
    std::lock_guard<std::mutex> lock(skipperMutex);
    auto it = consecutiveSkips.find(channelIndex);
    return (it != consecutiveSkips.end()) ? it->second : 0;
}

// FrameRateLoadBalancer implementation
FrameRateLoadBalancer::FrameRateLoadBalancer() {
    LOGD("FrameRateLoadBalancer created");
}

FrameRateLoadBalancer::~FrameRateLoadBalancer() {
    LOGD("FrameRateLoadBalancer destroyed");
}

void FrameRateLoadBalancer::rebalanceFrameRates(const std::vector<int>& channels,
                                               const std::unordered_map<int, int>& priorities,
                                               const std::unordered_map<int, bool>& activeStates) {
    std::lock_guard<std::mutex> lock(balancerMutex);

    // Clear previous allocations
    allocatedFps.clear();

    // Calculate total priority weight
    int totalPriorityWeight = 0;
    int activeChannels = 0;

    for (int channelIndex : channels) {
        auto priorityIt = priorities.find(channelIndex);
        auto activeIt = activeStates.find(channelIndex);

        int priority = (priorityIt != priorities.end()) ? priorityIt->second : 1;
        bool isActive = (activeIt != activeStates.end()) ? activeIt->second : false;

        totalPriorityWeight += priority * (isActive ? 2 : 1); // Active channels get double weight
        if (isActive) activeChannels++;
    }

    // Distribute FPS budget based on priority and active state
    for (int channelIndex : channels) {
        auto priorityIt = priorities.find(channelIndex);
        auto activeIt = activeStates.find(channelIndex);

        int priority = (priorityIt != priorities.end()) ? priorityIt->second : 1;
        bool isActive = (activeIt != activeStates.end()) ? activeIt->second : false;

        int weight = priority * (isActive ? 2 : 1);
        float allocatedFpsValue = (config.totalFpsBudget * weight) / totalPriorityWeight;

        // Clamp to configured limits
        allocatedFpsValue = std::max(static_cast<float>(config.minFpsPerChannel),
                                   std::min(static_cast<float>(config.maxFpsPerChannel), allocatedFpsValue));

        allocatedFps[channelIndex] = allocatedFpsValue;
    }

    LOGD("Rebalanced frame rates for %zu channels", channels.size());
}

float FrameRateLoadBalancer::getAllocatedFps(int channelIndex) const {
    std::lock_guard<std::mutex> lock(balancerMutex);
    auto it = allocatedFps.find(channelIndex);
    return (it != allocatedFps.end()) ? it->second : 30.0f;
}

void FrameRateLoadBalancer::setChannelFpsAllocation(int channelIndex, float fps) {
    std::lock_guard<std::mutex> lock(balancerMutex);
    allocatedFps[channelIndex] = std::max(static_cast<float>(config.minFpsPerChannel),
                                        std::min(static_cast<float>(config.maxFpsPerChannel), fps));
}

void FrameRateLoadBalancer::setLoadBalanceConfig(const LoadBalanceConfig& newConfig) {
    std::lock_guard<std::mutex> lock(balancerMutex);
    config = newConfig;
    LOGD("Load balance configuration updated");
}

FrameRateLoadBalancer::LoadBalanceConfig FrameRateLoadBalancer::getLoadBalanceConfig() const {
    std::lock_guard<std::mutex> lock(balancerMutex);
    return config;
}

float FrameRateLoadBalancer::getTotalAllocatedFps() const {
    std::lock_guard<std::mutex> lock(balancerMutex);
    float total = 0.0f;
    for (const auto& pair : allocatedFps) {
        total += pair.second;
    }
    return total;
}

float FrameRateLoadBalancer::getRemainingFpsBudget() const {
    return config.totalFpsBudget - getTotalAllocatedFps();
}

std::vector<std::pair<int, float>> FrameRateLoadBalancer::getFpsAllocationReport() const {
    std::lock_guard<std::mutex> lock(balancerMutex);
    std::vector<std::pair<int, float>> report;

    for (const auto& pair : allocatedFps) {
        report.push_back({pair.first, pair.second});
    }

    return report;
}
