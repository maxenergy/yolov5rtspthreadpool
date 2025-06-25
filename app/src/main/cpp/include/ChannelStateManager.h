#ifndef AIBOX_CHANNEL_STATE_MANAGER_H
#define AIBOX_CHANNEL_STATE_MANAGER_H

#include <memory>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <queue>

#include "log4c.h"

/**
 * Enhanced Channel State Manager
 * Provides comprehensive state tracking, automatic reconnection, and health monitoring
 */
class ChannelStateManager {
public:
    enum ChannelState {
        INACTIVE = 0,
        INITIALIZING = 1,
        CONNECTING = 2,
        ACTIVE = 3,
        PAUSED = 4,
        ERROR = 5,
        RECONNECTING = 6,
        DESTROYED = 7
    };

    enum HealthStatus {
        HEALTHY = 0,
        WARNING = 1,
        CRITICAL = 2,
        FAILED = 3
    };

    struct StateTransition {
        int channelIndex;
        ChannelState fromState;
        ChannelState toState;
        std::chrono::steady_clock::time_point timestamp;
        std::string reason;
        
        StateTransition(int channel, ChannelState from, ChannelState to, const std::string& r = "")
            : channelIndex(channel), fromState(from), toState(to), reason(r) {
            timestamp = std::chrono::steady_clock::now();
        }
    };

    struct ChannelHealthMetrics {
        int channelIndex;
        HealthStatus status;
        float frameRate;
        int droppedFrames;
        int errorCount;
        double averageLatency;
        std::chrono::steady_clock::time_point lastFrameTime;
        std::chrono::steady_clock::time_point lastHealthCheck;
        std::vector<std::string> recentErrors;
        
        ChannelHealthMetrics(int index) : channelIndex(index), status(HEALTHY),
                                        frameRate(0.0f), droppedFrames(0), errorCount(0),
                                        averageLatency(0.0) {
            lastFrameTime = lastHealthCheck = std::chrono::steady_clock::now();
        }
    };

    struct ReconnectionPolicy {
        bool enabled;
        int maxAttempts;
        int baseDelayMs;
        int maxDelayMs;
        float backoffMultiplier;
        bool exponentialBackoff;
        std::vector<int> customDelays; // Custom delay sequence
        
        ReconnectionPolicy() : enabled(true), maxAttempts(5), baseDelayMs(1000),
                             maxDelayMs(30000), backoffMultiplier(2.0f),
                             exponentialBackoff(true) {}
    };

    struct ChannelStateInfo {
        int channelIndex;
        ChannelState currentState;
        ChannelState previousState;
        ChannelHealthMetrics healthMetrics;
        ReconnectionPolicy reconnectionPolicy;
        std::atomic<int> reconnectAttempts;
        std::chrono::steady_clock::time_point stateChangeTime;
        std::chrono::steady_clock::time_point lastReconnectTime;
        std::vector<StateTransition> stateHistory;
        std::string lastError;
        mutable std::mutex stateMutex;
        
        ChannelStateInfo(int index) : channelIndex(index), currentState(INACTIVE),
                                    previousState(INACTIVE), healthMetrics(index),
                                    reconnectAttempts(0) {
            stateChangeTime = lastReconnectTime = std::chrono::steady_clock::now();
        }
    };

    // Event listener interface
    class StateEventListener {
    public:
        virtual ~StateEventListener() = default;
        virtual void onStateChanged(int channelIndex, ChannelState oldState, ChannelState newState, const std::string& reason) = 0;
        virtual void onHealthStatusChanged(int channelIndex, HealthStatus oldStatus, HealthStatus newStatus) = 0;
        virtual void onReconnectionAttempt(int channelIndex, int attemptNumber, int maxAttempts) = 0;
        virtual void onReconnectionSuccess(int channelIndex, int totalAttempts) = 0;
        virtual void onReconnectionFailed(int channelIndex, const std::string& reason) = 0;
        virtual void onChannelTimeout(int channelIndex, int timeoutMs) = 0;
    };

private:
    std::map<int, std::unique_ptr<ChannelStateInfo>> channels;
    mutable std::mutex channelsMutex;
    
    // State monitoring
    std::thread monitorThread;
    std::atomic<bool> monitorRunning;
    std::condition_variable monitorCv;
    std::mutex monitorMutex;
    
    // Reconnection management
    std::thread reconnectThread;
    std::queue<int> reconnectQueue;
    std::condition_variable reconnectCv;
    std::mutex reconnectMutex;
    
    // Event listener
    StateEventListener* eventListener;
    
    // Configuration
    int healthCheckIntervalMs;
    int frameTimeoutMs;
    int stateHistoryLimit;

public:
    ChannelStateManager();
    ~ChannelStateManager();
    
    // Initialization
    bool initialize();
    void cleanup();
    
    // Channel management
    bool addChannel(int channelIndex, const ReconnectionPolicy& policy = ReconnectionPolicy());
    bool removeChannel(int channelIndex);
    bool isChannelRegistered(int channelIndex) const;
    
    // State management
    bool setState(int channelIndex, ChannelState newState, const std::string& reason = "");
    ChannelState getState(int channelIndex) const;
    ChannelState getPreviousState(int channelIndex) const;
    std::vector<StateTransition> getStateHistory(int channelIndex) const;
    
    // Health monitoring
    void updateHealthMetrics(int channelIndex, float frameRate, int droppedFrames, double latency);
    void reportError(int channelIndex, const std::string& error);
    void reportFrameReceived(int channelIndex);
    HealthStatus getHealthStatus(int channelIndex) const;
    ChannelHealthMetrics getHealthMetrics(int channelIndex) const;
    
    // Reconnection control
    void setReconnectionPolicy(int channelIndex, const ReconnectionPolicy& policy);
    ReconnectionPolicy getReconnectionPolicy(int channelIndex) const;
    void triggerReconnection(int channelIndex, const std::string& reason = "");
    void cancelReconnection(int channelIndex);
    int getReconnectAttempts(int channelIndex) const;
    
    // System status
    std::vector<int> getActiveChannels() const;
    std::vector<int> getErrorChannels() const;
    std::vector<int> getReconnectingChannels() const;
    int getTotalChannelCount() const;
    
    // Event handling
    void setEventListener(StateEventListener* listener);
    
    // Configuration
    void setHealthCheckInterval(int intervalMs);
    void setFrameTimeout(int timeoutMs);
    void setStateHistoryLimit(int limit);
    
    // Diagnostics
    std::string generateStateReport() const;
    std::string generateHealthReport() const;
    std::vector<std::string> getSystemRecommendations() const;

private:
    // Internal state management
    void changeState(ChannelStateInfo* channelInfo, ChannelState newState, const std::string& reason);
    void addStateToHistory(ChannelStateInfo* channelInfo, ChannelState fromState, ChannelState toState, const std::string& reason);
    
    // Health monitoring
    void monitoringLoop();
    void checkChannelHealth(ChannelStateInfo* channelInfo);
    void updateHealthStatus(ChannelStateInfo* channelInfo);
    bool isChannelTimedOut(const ChannelStateInfo* channelInfo) const;
    
    // Reconnection management
    void reconnectionLoop();
    void processReconnection(int channelIndex);
    int calculateReconnectDelay(const ChannelStateInfo* channelInfo) const;
    bool shouldAttemptReconnection(const ChannelStateInfo* channelInfo) const;
    
    // Utility methods
    ChannelStateInfo* getChannelInfo(int channelIndex);
    const ChannelStateInfo* getChannelInfo(int channelIndex) const;
    std::string stateToString(ChannelState state) const;
    std::string healthStatusToString(HealthStatus status) const;
    bool validateChannelIndex(int channelIndex) const;
    
    // Event notifications
    void notifyStateChanged(int channelIndex, ChannelState oldState, ChannelState newState, const std::string& reason);
    void notifyHealthStatusChanged(int channelIndex, HealthStatus oldStatus, HealthStatus newStatus);
    void notifyReconnectionAttempt(int channelIndex, int attemptNumber, int maxAttempts);
    void notifyReconnectionSuccess(int channelIndex, int totalAttempts);
    void notifyReconnectionFailed(int channelIndex, const std::string& reason);
    void notifyChannelTimeout(int channelIndex, int timeoutMs);
};

/**
 * Channel State Statistics Collector
 * Collects and analyzes channel state statistics for system optimization
 */
class ChannelStateStatistics {
public:
    struct StateStatistics {
        std::map<ChannelStateManager::ChannelState, int> stateOccurrences;
        std::map<ChannelStateManager::ChannelState, long> totalTimeInState; // milliseconds
        int totalStateChanges;
        int totalReconnections;
        int successfulReconnections;
        float averageReconnectionTime;
        std::chrono::steady_clock::time_point collectionStart;
        
        StateStatistics() : totalStateChanges(0), totalReconnections(0),
                          successfulReconnections(0), averageReconnectionTime(0.0f) {
            collectionStart = std::chrono::steady_clock::now();
        }
    };

private:
    std::map<int, StateStatistics> channelStats;
    mutable std::mutex statsMutex;
    ChannelStateManager* stateManager;

public:
    ChannelStateStatistics(ChannelStateManager* manager);
    ~ChannelStateStatistics();
    
    // Statistics collection
    void recordStateChange(int channelIndex, ChannelStateManager::ChannelState fromState, 
                          ChannelStateManager::ChannelState toState);
    void recordReconnectionAttempt(int channelIndex);
    void recordReconnectionResult(int channelIndex, bool success, float duration);
    
    // Statistics retrieval
    StateStatistics getChannelStatistics(int channelIndex) const;
    std::map<int, StateStatistics> getAllChannelStatistics() const;
    
    // Analysis
    std::vector<int> getMostUnstableChannels() const;
    std::vector<int> getChannelsWithHighReconnectionRate() const;
    float getSystemStabilityScore() const;
    
    // Reporting
    std::string generateStatisticsReport() const;
    void resetStatistics();
    void resetChannelStatistics(int channelIndex);
};

#endif // AIBOX_CHANNEL_STATE_MANAGER_H
