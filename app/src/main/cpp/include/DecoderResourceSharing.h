#ifndef AIBOX_DECODER_RESOURCE_SHARING_H
#define AIBOX_DECODER_RESOURCE_SHARING_H

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

#include "mpp_decoder.h"
#include "DecoderManager.h"
#include "log4c.h"

/**
 * Advanced Decoder Resource Sharing System
 * Optimizes MPP decoder resource allocation and sharing across multiple streams
 */
class DecoderResourceSharing {
public:
    enum SharingStrategy {
        EXCLUSIVE = 0,      // Each channel gets its own decoder
        SHARED_POOL = 1,    // Decoders are shared from a common pool
        ADAPTIVE = 2,       // Dynamically switch between exclusive and shared
        PRIORITY_BASED = 3, // Share based on channel priority
        LOAD_BALANCED = 4   // Balance load across available decoders
    };

    enum DecoderType {
        H264_DECODER = 0,
        H265_DECODER = 1,
        GENERIC_DECODER = 2
    };

    struct DecoderResourceConfig {
        SharingStrategy strategy;
        int maxDecodersPerType;
        int maxSharedDecoders;
        int minDecodersPerChannel;
        int maxDecodersPerChannel;
        bool enableDynamicAllocation;
        bool enableResourcePreemption;
        float resourceUtilizationThreshold;
        int idleTimeoutMs;
        
        DecoderResourceConfig() : strategy(ADAPTIVE), maxDecodersPerType(8),
                                maxSharedDecoders(16), minDecodersPerChannel(1),
                                maxDecodersPerChannel(4), enableDynamicAllocation(true),
                                enableResourcePreemption(false), resourceUtilizationThreshold(0.8f),
                                idleTimeoutMs(30000) {}
    };

    struct ChannelDecoderInfo {
        int channelIndex;
        DecoderType decoderType;
        int priority;
        bool exclusiveAccess;
        std::vector<std::shared_ptr<MppDecoder>> assignedDecoders;
        std::queue<std::shared_ptr<MppDecoder>> availableDecoders;
        std::atomic<int> activeDecoders;
        std::atomic<long> totalFramesDecoded;
        std::atomic<long> totalDecodeTime;
        std::chrono::steady_clock::time_point lastUsed;
        std::mutex channelMutex;
        
        ChannelDecoderInfo(int index, DecoderType type) : channelIndex(index), decoderType(type),
                                                        priority(1), exclusiveAccess(false),
                                                        activeDecoders(0), totalFramesDecoded(0),
                                                        totalDecodeTime(0) {
            lastUsed = std::chrono::steady_clock::now();
        }
    };

    struct SharedDecoderPool {
        DecoderType type;
        std::vector<std::shared_ptr<MppDecoder>> decoders;
        std::queue<std::shared_ptr<MppDecoder>> availableDecoders;
        std::map<int, std::shared_ptr<MppDecoder>> activeAssignments; // channelIndex -> decoder
        std::atomic<int> totalDecoders;
        std::atomic<int> availableCount;
        std::atomic<int> activeCount;
        std::mutex poolMutex;
        
        SharedDecoderPool(DecoderType t) : type(t), totalDecoders(0), availableCount(0), activeCount(0) {}
    };

    struct ResourceStatistics {
        int totalDecoders;
        int activeDecoders;
        int idleDecoders;
        float averageUtilization;
        float peakUtilization;
        long totalFramesDecoded;
        float averageDecodeTime;
        int resourceContentions;
        int preemptions;
        std::map<DecoderType, int> decodersByType;
        std::map<int, float> channelUtilization;
        
        ResourceStatistics() : totalDecoders(0), activeDecoders(0), idleDecoders(0),
                             averageUtilization(0.0f), peakUtilization(0.0f),
                             totalFramesDecoded(0), averageDecodeTime(0.0f),
                             resourceContentions(0), preemptions(0) {}
    };

    // Event listener interface
    class ResourceSharingEventListener {
    public:
        virtual ~ResourceSharingEventListener() = default;
        virtual void onDecoderAssigned(int channelIndex, std::shared_ptr<MppDecoder> decoder) = 0;
        virtual void onDecoderReleased(int channelIndex, std::shared_ptr<MppDecoder> decoder) = 0;
        virtual void onResourceContention(int channelIndex, DecoderType type) = 0;
        virtual void onResourcePreemption(int fromChannel, int toChannel, std::shared_ptr<MppDecoder> decoder) = 0;
        virtual void onPoolExpanded(DecoderType type, int newSize) = 0;
        virtual void onPoolShrunk(DecoderType type, int newSize) = 0;
    };

private:
    // Configuration
    DecoderResourceConfig config;
    mutable std::mutex configMutex;

    // Channel management
    std::map<int, std::unique_ptr<ChannelDecoderInfo>> channels;
    mutable std::mutex channelsMutex;

    // Shared decoder pools
    std::map<DecoderType, std::unique_ptr<SharedDecoderPool>> sharedPools;
    mutable std::mutex poolsMutex;

    // Resource monitoring
    ResourceStatistics statistics;
    mutable std::mutex statisticsMutex;
    
    // Management threads
    std::thread resourceManagerThread;
    std::thread statisticsThread;
    std::atomic<bool> threadsRunning;
    std::condition_variable resourceManagerCv;
    std::condition_variable statisticsCv;
    std::mutex threadMutex;
    
    // Event listener
    ResourceSharingEventListener* eventListener;

public:
    DecoderResourceSharing();
    ~DecoderResourceSharing();
    
    // Initialization
    bool initialize(const DecoderResourceConfig& config = DecoderResourceConfig());
    void cleanup();
    
    // Channel management
    bool addChannel(int channelIndex, DecoderType type, int priority = 1);
    bool removeChannel(int channelIndex);
    bool setChannelPriority(int channelIndex, int priority);
    bool setChannelExclusiveAccess(int channelIndex, bool exclusive);
    
    // Decoder allocation
    std::shared_ptr<MppDecoder> acquireDecoder(int channelIndex);
    bool releaseDecoder(int channelIndex, std::shared_ptr<MppDecoder> decoder);
    bool preemptDecoder(int fromChannel, int toChannel);
    
    // Pool management
    bool expandPool(DecoderType type, int additionalDecoders);
    bool shrinkPool(DecoderType type, int targetSize);
    void optimizePools();
    
    // Configuration
    void setSharingStrategy(SharingStrategy strategy);
    SharingStrategy getSharingStrategy() const;
    void setResourceConfig(const DecoderResourceConfig& config);
    DecoderResourceConfig getResourceConfig() const;
    
    // Statistics and monitoring
    ResourceStatistics getResourceStatistics() const;
    float getChannelUtilization(int channelIndex) const;
    std::vector<int> getActiveChannels() const;
    int getAvailableDecoders(DecoderType type) const;
    
    // Event handling
    void setEventListener(ResourceSharingEventListener* listener);
    
    // Performance optimization
    void balanceLoad();
    void reclaimIdleResources();
    void adaptToSystemLoad();
    
    // Diagnostics
    std::string generateResourceReport() const;
    std::vector<std::string> getOptimizationRecommendations() const;

private:
    // Internal allocation logic
    std::shared_ptr<MppDecoder> allocateExclusiveDecoder(int channelIndex);
    std::shared_ptr<MppDecoder> allocateFromSharedPool(int channelIndex);
    std::shared_ptr<MppDecoder> allocateAdaptive(int channelIndex);
    std::shared_ptr<MppDecoder> allocatePriorityBased(int channelIndex);
    std::shared_ptr<MppDecoder> allocateLoadBalanced(int channelIndex);
    
    // Pool management
    SharedDecoderPool* getSharedPool(DecoderType type);
    bool createSharedPool(DecoderType type, int initialSize);
    std::shared_ptr<MppDecoder> createDecoder(DecoderType type);
    
    // Resource monitoring
    void resourceManagerLoop();
    void statisticsLoop();
    void updateStatistics();
    void monitorResourceUtilization();
    void detectResourceContentions();
    
    // Optimization algorithms
    void performLoadBalancing();
    void reclaimIdleDecoders();
    void adaptPoolSizes();
    std::vector<int> identifyLowPriorityChannels() const;
    std::vector<int> identifyHighUtilizationChannels() const;
    
    // Utility methods
    ChannelDecoderInfo* getChannelInfo(int channelIndex);
    const ChannelDecoderInfo* getChannelInfo(int channelIndex) const;
    bool validateChannelIndex(int channelIndex) const;
    std::string decoderTypeToString(DecoderType type) const;
    std::string sharingStrategyToString(SharingStrategy strategy) const;
    
    // Event notifications
    void notifyDecoderAssigned(int channelIndex, std::shared_ptr<MppDecoder> decoder);
    void notifyDecoderReleased(int channelIndex, std::shared_ptr<MppDecoder> decoder);
    void notifyResourceContention(int channelIndex, DecoderType type);
    void notifyResourcePreemption(int fromChannel, int toChannel, std::shared_ptr<MppDecoder> decoder);
    void notifyPoolExpanded(DecoderType type, int newSize);
    void notifyPoolShrunk(DecoderType type, int newSize);
};

/**
 * Decoder Performance Optimizer
 * Optimizes decoder performance based on usage patterns and system load
 */
class DecoderPerformanceOptimizer {
public:
    struct OptimizationMetrics {
        float decodeLatency;
        float throughput;
        float resourceEfficiency;
        float memoryUsage;
        int queueDepth;
        
        OptimizationMetrics() : decodeLatency(0.0f), throughput(0.0f),
                              resourceEfficiency(0.0f), memoryUsage(0.0f), queueDepth(0) {}
    };

private:
    DecoderResourceSharing* resourceSharing;
    std::map<int, OptimizationMetrics> channelMetrics;
    mutable std::mutex metricsMutex;

public:
    DecoderPerformanceOptimizer(DecoderResourceSharing* sharing);
    ~DecoderPerformanceOptimizer();
    
    // Optimization control
    void startOptimization();
    void stopOptimization();
    
    // Metrics collection
    void updateChannelMetrics(int channelIndex, const OptimizationMetrics& metrics);
    OptimizationMetrics getChannelMetrics(int channelIndex) const;
    
    // Optimization algorithms
    void optimizeChannelPerformance(int channelIndex);
    void optimizeSystemPerformance();
    std::vector<std::string> generateOptimizationRecommendations() const;

private:
    void optimizationLoop();
    void analyzePerformancePatterns();
    void adjustResourceAllocation();
};

#endif // AIBOX_DECODER_RESOURCE_SHARING_H
