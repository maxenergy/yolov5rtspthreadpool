#ifndef AIBOX_SHARED_RESOURCE_POOL_H
#define AIBOX_SHARED_RESOURCE_POOL_H

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

#include "yolov5_thread_pool.h"
#include "mpp_decoder.h"
#include "ResourceManager.h"
#include "DecoderResourceSharing.h"
#include "log4c.h"

/**
 * Shared Resource Pool System
 * Centralized management of shared resources across multiple channels
 * Optimizes resource utilization and provides intelligent allocation strategies
 */
class SharedResourcePool {
public:
    enum PoolType {
        YOLOV5_THREAD_POOL = 0,
        MPP_DECODER_POOL = 1,
        MEMORY_BUFFER_POOL = 2,
        FRAME_BUFFER_POOL = 3,
        DETECTION_RESULT_POOL = 4
    };

    enum AllocationStrategy {
        ROUND_ROBIN = 0,
        LEAST_LOADED = 1,
        PRIORITY_BASED = 2,
        AFFINITY_BASED = 3,
        ADAPTIVE = 4
    };

    struct PoolConfiguration {
        PoolType type;
        int initialSize;
        int maxSize;
        int minSize;
        bool enableDynamicResize;
        bool enableLoadBalancing;
        AllocationStrategy strategy;
        int idleTimeoutMs;
        float utilizationThreshold;
        
        PoolConfiguration() : type(YOLOV5_THREAD_POOL), initialSize(4), maxSize(16), minSize(2),
                             enableDynamicResize(true), enableLoadBalancing(true),
                             strategy(ADAPTIVE), idleTimeoutMs(30000),
                             utilizationThreshold(0.8f) {}

        PoolConfiguration(PoolType t) : type(t), initialSize(4), maxSize(16), minSize(2),
                                      enableDynamicResize(true), enableLoadBalancing(true),
                                      strategy(ADAPTIVE), idleTimeoutMs(30000),
                                      utilizationThreshold(0.8f) {}
    };

    struct ResourceInstance {
        int instanceId;
        PoolType type;
        std::shared_ptr<void> resource;
        std::atomic<bool> inUse;
        std::atomic<int> assignedChannel;
        std::atomic<int> usageCount;
        std::chrono::steady_clock::time_point lastUsed;
        std::chrono::steady_clock::time_point createdTime;
        std::mutex instanceMutex;
        
        ResourceInstance(int id, PoolType t, std::shared_ptr<void> res) 
            : instanceId(id), type(t), resource(res), inUse(false), 
              assignedChannel(-1), usageCount(0) {
            lastUsed = createdTime = std::chrono::steady_clock::now();
        }
    };

    struct PoolStatistics {
        PoolType type;
        int totalInstances;
        int activeInstances;
        int idleInstances;
        float utilizationRate;
        float averageResponseTime;
        int totalRequests;
        int successfulAllocations;
        int failedAllocations;
        int dynamicExpansions;
        int dynamicShrinks;
        std::map<int, int> channelUsage; // channelIndex -> usage count
        
        PoolStatistics() : type(YOLOV5_THREAD_POOL), totalInstances(0), activeInstances(0),
                          idleInstances(0), utilizationRate(0.0f),
                          averageResponseTime(0.0f), totalRequests(0),
                          successfulAllocations(0), failedAllocations(0),
                          dynamicExpansions(0), dynamicShrinks(0) {}

        PoolStatistics(PoolType t) : type(t), totalInstances(0), activeInstances(0),
                                   idleInstances(0), utilizationRate(0.0f),
                                   averageResponseTime(0.0f), totalRequests(0),
                                   successfulAllocations(0), failedAllocations(0),
                                   dynamicExpansions(0), dynamicShrinks(0) {}
    };

    // Event listener interface
    class PoolEventListener {
    public:
        virtual ~PoolEventListener() = default;
        virtual void onResourceAllocated(PoolType type, int instanceId, int channelIndex) = 0;
        virtual void onResourceReleased(PoolType type, int instanceId, int channelIndex) = 0;
        virtual void onPoolExpanded(PoolType type, int newSize) = 0;
        virtual void onPoolShrunk(PoolType type, int newSize) = 0;
        virtual void onAllocationFailed(PoolType type, int channelIndex) = 0;
        virtual void onUtilizationAlert(PoolType type, float utilization) = 0;
    };

private:
    // Pool management
    std::map<PoolType, std::vector<std::unique_ptr<ResourceInstance>>> resourcePools;
    std::map<PoolType, PoolConfiguration> poolConfigs;
    std::map<PoolType, PoolStatistics> poolStats;
    mutable std::mutex poolsMutex;

    // Model data for YOLOv5 instances
    char* sharedModelData;
    int sharedModelSize;
    mutable std::mutex modelDataMutex;

    // Channel affinity tracking
    std::map<int, std::map<PoolType, int>> channelAffinity; // channelIndex -> poolType -> preferredInstanceId
    mutable std::mutex affinityMutex;
    
    // Management threads
    std::thread poolManagerThread;
    std::thread statisticsThread;
    std::atomic<bool> threadsRunning;
    std::condition_variable poolManagerCv;
    std::condition_variable statisticsCv;
    std::mutex threadMutex;
    
    // Event listener
    PoolEventListener* eventListener;
    
    // Performance monitoring
    std::map<PoolType, std::chrono::steady_clock::time_point> lastAllocationTime;
    std::map<PoolType, std::queue<float>> responseTimeHistory;
    mutable std::mutex performanceMutex;

public:
    SharedResourcePool();
    ~SharedResourcePool();
    
    // Initialization
    bool initialize(char* modelData, int modelSize);
    void cleanup();
    
    // Pool configuration
    bool createPool(PoolType type, const PoolConfiguration& config = PoolConfiguration(YOLOV5_THREAD_POOL));
    bool removePool(PoolType type);
    void setPoolConfiguration(PoolType type, const PoolConfiguration& config);
    PoolConfiguration getPoolConfiguration(PoolType type) const;
    
    // Resource allocation
    std::shared_ptr<void> allocateResource(PoolType type, int channelIndex, int priority = 1);
    bool releaseResource(PoolType type, std::shared_ptr<void> resource, int channelIndex);
    bool releaseChannelResources(int channelIndex);
    
    // Specialized allocation methods
    std::shared_ptr<Yolov5ThreadPool> allocateYolov5ThreadPool(int channelIndex, int priority = 1);
    std::shared_ptr<MppDecoder> allocateMppDecoder(int channelIndex, int priority = 1);
    std::shared_ptr<void> allocateMemoryBuffer(int channelIndex, size_t size);
    std::shared_ptr<frame_data_t> allocateFrameBuffer(int channelIndex);
    
    // Pool management
    bool expandPool(PoolType type, int additionalInstances);
    bool shrinkPool(PoolType type, int targetSize);
    void optimizePools();
    void balanceLoad();
    
    // Channel affinity
    void setChannelAffinity(int channelIndex, PoolType type, int instanceId);
    int getChannelAffinity(int channelIndex, PoolType type) const;
    void clearChannelAffinity(int channelIndex);
    
    // Statistics and monitoring
    PoolStatistics getPoolStatistics(PoolType type) const;
    std::map<PoolType, PoolStatistics> getAllPoolStatistics() const;
    float getPoolUtilization(PoolType type) const;
    std::vector<int> getActiveChannels() const;
    
    // Event handling
    void setEventListener(PoolEventListener* listener);
    
    // Performance optimization
    void enableLoadBalancing(PoolType type, bool enabled);
    void setAllocationStrategy(PoolType type, AllocationStrategy strategy);
    void adaptToSystemLoad();
    
    // Diagnostics
    std::string generatePoolReport() const;
    std::vector<std::string> getOptimizationRecommendations() const;

private:
    // Internal allocation logic
    ResourceInstance* findAvailableInstance(PoolType type, int channelIndex);
    ResourceInstance* selectInstanceByStrategy(PoolType type, int channelIndex, AllocationStrategy strategy);
    ResourceInstance* selectRoundRobin(PoolType type);
    ResourceInstance* selectLeastLoaded(PoolType type);
    ResourceInstance* selectByPriority(PoolType type, int channelIndex);
    ResourceInstance* selectByAffinity(PoolType type, int channelIndex);
    ResourceInstance* selectAdaptive(PoolType type, int channelIndex);
    
    // Resource creation
    std::shared_ptr<void> createResourceInstance(PoolType type);
    std::shared_ptr<Yolov5ThreadPool> createYolov5ThreadPool();
    std::shared_ptr<MppDecoder> createMppDecoder();
    std::shared_ptr<void> createMemoryBuffer(size_t size);
    std::shared_ptr<frame_data_t> createFrameBuffer();
    
    // Pool management
    void poolManagerLoop();
    void statisticsLoop();
    void updatePoolStatistics();
    void monitorPoolUtilization();
    void performDynamicResize();
    void reclaimIdleResources();
    
    // Utility methods
    std::vector<std::unique_ptr<ResourceInstance>>* getPool(PoolType type);
    const std::vector<std::unique_ptr<ResourceInstance>>* getPool(PoolType type) const;
    ResourceInstance* findInstanceById(PoolType type, int instanceId);
    ResourceInstance* findInstanceByResource(PoolType type, std::shared_ptr<void> resource);
    std::string poolTypeToString(PoolType type) const;
    std::string allocationStrategyToString(AllocationStrategy strategy) const;
    
    // Performance tracking
    void recordAllocationTime(PoolType type, float responseTime);
    float getAverageResponseTime(PoolType type) const;
    void updatePerformanceMetrics();
    
    // Event notifications
    void notifyResourceAllocated(PoolType type, int instanceId, int channelIndex);
    void notifyResourceReleased(PoolType type, int instanceId, int channelIndex);
    void notifyPoolExpanded(PoolType type, int newSize);
    void notifyPoolShrunk(PoolType type, int newSize);
    void notifyAllocationFailed(PoolType type, int channelIndex);
    void notifyUtilizationAlert(PoolType type, float utilization);
};

/**
 * Resource Pool Manager
 * High-level interface for managing multiple resource pools
 */
class ResourcePoolManager {
public:
    struct SystemConfiguration {
        int maxChannels;
        bool enableGlobalOptimization;
        bool enableCrossPoolBalancing;
        float globalUtilizationThreshold;
        int optimizationIntervalMs;
        
        SystemConfiguration() : maxChannels(16), enableGlobalOptimization(true),
                              enableCrossPoolBalancing(true), globalUtilizationThreshold(0.85f),
                              optimizationIntervalMs(5000) {}
    };

private:
    std::unique_ptr<SharedResourcePool> sharedPool;
    std::unique_ptr<ResourceManager> resourceManager;
    std::unique_ptr<DecoderResourceSharing> decoderSharing;
    SystemConfiguration systemConfig;
    mutable std::mutex managerMutex;

public:
    ResourcePoolManager();
    ~ResourcePoolManager();
    
    // Initialization
    bool initialize(char* modelData, int modelSize, const SystemConfiguration& config = SystemConfiguration());
    void cleanup();
    
    // High-level resource allocation
    bool allocateChannelResources(int channelIndex, int priority = 1);
    bool releaseChannelResources(int channelIndex);
    
    // Resource access
    std::shared_ptr<Yolov5ThreadPool> getYolov5ThreadPool(int channelIndex);
    std::shared_ptr<MppDecoder> getMppDecoder(int channelIndex);
    
    // System optimization
    void optimizeSystemResources();
    void balanceSystemLoad();
    
    // Configuration
    void setSystemConfiguration(const SystemConfiguration& config);
    SystemConfiguration getSystemConfiguration() const;
    
    // Monitoring
    std::string generateSystemReport() const;
    std::vector<std::string> getSystemRecommendations() const;
};

#endif // AIBOX_SHARED_RESOURCE_POOL_H
