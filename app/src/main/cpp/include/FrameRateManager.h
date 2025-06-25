#ifndef FRAME_RATE_MANAGER_H
#define FRAME_RATE_MANAGER_H

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <condition_variable>

/**
 * Intelligent Frame Rate Management System
 * Ensures 30FPS maintenance across multiple channels with adaptive optimization
 */
class FrameRateManager {
public:
    enum FrameRateStrategy {
        FIXED_30FPS = 0,        // Fixed 30 FPS for all channels
        ADAPTIVE = 1,           // Adaptive based on system load
        PRIORITY_BASED = 2,     // Priority-based allocation
        LOAD_BALANCED = 3       // Load-balanced across channels
    };

    struct ChannelFrameState {
        int channelIndex;
        float targetFps;
        float actualFps;
        float averageFrameTime;
        int priority;
        bool isActive;
        bool isVisible;
        std::chrono::steady_clock::time_point lastFrameTime;
        std::chrono::steady_clock::time_point lastFpsUpdate;
        int frameCount;
        int droppedFrames;
        float frameTimeVariance;
        
        ChannelFrameState() : channelIndex(-1), targetFps(30.0f), actualFps(0.0f),
                             averageFrameTime(33.33f), priority(1), isActive(false),
                             isVisible(true), frameCount(0), droppedFrames(0),
                             frameTimeVariance(0.0f) {
            lastFrameTime = std::chrono::steady_clock::now();
            lastFpsUpdate = lastFrameTime;
        }
    };

    struct SystemFrameMetrics {
        float totalSystemLoad;
        float averageSystemFps;
        int activeChannels;
        int totalFramesProcessed;
        int totalFramesDropped;
        float systemFrameTimeVariance;
        std::chrono::steady_clock::time_point lastUpdate;
        
        SystemFrameMetrics() : totalSystemLoad(0.0f), averageSystemFps(0.0f),
                              activeChannels(0), totalFramesProcessed(0),
                              totalFramesDropped(0), systemFrameTimeVariance(0.0f),
                              lastUpdate(std::chrono::steady_clock::now()) {}
    };

private:
    std::unordered_map<int, std::unique_ptr<ChannelFrameState>> channelStates;
    SystemFrameMetrics systemMetrics;
    mutable std::mutex statesMutex;
    mutable std::mutex metricsMutex;
    
    // Configuration
    std::atomic<FrameRateStrategy> strategy{ADAPTIVE};
    std::atomic<float> targetSystemFps{30.0f};
    std::atomic<float> systemLoadThreshold{0.8f};
    std::atomic<bool> adaptiveFrameSkippingEnabled{true};
    std::atomic<bool> loadBalancingEnabled{true};
    
    // Performance monitoring
    std::atomic<bool> monitoringActive{false};
    std::thread monitoringThread;
    std::condition_variable monitoringCv;
    std::mutex monitoringMutex;
    
    // Frame timing
    std::chrono::steady_clock::time_point systemStartTime;
    std::atomic<float> currentSystemLoad{0.0f};

public:
    FrameRateManager();
    ~FrameRateManager();

    // Channel management
    bool addChannel(int channelIndex, float targetFps = 30.0f, int priority = 1);
    bool removeChannel(int channelIndex);
    bool setChannelTargetFps(int channelIndex, float targetFps);
    bool setChannelPriority(int channelIndex, int priority);
    bool setChannelActive(int channelIndex, bool active);
    bool setChannelVisible(int channelIndex, bool visible);

    // Frame processing
    bool shouldProcessFrame(int channelIndex);
    void recordFrameProcessed(int channelIndex);
    void recordFrameDropped(int channelIndex);
    float getChannelFrameInterval(int channelIndex) const; // in milliseconds

    // System optimization
    void updateSystemLoad(float load);
    void optimizeFrameRates();
    void setFrameRateStrategy(FrameRateStrategy strategy);
    void setTargetSystemFps(float fps);
    void setSystemLoadThreshold(float threshold);

    // Monitoring and metrics
    void startMonitoring();
    void stopMonitoring();
    SystemFrameMetrics getSystemMetrics() const;
    ChannelFrameState getChannelState(int channelIndex) const;
    std::vector<int> getActiveChannels() const;
    std::vector<int> getSlowChannels(float thresholdFps = 25.0f) const;

    // Configuration
    void setAdaptiveFrameSkippingEnabled(bool enabled);
    void setLoadBalancingEnabled(bool enabled);
    void resetAllChannels();

private:
    // Internal helper methods
    ChannelFrameState* getChannelStateInternal(int channelIndex);
    const ChannelFrameState* getChannelStateInternal(int channelIndex) const;
    void updateChannelMetrics(int channelIndex);
    void updateSystemMetrics();
    void applyAdaptiveOptimization();
    void applyPriorityBasedOptimization();
    void applyLoadBalancedOptimization();
    float calculateOptimalFps(int channelIndex) const;
    bool shouldSkipFrame(int channelIndex) const;
    void monitoringLoop();
    bool validateChannelIndex(int channelIndex) const;
};

/**
 * Adaptive Frame Skipper
 * Implements intelligent frame skipping based on system performance
 */
class AdaptiveFrameSkipper {
public:
    struct SkippingConfig {
        float maxSkipRatio;        // Maximum ratio of frames to skip (0.0-1.0)
        float loadThreshold;       // System load threshold to start skipping
        int maxConsecutiveSkips;   // Maximum consecutive frames to skip
        bool prioritizeActiveChannels;
        
        SkippingConfig() : maxSkipRatio(0.5f), loadThreshold(0.7f),
                          maxConsecutiveSkips(2), prioritizeActiveChannels(true) {}
    };

private:
    std::unordered_map<int, int> consecutiveSkips;
    SkippingConfig config;
    mutable std::mutex skipperMutex;

public:
    AdaptiveFrameSkipper();
    ~AdaptiveFrameSkipper();

    // Frame skipping decisions
    bool shouldSkipFrame(int channelIndex, float systemLoad, bool isActiveChannel, float channelFps);
    void recordFrameSkipped(int channelIndex);
    void recordFrameProcessed(int channelIndex);
    
    // Configuration
    void setSkippingConfig(const SkippingConfig& config);
    SkippingConfig getSkippingConfig() const;
    void resetSkippingState();
    
    // Statistics
    int getConsecutiveSkips(int channelIndex) const;
    float calculateSkipRatio(int channelIndex, int totalFrames) const;
};

/**
 * Load Balancer for Frame Rate Distribution
 * Distributes frame rate budget across channels based on priority and performance
 */
class FrameRateLoadBalancer {
public:
    struct LoadBalanceConfig {
        float totalFpsBudget;      // Total FPS budget for all channels
        bool enableDynamicReallocation;
        float reallocationThreshold;
        int minFpsPerChannel;
        int maxFpsPerChannel;
        
        LoadBalanceConfig() : totalFpsBudget(480.0f), enableDynamicReallocation(true),
                             reallocationThreshold(0.8f), minFpsPerChannel(5),
                             maxFpsPerChannel(30) {}
    };

private:
    LoadBalanceConfig config;
    std::unordered_map<int, float> allocatedFps;
    mutable std::mutex balancerMutex;

public:
    FrameRateLoadBalancer();
    ~FrameRateLoadBalancer();

    // Load balancing
    void rebalanceFrameRates(const std::vector<int>& channels,
                           const std::unordered_map<int, int>& priorities,
                           const std::unordered_map<int, bool>& activeStates);
    float getAllocatedFps(int channelIndex) const;
    void setChannelFpsAllocation(int channelIndex, float fps);
    
    // Configuration
    void setLoadBalanceConfig(const LoadBalanceConfig& config);
    LoadBalanceConfig getLoadBalanceConfig() const;
    
    // Statistics
    float getTotalAllocatedFps() const;
    float getRemainingFpsBudget() const;
    std::vector<std::pair<int, float>> getFpsAllocationReport() const;
};

#endif // FRAME_RATE_MANAGER_H
