#include "ChannelStateManager.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

ChannelStateManager::ChannelStateManager()
    : eventListener(nullptr), monitorRunning(false), healthCheckIntervalMs(2000),
      frameTimeoutMs(5000), stateHistoryLimit(50) {
    LOGD("ChannelStateManager created");
}

ChannelStateManager::~ChannelStateManager() {
    cleanup();
    LOGD("ChannelStateManager destroyed");
}

bool ChannelStateManager::initialize() {
    // Start monitoring thread
    monitorRunning = true;
    monitorThread = std::thread(&ChannelStateManager::monitoringLoop, this);
    reconnectThread = std::thread(&ChannelStateManager::reconnectionLoop, this);
    
    LOGD("ChannelStateManager initialized");
    return true;
}

void ChannelStateManager::cleanup() {
    // Stop threads
    monitorRunning = false;
    monitorCv.notify_all();
    reconnectCv.notify_all();
    
    if (monitorThread.joinable()) {
        monitorThread.join();
    }
    
    if (reconnectThread.joinable()) {
        reconnectThread.join();
    }
    
    // Clear channels
    {
        std::lock_guard<std::mutex> lock(channelsMutex);
        channels.clear();
    }
    
    LOGD("ChannelStateManager cleanup completed");
}

bool ChannelStateManager::addChannel(int channelIndex, const ReconnectionPolicy& policy) {
    if (!validateChannelIndex(channelIndex)) {
        LOGE("Invalid channel index: %d", channelIndex);
        return false;
    }
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    
    if (channels.find(channelIndex) != channels.end()) {
        LOGW("Channel %d already exists", channelIndex);
        return false;
    }
    
    auto channelInfo = std::make_unique<ChannelStateInfo>(channelIndex);
    channelInfo->reconnectionPolicy = policy;
    
    channels[channelIndex] = std::move(channelInfo);
    
    LOGD("Added channel %d to state manager", channelIndex);
    return true;
}

bool ChannelStateManager::removeChannel(int channelIndex) {
    std::lock_guard<std::mutex> lock(channelsMutex);
    
    auto it = channels.find(channelIndex);
    if (it == channels.end()) {
        LOGW("Channel %d not found", channelIndex);
        return false;
    }
    
    // Set state to destroyed before removal
    changeState(it->second.get(), DESTROYED, "Channel removed");
    
    channels.erase(it);
    
    LOGD("Removed channel %d from state manager", channelIndex);
    return true;
}

bool ChannelStateManager::setState(int channelIndex, ChannelState newState, const std::string& reason) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        LOGE("Channel %d not found", channelIndex);
        return false;
    }
    
    changeState(channelInfo, newState, reason);
    return true;
}

ChannelStateManager::ChannelState ChannelStateManager::getState(int channelIndex) const {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return INACTIVE;
    }
    
    std::lock_guard<std::mutex> lock(channelInfo->stateMutex);
    return channelInfo->currentState;
}

void ChannelStateManager::updateHealthMetrics(int channelIndex, float frameRate, int droppedFrames, double latency) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(channelInfo->stateMutex);
    
    channelInfo->healthMetrics.frameRate = frameRate;
    channelInfo->healthMetrics.droppedFrames += droppedFrames;
    channelInfo->healthMetrics.averageLatency = latency;
    channelInfo->healthMetrics.lastHealthCheck = std::chrono::steady_clock::now();
    
    // Update health status based on metrics
    updateHealthStatus(channelInfo);
    
    LOGD("Updated health metrics for channel %d: FPS=%.2f, Drops=%d, Latency=%.2fms", 
         channelIndex, frameRate, droppedFrames, latency);
}

void ChannelStateManager::reportError(int channelIndex, const std::string& error) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(channelInfo->stateMutex);
    
    channelInfo->healthMetrics.errorCount++;
    channelInfo->lastError = error;
    
    // Add to recent errors list
    channelInfo->healthMetrics.recentErrors.push_back(error);
    if (channelInfo->healthMetrics.recentErrors.size() > 10) {
        channelInfo->healthMetrics.recentErrors.erase(channelInfo->healthMetrics.recentErrors.begin());
    }
    
    // Update health status
    updateHealthStatus(channelInfo);
    
    // Trigger state change to ERROR if not already
    if (channelInfo->currentState != ERROR && channelInfo->currentState != DESTROYED) {
        changeState(channelInfo, ERROR, error);
        
        // Schedule reconnection if enabled
        if (channelInfo->reconnectionPolicy.enabled) {
            triggerReconnection(channelIndex, error);
        }
    }
    
    LOGE("Error reported for channel %d: %s", channelIndex, error.c_str());
}

void ChannelStateManager::reportFrameReceived(int channelIndex) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(channelInfo->stateMutex);
    channelInfo->healthMetrics.lastFrameTime = std::chrono::steady_clock::now();
}

void ChannelStateManager::triggerReconnection(int channelIndex, const std::string& reason) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo || !channelInfo->reconnectionPolicy.enabled) {
        return;
    }
    
    if (!shouldAttemptReconnection(channelInfo)) {
        LOGW("Reconnection not allowed for channel %d (max attempts reached)", channelIndex);
        notifyReconnectionFailed(channelIndex, "Max attempts reached");
        return;
    }
    
    changeState(channelInfo, RECONNECTING, reason);
    
    {
        std::lock_guard<std::mutex> lock(reconnectMutex);
        reconnectQueue.push(channelIndex);
    }
    reconnectCv.notify_one();
    
    LOGD("Triggered reconnection for channel %d: %s", channelIndex, reason.c_str());
}

void ChannelStateManager::monitoringLoop() {
    while (monitorRunning) {
        std::unique_lock<std::mutex> lock(monitorMutex);
        monitorCv.wait_for(lock, std::chrono::milliseconds(healthCheckIntervalMs), 
                          [this] { return !monitorRunning; });
        
        if (!monitorRunning) break;
        
        // Check health of all channels
        std::lock_guard<std::mutex> channelsLock(channelsMutex);
        for (auto& pair : channels) {
            checkChannelHealth(pair.second.get());
        }
    }
}

void ChannelStateManager::checkChannelHealth(ChannelStateInfo* channelInfo) {
    if (!channelInfo) return;
    
    std::lock_guard<std::mutex> lock(channelInfo->stateMutex);
    
    // Check for frame timeout
    if (channelInfo->currentState == ACTIVE && isChannelTimedOut(channelInfo)) {
        LOGW("Frame timeout detected for channel %d", channelInfo->channelIndex);
        
        channelInfo->healthMetrics.errorCount++;
        updateHealthStatus(channelInfo);
        
        notifyChannelTimeout(channelInfo->channelIndex, frameTimeoutMs);
        
        // Trigger reconnection if enabled
        if (channelInfo->reconnectionPolicy.enabled) {
            changeState(channelInfo, ERROR, "Frame timeout");
            triggerReconnection(channelInfo->channelIndex, "Frame timeout");
        }
    }
    
    // Update health status based on current metrics
    updateHealthStatus(channelInfo);
}

bool ChannelStateManager::isChannelTimedOut(const ChannelStateInfo* channelInfo) const {
    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastFrame = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - channelInfo->healthMetrics.lastFrameTime);
    
    return timeSinceLastFrame.count() > frameTimeoutMs;
}

void ChannelStateManager::updateHealthStatus(ChannelStateInfo* channelInfo) {
    if (!channelInfo) return;
    
    HealthStatus oldStatus = channelInfo->healthMetrics.status;
    HealthStatus newStatus = HEALTHY;
    
    // Determine health status based on metrics
    if (channelInfo->healthMetrics.errorCount > 10) {
        newStatus = FAILED;
    } else if (channelInfo->healthMetrics.errorCount > 5 || 
               channelInfo->healthMetrics.frameRate < 15.0f ||
               channelInfo->healthMetrics.droppedFrames > 100) {
        newStatus = CRITICAL;
    } else if (channelInfo->healthMetrics.errorCount > 2 || 
               channelInfo->healthMetrics.frameRate < 25.0f ||
               channelInfo->healthMetrics.droppedFrames > 50) {
        newStatus = WARNING;
    }
    
    if (newStatus != oldStatus) {
        channelInfo->healthMetrics.status = newStatus;
        notifyHealthStatusChanged(channelInfo->channelIndex, oldStatus, newStatus);
    }
}

void ChannelStateManager::reconnectionLoop() {
    while (monitorRunning) {
        std::unique_lock<std::mutex> lock(reconnectMutex);
        reconnectCv.wait(lock, [this] { return !reconnectQueue.empty() || !monitorRunning; });
        
        if (!monitorRunning) break;
        
        if (!reconnectQueue.empty()) {
            int channelIndex = reconnectQueue.front();
            reconnectQueue.pop();
            lock.unlock();
            
            processReconnection(channelIndex);
        }
    }
}

void ChannelStateManager::processReconnection(int channelIndex) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo || !shouldAttemptReconnection(channelInfo)) {
        return;
    }
    
    int attemptNumber = channelInfo->reconnectAttempts.load() + 1;
    channelInfo->reconnectAttempts = attemptNumber;
    
    int delay = calculateReconnectDelay(channelInfo);
    
    notifyReconnectionAttempt(channelIndex, attemptNumber, channelInfo->reconnectionPolicy.maxAttempts);
    
    LOGD("Reconnection attempt %d/%d for channel %d (delay: %dms)", 
         attemptNumber, channelInfo->reconnectionPolicy.maxAttempts, channelIndex, delay);
    
    // Wait before attempting reconnection
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    
    // Update last reconnect time
    {
        std::lock_guard<std::mutex> lock(channelInfo->stateMutex);
        channelInfo->lastReconnectTime = std::chrono::steady_clock::now();
    }
    
    // The actual reconnection logic would be handled by the calling system
    // Here we just change state to CONNECTING to indicate reconnection is starting
    changeState(channelInfo, CONNECTING, "Reconnection attempt " + std::to_string(attemptNumber));
}

int ChannelStateManager::calculateReconnectDelay(const ChannelStateInfo* channelInfo) const {
    if (!channelInfo) return 1000;
    
    const auto& policy = channelInfo->reconnectionPolicy;
    int attemptNumber = channelInfo->reconnectAttempts.load();
    
    if (!policy.customDelays.empty() && attemptNumber <= policy.customDelays.size()) {
        return policy.customDelays[attemptNumber - 1];
    }
    
    if (policy.exponentialBackoff) {
        int delay = policy.baseDelayMs;
        for (int i = 1; i < attemptNumber; i++) {
            delay = static_cast<int>(delay * policy.backoffMultiplier);
        }
        return std::min(delay, policy.maxDelayMs);
    } else {
        return policy.baseDelayMs;
    }
}

bool ChannelStateManager::shouldAttemptReconnection(const ChannelStateInfo* channelInfo) const {
    if (!channelInfo || !channelInfo->reconnectionPolicy.enabled) {
        return false;
    }
    
    return channelInfo->reconnectAttempts.load() < channelInfo->reconnectionPolicy.maxAttempts;
}

void ChannelStateManager::changeState(ChannelStateInfo* channelInfo, ChannelState newState, const std::string& reason) {
    if (!channelInfo) return;
    
    std::lock_guard<std::mutex> lock(channelInfo->stateMutex);
    
    if (channelInfo->currentState == newState) {
        return; // No change needed
    }
    
    ChannelState oldState = channelInfo->currentState;
    channelInfo->previousState = oldState;
    channelInfo->currentState = newState;
    channelInfo->stateChangeTime = std::chrono::steady_clock::now();
    
    // Add to state history
    addStateToHistory(channelInfo, oldState, newState, reason);
    
    // Reset reconnect attempts on successful connection
    if (newState == ACTIVE) {
        channelInfo->reconnectAttempts = 0;
        channelInfo->healthMetrics.errorCount = 0;
        channelInfo->healthMetrics.recentErrors.clear();
        
        if (oldState == RECONNECTING) {
            notifyReconnectionSuccess(channelInfo->channelIndex, channelInfo->reconnectAttempts.load());
        }
    }
    
    notifyStateChanged(channelInfo->channelIndex, oldState, newState, reason);
    
    LOGD("Channel %d state changed: %s -> %s (%s)", 
         channelInfo->channelIndex, stateToString(oldState).c_str(), 
         stateToString(newState).c_str(), reason.c_str());
}

void ChannelStateManager::addStateToHistory(ChannelStateInfo* channelInfo, ChannelState fromState, 
                                           ChannelState toState, const std::string& reason) {
    if (!channelInfo) return;
    
    channelInfo->stateHistory.emplace_back(channelInfo->channelIndex, fromState, toState, reason);
    
    // Limit history size
    if (channelInfo->stateHistory.size() > stateHistoryLimit) {
        channelInfo->stateHistory.erase(channelInfo->stateHistory.begin());
    }
}

ChannelStateManager::ChannelStateInfo* ChannelStateManager::getChannelInfo(int channelIndex) {
    std::lock_guard<std::mutex> lock(channelsMutex);
    auto it = channels.find(channelIndex);
    return (it != channels.end()) ? it->second.get() : nullptr;
}

const ChannelStateManager::ChannelStateInfo* ChannelStateManager::getChannelInfo(int channelIndex) const {
    std::lock_guard<std::mutex> lock(channelsMutex);
    auto it = channels.find(channelIndex);
    return (it != channels.end()) ? it->second.get() : nullptr;
}

std::string ChannelStateManager::stateToString(ChannelState state) const {
    switch (state) {
        case INACTIVE: return "INACTIVE";
        case INITIALIZING: return "INITIALIZING";
        case CONNECTING: return "CONNECTING";
        case ACTIVE: return "ACTIVE";
        case PAUSED: return "PAUSED";
        case ERROR: return "ERROR";
        case RECONNECTING: return "RECONNECTING";
        case DESTROYED: return "DESTROYED";
        default: return "UNKNOWN";
    }
}

std::string ChannelStateManager::healthStatusToString(HealthStatus status) const {
    switch (status) {
        case HEALTHY: return "HEALTHY";
        case WARNING: return "WARNING";
        case CRITICAL: return "CRITICAL";
        case FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

bool ChannelStateManager::validateChannelIndex(int channelIndex) const {
    return channelIndex >= 0 && channelIndex < 16; // Support up to 16 channels
}

std::vector<int> ChannelStateManager::getActiveChannels() const {
    std::vector<int> activeChannels;
    std::lock_guard<std::mutex> lock(channelsMutex);
    
    for (const auto& pair : channels) {
        if (pair.second->currentState == ACTIVE) {
            activeChannels.push_back(pair.first);
        }
    }
    
    return activeChannels;
}

std::string ChannelStateManager::generateStateReport() const {
    std::ostringstream report;
    
    report << "=== Channel State Manager Report ===\n";
    report << "Total Channels: " << channels.size() << "\n";
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    
    for (const auto& pair : channels) {
        const auto& channelInfo = pair.second;
        std::lock_guard<std::mutex> stateLock(channelInfo->stateMutex);
        
        report << "\nChannel " << channelInfo->channelIndex << ":\n";
        report << "  State: " << stateToString(channelInfo->currentState) << "\n";
        report << "  Health: " << healthStatusToString(channelInfo->healthMetrics.status) << "\n";
        report << "  FPS: " << std::fixed << std::setprecision(2) << channelInfo->healthMetrics.frameRate << "\n";
        report << "  Errors: " << channelInfo->healthMetrics.errorCount << "\n";
        report << "  Reconnect Attempts: " << channelInfo->reconnectAttempts.load() << "\n";
        
        if (!channelInfo->lastError.empty()) {
            report << "  Last Error: " << channelInfo->lastError << "\n";
        }
    }
    
    return report.str();
}

// Event notification methods
void ChannelStateManager::notifyStateChanged(int channelIndex, ChannelState oldState, ChannelState newState, const std::string& reason) {
    if (eventListener) {
        eventListener->onStateChanged(channelIndex, oldState, newState, reason);
    }
}

void ChannelStateManager::notifyHealthStatusChanged(int channelIndex, HealthStatus oldStatus, HealthStatus newStatus) {
    if (eventListener) {
        eventListener->onHealthStatusChanged(channelIndex, oldStatus, newStatus);
    }
}

void ChannelStateManager::notifyReconnectionAttempt(int channelIndex, int attemptNumber, int maxAttempts) {
    if (eventListener) {
        eventListener->onReconnectionAttempt(channelIndex, attemptNumber, maxAttempts);
    }
}

void ChannelStateManager::notifyReconnectionSuccess(int channelIndex, int totalAttempts) {
    if (eventListener) {
        eventListener->onReconnectionSuccess(channelIndex, totalAttempts);
    }
}

void ChannelStateManager::notifyReconnectionFailed(int channelIndex, const std::string& reason) {
    if (eventListener) {
        eventListener->onReconnectionFailed(channelIndex, reason);
    }
}

void ChannelStateManager::notifyChannelTimeout(int channelIndex, int timeoutMs) {
    if (eventListener) {
        eventListener->onChannelTimeout(channelIndex, timeoutMs);
    }
}

// Additional methods implementation
bool ChannelStateManager::isChannelRegistered(int channelIndex) const {
    std::lock_guard<std::mutex> lock(channelsMutex);
    return channels.find(channelIndex) != channels.end();
}

ChannelStateManager::ChannelState ChannelStateManager::getPreviousState(int channelIndex) const {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return INACTIVE;
    }

    std::lock_guard<std::mutex> lock(channelInfo->stateMutex);
    return channelInfo->previousState;
}

std::vector<ChannelStateManager::StateTransition> ChannelStateManager::getStateHistory(int channelIndex) const {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return {};
    }

    std::lock_guard<std::mutex> lock(channelInfo->stateMutex);
    return channelInfo->stateHistory;
}

ChannelStateManager::HealthStatus ChannelStateManager::getHealthStatus(int channelIndex) const {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return FAILED;
    }

    std::lock_guard<std::mutex> lock(channelInfo->stateMutex);
    return channelInfo->healthMetrics.status;
}

ChannelStateManager::ChannelHealthMetrics ChannelStateManager::getHealthMetrics(int channelIndex) const {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return ChannelHealthMetrics(channelIndex);
    }

    std::lock_guard<std::mutex> lock(channelInfo->stateMutex);
    return channelInfo->healthMetrics;
}

void ChannelStateManager::setReconnectionPolicy(int channelIndex, const ReconnectionPolicy& policy) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (channelInfo) {
        std::lock_guard<std::mutex> lock(channelInfo->stateMutex);
        channelInfo->reconnectionPolicy = policy;
        LOGD("Updated reconnection policy for channel %d", channelIndex);
    }
}

ChannelStateManager::ReconnectionPolicy ChannelStateManager::getReconnectionPolicy(int channelIndex) const {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return ReconnectionPolicy();
    }

    std::lock_guard<std::mutex> lock(channelInfo->stateMutex);
    return channelInfo->reconnectionPolicy;
}

void ChannelStateManager::cancelReconnection(int channelIndex) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (channelInfo) {
        std::lock_guard<std::mutex> lock(channelInfo->stateMutex);
        channelInfo->reconnectAttempts = channelInfo->reconnectionPolicy.maxAttempts; // Prevent further attempts
        LOGD("Cancelled reconnection for channel %d", channelIndex);
    }
}

int ChannelStateManager::getReconnectAttempts(int channelIndex) const {
    auto channelInfo = getChannelInfo(channelIndex);
    return channelInfo ? channelInfo->reconnectAttempts.load() : 0;
}

std::vector<int> ChannelStateManager::getErrorChannels() const {
    std::vector<int> errorChannels;
    std::lock_guard<std::mutex> lock(channelsMutex);

    for (const auto& pair : channels) {
        if (pair.second->currentState == ERROR) {
            errorChannels.push_back(pair.first);
        }
    }

    return errorChannels;
}

std::vector<int> ChannelStateManager::getReconnectingChannels() const {
    std::vector<int> reconnectingChannels;
    std::lock_guard<std::mutex> lock(channelsMutex);

    for (const auto& pair : channels) {
        if (pair.second->currentState == RECONNECTING) {
            reconnectingChannels.push_back(pair.first);
        }
    }

    return reconnectingChannels;
}

int ChannelStateManager::getTotalChannelCount() const {
    std::lock_guard<std::mutex> lock(channelsMutex);
    return channels.size();
}

void ChannelStateManager::setEventListener(StateEventListener* listener) {
    eventListener = listener;
}

void ChannelStateManager::setHealthCheckInterval(int intervalMs) {
    healthCheckIntervalMs = intervalMs;
    LOGD("Health check interval set to %dms", intervalMs);
}

void ChannelStateManager::setFrameTimeout(int timeoutMs) {
    frameTimeoutMs = timeoutMs;
    LOGD("Frame timeout set to %dms", timeoutMs);
}

void ChannelStateManager::setStateHistoryLimit(int limit) {
    stateHistoryLimit = limit;
    LOGD("State history limit set to %d", limit);
}

std::string ChannelStateManager::generateHealthReport() const {
    std::ostringstream report;

    report << "=== Channel Health Report ===\n";

    std::lock_guard<std::mutex> lock(channelsMutex);

    int healthyCount = 0, warningCount = 0, criticalCount = 0, failedCount = 0;

    for (const auto& pair : channels) {
        const auto& channelInfo = pair.second;
        std::lock_guard<std::mutex> stateLock(channelInfo->stateMutex);

        switch (channelInfo->healthMetrics.status) {
            case HEALTHY: healthyCount++; break;
            case WARNING: warningCount++; break;
            case CRITICAL: criticalCount++; break;
            case FAILED: failedCount++; break;
        }
    }

    report << "Health Summary:\n";
    report << "  Healthy: " << healthyCount << "\n";
    report << "  Warning: " << warningCount << "\n";
    report << "  Critical: " << criticalCount << "\n";
    report << "  Failed: " << failedCount << "\n\n";

    for (const auto& pair : channels) {
        const auto& channelInfo = pair.second;
        std::lock_guard<std::mutex> stateLock(channelInfo->stateMutex);

        if (channelInfo->healthMetrics.status != HEALTHY) {
            report << "Channel " << channelInfo->channelIndex << " ("
                   << healthStatusToString(channelInfo->healthMetrics.status) << "):\n";
            report << "  FPS: " << channelInfo->healthMetrics.frameRate << "\n";
            report << "  Dropped Frames: " << channelInfo->healthMetrics.droppedFrames << "\n";
            report << "  Error Count: " << channelInfo->healthMetrics.errorCount << "\n";
            report << "  Latency: " << channelInfo->healthMetrics.averageLatency << "ms\n";

            if (!channelInfo->healthMetrics.recentErrors.empty()) {
                report << "  Recent Errors:\n";
                for (const auto& error : channelInfo->healthMetrics.recentErrors) {
                    report << "    - " << error << "\n";
                }
            }
            report << "\n";
        }
    }

    return report.str();
}

std::vector<std::string> ChannelStateManager::getSystemRecommendations() const {
    std::vector<std::string> recommendations;

    std::lock_guard<std::mutex> lock(channelsMutex);

    int errorChannelCount = 0;
    int reconnectingChannelCount = 0;
    int lowFpsChannelCount = 0;

    for (const auto& pair : channels) {
        const auto& channelInfo = pair.second;
        std::lock_guard<std::mutex> stateLock(channelInfo->stateMutex);

        if (channelInfo->currentState == ERROR) {
            errorChannelCount++;
        } else if (channelInfo->currentState == RECONNECTING) {
            reconnectingChannelCount++;
        }

        if (channelInfo->healthMetrics.frameRate < 15.0f && channelInfo->currentState == ACTIVE) {
            lowFpsChannelCount++;
        }
    }

    if (errorChannelCount > channels.size() * 0.3f) {
        recommendations.push_back("High number of error channels detected. Check network connectivity and stream sources.");
    }

    if (reconnectingChannelCount > 3) {
        recommendations.push_back("Multiple channels are reconnecting. Consider checking system resources and network stability.");
    }

    if (lowFpsChannelCount > 0) {
        recommendations.push_back("Some channels have low frame rates. Consider optimizing processing or reducing channel count.");
    }

    return recommendations;
}

// ChannelStateStatistics implementation
ChannelStateStatistics::ChannelStateStatistics(ChannelStateManager* manager) : stateManager(manager) {
    LOGD("ChannelStateStatistics created");
}

ChannelStateStatistics::~ChannelStateStatistics() {
    LOGD("ChannelStateStatistics destroyed");
}

void ChannelStateStatistics::recordStateChange(int channelIndex, ChannelStateManager::ChannelState fromState,
                                              ChannelStateManager::ChannelState toState) {
    std::lock_guard<std::mutex> lock(statsMutex);

    auto& stats = channelStats[channelIndex];
    stats.stateOccurrences[toState]++;
    stats.totalStateChanges++;

    LOGD("Recorded state change for channel %d: %d -> %d", channelIndex, fromState, toState);
}

void ChannelStateStatistics::recordReconnectionAttempt(int channelIndex) {
    std::lock_guard<std::mutex> lock(statsMutex);

    auto& stats = channelStats[channelIndex];
    stats.totalReconnections++;

    LOGD("Recorded reconnection attempt for channel %d", channelIndex);
}

void ChannelStateStatistics::recordReconnectionResult(int channelIndex, bool success, float duration) {
    std::lock_guard<std::mutex> lock(statsMutex);

    auto& stats = channelStats[channelIndex];
    if (success) {
        stats.successfulReconnections++;
    }

    // Update average reconnection time
    if (stats.totalReconnections > 0) {
        stats.averageReconnectionTime =
            (stats.averageReconnectionTime * (stats.totalReconnections - 1) + duration) / stats.totalReconnections;
    }

    LOGD("Recorded reconnection result for channel %d: %s (%.2fs)",
         channelIndex, success ? "SUCCESS" : "FAILED", duration);
}

ChannelStateStatistics::StateStatistics ChannelStateStatistics::getChannelStatistics(int channelIndex) const {
    std::lock_guard<std::mutex> lock(statsMutex);

    auto it = channelStats.find(channelIndex);
    if (it != channelStats.end()) {
        return it->second;
    }

    return StateStatistics();
}

std::map<int, ChannelStateStatistics::StateStatistics> ChannelStateStatistics::getAllChannelStatistics() const {
    std::lock_guard<std::mutex> lock(statsMutex);
    return channelStats;
}

std::vector<int> ChannelStateStatistics::getMostUnstableChannels() const {
    std::vector<std::pair<int, int>> channelStability;

    {
        std::lock_guard<std::mutex> lock(statsMutex);
        for (const auto& pair : channelStats) {
            channelStability.emplace_back(pair.first, pair.second.totalStateChanges);
        }
    }

    // Sort by state changes (descending)
    std::sort(channelStability.begin(), channelStability.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<int> unstableChannels;
    for (const auto& pair : channelStability) {
        if (pair.second > 10) { // Threshold for instability
            unstableChannels.push_back(pair.first);
        }
    }

    return unstableChannels;
}

float ChannelStateStatistics::getSystemStabilityScore() const {
    std::lock_guard<std::mutex> lock(statsMutex);

    if (channelStats.empty()) return 1.0f;

    float totalScore = 0.0f;
    for (const auto& pair : channelStats) {
        const auto& stats = pair.second;

        // Calculate stability score based on state changes and reconnections
        float channelScore = 1.0f;
        if (stats.totalStateChanges > 0) {
            channelScore -= std::min(0.8f, stats.totalStateChanges * 0.05f);
        }
        if (stats.totalReconnections > 0) {
            channelScore -= std::min(0.5f, stats.totalReconnections * 0.1f);
        }

        totalScore += std::max(0.0f, channelScore);
    }

    return totalScore / channelStats.size();
}

std::string ChannelStateStatistics::generateStatisticsReport() const {
    std::ostringstream report;

    report << "=== Channel State Statistics Report ===\n";

    std::lock_guard<std::mutex> lock(statsMutex);

    for (const auto& pair : channelStats) {
        const auto& stats = pair.second;

        report << "\nChannel " << pair.first << ":\n";
        report << "  Total State Changes: " << stats.totalStateChanges << "\n";
        report << "  Total Reconnections: " << stats.totalReconnections << "\n";
        report << "  Successful Reconnections: " << stats.successfulReconnections << "\n";
        report << "  Average Reconnection Time: " << std::fixed << std::setprecision(2)
               << stats.averageReconnectionTime << "s\n";

        if (!stats.stateOccurrences.empty()) {
            report << "  State Occurrences:\n";
            for (const auto& statePair : stats.stateOccurrences) {
                report << "    State " << statePair.first << ": " << statePair.second << " times\n";
            }
        }
    }

    report << "\nSystem Stability Score: " << std::fixed << std::setprecision(3)
           << getSystemStabilityScore() << "\n";

    return report.str();
}

void ChannelStateStatistics::resetStatistics() {
    std::lock_guard<std::mutex> lock(statsMutex);
    channelStats.clear();
    LOGD("Reset all channel statistics");
}

void ChannelStateStatistics::resetChannelStatistics(int channelIndex) {
    std::lock_guard<std::mutex> lock(statsMutex);
    channelStats.erase(channelIndex);
    LOGD("Reset statistics for channel %d", channelIndex);
}
