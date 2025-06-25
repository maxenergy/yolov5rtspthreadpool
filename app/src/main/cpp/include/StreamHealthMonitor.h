#ifndef AIBOX_STREAM_HEALTH_MONITOR_H
#define AIBOX_STREAM_HEALTH_MONITOR_H

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
 * Stream Health Monitor for comprehensive stream monitoring and diagnostics
 * Provides real-time health assessment, anomaly detection, and recovery mechanisms
 */
class StreamHealthMonitor {
public:
    enum HealthStatus {
        HEALTHY = 0,
        WARNING = 1,
        CRITICAL = 2,
        FAILED = 3,
        UNKNOWN = 4
    };

    enum HealthMetric {
        FRAME_RATE = 0,
        FRAME_DROPS = 1,
        LATENCY = 2,
        BANDWIDTH = 3,
        ERROR_RATE = 4,
        CONNECTION_STABILITY = 5,
        MEMORY_USAGE = 6,
        CPU_USAGE = 7
    };

    struct HealthData {
        int channelIndex;
        HealthStatus overallStatus;
        std::map<HealthMetric, float> metrics;
        std::map<HealthMetric, HealthStatus> metricStatus;
        std::chrono::steady_clock::time_point lastUpdate;
        std::chrono::steady_clock::time_point lastHealthyTime;
        int consecutiveFailures;
        std::vector<std::string> activeAlerts;
        std::string lastError;
        
        // Performance statistics
        float averageFps;
        float peakFps;
        float minFps;
        long totalFrames;
        long droppedFrames;
        double averageLatency;
        double peakLatency;
        long totalBytes;
        int reconnectCount;
        
        HealthData(int index) : channelIndex(index), overallStatus(UNKNOWN),
                               consecutiveFailures(0), averageFps(0.0f), peakFps(0.0f), minFps(0.0f),
                               totalFrames(0), droppedFrames(0), averageLatency(0.0), peakLatency(0.0),
                               totalBytes(0), reconnectCount(0) {
            lastUpdate = std::chrono::steady_clock::now();
            lastHealthyTime = std::chrono::steady_clock::now();
        }
    };

    struct HealthThresholds {
        float minFps = 15.0f;
        float maxDropRate = 0.05f;  // 5%
        double maxLatency = 500.0;  // 500ms
        float maxErrorRate = 0.02f; // 2%
        int maxConsecutiveFailures = 3;
        int healthCheckInterval = 1000; // 1 second
        int criticalThreshold = 5000;   // 5 seconds
        
        HealthThresholds() = default;
    };

    // Callback interface for health events
    class HealthEventListener {
    public:
        virtual ~HealthEventListener() = default;
        virtual void onHealthStatusChanged(int channelIndex, HealthStatus oldStatus, HealthStatus newStatus) = 0;
        virtual void onHealthAlert(int channelIndex, HealthMetric metric, const std::string& message) = 0;
        virtual void onHealthRecovered(int channelIndex, HealthMetric metric) = 0;
        virtual void onStreamFailure(int channelIndex, const std::string& reason) = 0;
        virtual void onRecoveryAction(int channelIndex, const std::string& action) = 0;
    };

private:
    std::map<int, std::unique_ptr<HealthData>> healthDataMap;
    std::mutex healthDataMutex;
    
    // Monitoring thread
    std::thread monitorThread;
    std::atomic<bool> shouldStop;
    std::condition_variable monitorCv;
    std::mutex monitorMutex;
    
    // Alert management
    std::queue<std::pair<int, std::string>> alertQueue;
    std::mutex alertMutex;
    std::condition_variable alertCv;
    std::thread alertProcessorThread;
    
    // Configuration
    HealthThresholds thresholds;
    HealthEventListener* eventListener;
    
    // Statistics
    std::atomic<int> totalChannels;
    std::atomic<int> healthyChannels;
    std::atomic<int> warningChannels;
    std::atomic<int> criticalChannels;
    std::atomic<int> failedChannels;

public:
    StreamHealthMonitor();
    ~StreamHealthMonitor();
    
    // Channel management
    bool addChannel(int channelIndex);
    bool removeChannel(int channelIndex);
    
    // Health data updates
    void updateFrameRate(int channelIndex, float fps);
    void updateFrameDrops(int channelIndex, int dropped, int total);
    void updateLatency(int channelIndex, double latencyMs);
    void updateBandwidth(int channelIndex, long bytes);
    void updateErrorRate(int channelIndex, int errors, int total);
    void updateConnectionStatus(int channelIndex, bool connected);
    void updateResourceUsage(int channelIndex, float cpuUsage, long memoryUsage);
    
    // Health queries
    HealthStatus getChannelHealth(int channelIndex) const;
    HealthData getChannelHealthData(int channelIndex) const;
    std::vector<int> getChannelsByStatus(HealthStatus status) const;
    std::vector<std::string> getActiveAlerts(int channelIndex) const;
    
    // System-wide health
    HealthStatus getSystemHealth() const;
    int getHealthyChannelCount() const { return healthyChannels.load(); }
    int getWarningChannelCount() const { return warningChannels.load(); }
    int getCriticalChannelCount() const { return criticalChannels.load(); }
    int getFailedChannelCount() const { return failedChannels.load(); }
    
    // Configuration
    void setHealthThresholds(const HealthThresholds& thresholds);
    void setEventListener(HealthEventListener* listener);
    void setMonitoringInterval(int intervalMs);
    
    // Recovery actions
    void triggerRecoveryAction(int channelIndex, const std::string& action);
    void resetChannelHealth(int channelIndex);
    void acknowledgeAlert(int channelIndex, HealthMetric metric);
    
    // Diagnostics
    std::string generateHealthReport() const;
    std::string generateChannelDiagnostics(int channelIndex) const;
    void exportHealthData(const std::string& filename) const;
    
    // Cleanup
    void cleanup();

private:
    // Internal monitoring
    void monitorLoop();
    void checkChannelHealth(HealthData* healthData);
    void updateOverallHealth(HealthData* healthData);
    void detectAnomalies(HealthData* healthData);
    
    // Alert processing
    void alertProcessorLoop();
    void processAlert(int channelIndex, const std::string& message);
    void addAlert(HealthData* healthData, HealthMetric metric, const std::string& message);
    void removeAlert(HealthData* healthData, HealthMetric metric);
    
    // Health assessment
    HealthStatus assessMetricHealth(HealthMetric metric, float value) const;
    HealthStatus combineHealthStatus(const std::vector<HealthStatus>& statuses) const;
    void updateSystemStatistics();
    
    // Utility methods
    HealthData* getHealthData(int channelIndex);
    const HealthData* getHealthData(int channelIndex) const;
    std::string healthStatusToString(HealthStatus status) const;
    std::string healthMetricToString(HealthMetric metric) const;
    
    // Thread safety helpers
    std::unique_lock<std::mutex> lockHealthData() { return std::unique_lock<std::mutex>(healthDataMutex); }
};

/**
 * Stream Anomaly Detector for advanced pattern recognition
 */
class StreamAnomalyDetector {
public:
    struct AnomalyPattern {
        std::string name;
        std::string description;
        std::function<bool(const StreamHealthMonitor::HealthData&)> detector;
        StreamHealthMonitor::HealthStatus severity;
        
        AnomalyPattern(const std::string& n, const std::string& desc, 
                      std::function<bool(const StreamHealthMonitor::HealthData&)> det,
                      StreamHealthMonitor::HealthStatus sev)
            : name(n), description(desc), detector(det), severity(sev) {}
    };

private:
    std::vector<AnomalyPattern> patterns;
    std::mutex patternsMutex;

public:
    StreamAnomalyDetector();
    
    // Pattern management
    void addPattern(const AnomalyPattern& pattern);
    void removePattern(const std::string& name);
    
    // Anomaly detection
    std::vector<std::string> detectAnomalies(const StreamHealthMonitor::HealthData& healthData);
    bool hasAnomalies(const StreamHealthMonitor::HealthData& healthData);
    
    // Built-in patterns
    void initializeBuiltInPatterns();

private:
    // Built-in anomaly patterns
    bool detectFrameRateFluctuation(const StreamHealthMonitor::HealthData& data);
    bool detectHighLatencySpikes(const StreamHealthMonitor::HealthData& data);
    bool detectConnectionInstability(const StreamHealthMonitor::HealthData& data);
    bool detectMemoryLeak(const StreamHealthMonitor::HealthData& data);
};

/**
 * Stream Recovery Manager for automated recovery actions
 */
class StreamRecoveryManager {
public:
    enum RecoveryAction {
        RESTART_STREAM = 0,
        REDUCE_QUALITY = 1,
        INCREASE_BUFFER = 2,
        RESET_DECODER = 3,
        RECONNECT = 4,
        CLEAR_CACHE = 5,
        ADJUST_BITRATE = 6
    };

    struct RecoveryStrategy {
        std::string name;
        std::vector<RecoveryAction> actions;
        int maxAttempts;
        int delayBetweenAttempts; // milliseconds

        // Default constructor
        RecoveryStrategy() : name(""), actions(), maxAttempts(3), delayBetweenAttempts(5000) {}

        RecoveryStrategy(const std::string& n, const std::vector<RecoveryAction>& acts,
                        int maxAtt = 3, int delay = 5000)
            : name(n), actions(acts), maxAttempts(maxAtt), delayBetweenAttempts(delay) {}
    };

private:
    std::map<StreamHealthMonitor::HealthStatus, RecoveryStrategy> strategies;
    std::map<int, int> recoveryAttempts;
    std::mutex recoveryMutex;

public:
    StreamRecoveryManager();
    
    // Strategy management
    void addRecoveryStrategy(StreamHealthMonitor::HealthStatus status, const RecoveryStrategy& strategy);
    void removeRecoveryStrategy(StreamHealthMonitor::HealthStatus status);
    
    // Recovery execution
    bool executeRecovery(int channelIndex, StreamHealthMonitor::HealthStatus status);
    void resetRecoveryAttempts(int channelIndex);
    int getRecoveryAttempts(int channelIndex) const;
    
    // Built-in strategies
    void initializeBuiltInStrategies();

private:
    bool executeRecoveryAction(int channelIndex, RecoveryAction action);
    std::string recoveryActionToString(RecoveryAction action) const;
};

#endif // AIBOX_STREAM_HEALTH_MONITOR_H
