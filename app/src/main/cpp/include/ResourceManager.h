#ifndef AIBOX_RESOURCE_MANAGER_H
#define AIBOX_RESOURCE_MANAGER_H

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
 * Resource Manager for handling resource sharing and isolation across multiple streams
 * Provides memory management, CPU allocation, and resource monitoring
 */
class ResourceManager {
public:
    enum ResourceType {
        MEMORY = 0,
        CPU = 1,
        GPU = 2,
        DECODER = 3,
        ENCODER = 4,
        NETWORK = 5,
        STORAGE = 6
    };

    enum AllocationStrategy {
        FAIR_SHARE = 0,
        PRIORITY_BASED = 1,
        DEMAND_BASED = 2,
        ADAPTIVE = 3
    };

    struct ResourceQuota {
        ResourceType type;
        long maxAmount;      // Maximum amount available
        long currentUsage;   // Current usage
        long reserved;       // Reserved amount
        std::map<int, long> channelAllocations; // Per-channel allocations
        
        ResourceQuota(ResourceType t, long max) 
            : type(t), maxAmount(max), currentUsage(0), reserved(0) {}
    };

    struct ChannelResourceInfo {
        int channelIndex;
        int priority;
        std::map<ResourceType, long> allocatedResources;
        std::map<ResourceType, long> requestedResources;
        std::map<ResourceType, long> actualUsage;
        std::chrono::steady_clock::time_point lastUpdate;
        bool isActive;
        
        ChannelResourceInfo(int index, int prio = 1) 
            : channelIndex(index), priority(prio), isActive(false) {
            lastUpdate = std::chrono::steady_clock::now();
        }
    };

    // Callback interface for resource events
    class ResourceEventListener {
    public:
        virtual ~ResourceEventListener() = default;
        virtual void onResourceAllocated(int channelIndex, ResourceType type, long amount) = 0;
        virtual void onResourceDeallocated(int channelIndex, ResourceType type, long amount) = 0;
        virtual void onResourceExhausted(ResourceType type, long requested, long available) = 0;
        virtual void onResourceRebalanced(const std::vector<int>& affectedChannels) = 0;
    };

private:
    std::map<ResourceType, std::unique_ptr<ResourceQuota>> resourceQuotas;
    std::map<int, std::unique_ptr<ChannelResourceInfo>> channelResources;
    std::mutex resourceMutex;
    
    // Resource monitoring
    std::thread monitorThread;
    std::atomic<bool> shouldStop;
    std::condition_variable monitorCv;
    std::mutex monitorMutex;
    
    // Allocation strategy
    AllocationStrategy currentStrategy;
    ResourceEventListener* eventListener;
    
    // System limits
    long totalSystemMemory;
    int totalCpuCores;
    int maxConcurrentChannels;

public:
    ResourceManager(long systemMemory = 1024 * 1024 * 1024, // 1GB default
                   int cpuCores = 4, int maxChannels = 16);
    ~ResourceManager();
    
    // Resource configuration
    void setResourceLimit(ResourceType type, long maxAmount);
    void setAllocationStrategy(AllocationStrategy strategy);
    void setChannelPriority(int channelIndex, int priority);
    
    // Channel management
    bool addChannel(int channelIndex, int priority = 1);
    bool removeChannel(int channelIndex);
    bool activateChannel(int channelIndex);
    bool deactivateChannel(int channelIndex);
    
    // Resource allocation
    bool allocateResource(int channelIndex, ResourceType type, long amount);
    bool deallocateResource(int channelIndex, ResourceType type, long amount);
    bool requestResource(int channelIndex, ResourceType type, long amount);
    void updateResourceUsage(int channelIndex, ResourceType type, long actualUsage);
    
    // Resource queries
    long getAvailableResource(ResourceType type) const;
    long getAllocatedResource(int channelIndex, ResourceType type) const;
    long getTotalResourceUsage(ResourceType type) const;
    float getResourceUtilization(ResourceType type) const;
    
    // Resource optimization
    void rebalanceResources();
    void optimizeResourceAllocation();
    void enforceResourceLimits();
    
    // Monitoring and statistics
    std::vector<int> getActiveChannels() const;
    std::map<ResourceType, float> getSystemResourceUtilization() const;
    std::string generateResourceReport() const;
    
    // Event handling
    void setEventListener(ResourceEventListener* listener);
    
    // Cleanup
    void cleanup();

private:
    // Internal allocation logic
    bool performAllocation(int channelIndex, ResourceType type, long amount);
    bool performDeallocation(int channelIndex, ResourceType type, long amount);
    long calculateOptimalAllocation(int channelIndex, ResourceType type, long requested);
    
    // Allocation strategies
    long fairShareAllocation(ResourceType type, long requested, int totalChannels);
    long priorityBasedAllocation(int channelIndex, ResourceType type, long requested);
    long demandBasedAllocation(int channelIndex, ResourceType type, long requested);
    long adaptiveAllocation(int channelIndex, ResourceType type, long requested);
    
    // Resource monitoring
    void monitorLoop();
    void updateSystemResourceUsage();
    void detectResourceLeaks();
    void enforceQuotas();
    
    // Utility methods
    ResourceQuota* getResourceQuota(ResourceType type);
    const ResourceQuota* getResourceQuota(ResourceType type) const;
    ChannelResourceInfo* getChannelResourceInfo(int channelIndex);
    const ChannelResourceInfo* getChannelResourceInfo(int channelIndex) const;
    std::string resourceTypeToString(ResourceType type) const;
    
    // Thread safety helpers
    std::unique_lock<std::mutex> lockResources() { return std::unique_lock<std::mutex>(resourceMutex); }
};

/**
 * Memory Pool Manager for efficient memory allocation and reuse
 */
class MemoryPoolManager {
public:
    struct MemoryBlock {
        void* data;
        size_t size;
        bool inUse;
        int channelIndex;
        std::chrono::steady_clock::time_point lastUsed;
        
        MemoryBlock(size_t s) : data(nullptr), size(s), inUse(false), channelIndex(-1) {
            data = malloc(s);
            lastUsed = std::chrono::steady_clock::now();
        }
        
        ~MemoryBlock() {
            if (data) {
                free(data);
                data = nullptr;
            }
        }
    };

private:
    std::vector<std::unique_ptr<MemoryBlock>> memoryBlocks;
    std::mutex poolMutex;
    size_t totalPoolSize;
    size_t maxPoolSize;
    size_t blockSize;

public:
    MemoryPoolManager(size_t maxSize = 256 * 1024 * 1024, // 256MB default
                     size_t blockSize = 1024 * 1024);     // 1MB blocks
    ~MemoryPoolManager();
    
    // Memory allocation
    void* allocateBlock(int channelIndex, size_t size);
    void deallocateBlock(void* ptr);
    void deallocateChannelBlocks(int channelIndex);
    
    // Pool management
    void expandPool(size_t additionalSize);
    void shrinkPool(size_t targetSize);
    void cleanupUnusedBlocks();
    
    // Statistics
    size_t getTotalPoolSize() const { return totalPoolSize; }
    size_t getUsedPoolSize() const;
    size_t getAvailablePoolSize() const;
    int getBlockCount() const;
    int getUsedBlockCount() const;

private:
    MemoryBlock* findAvailableBlock(size_t size);
    MemoryBlock* findBlockByPointer(void* ptr);
    void createNewBlock(size_t size);
    void removeOldestUnusedBlock();
};

/**
 * CPU Resource Allocator for managing CPU core allocation
 */
class CPUResourceAllocator {
public:
    struct CPUAllocation {
        int channelIndex;
        std::vector<int> assignedCores;
        float cpuQuota; // Percentage of CPU time
        int priority;
        
        CPUAllocation(int index, float quota, int prio) 
            : channelIndex(index), cpuQuota(quota), priority(prio) {}
    };

private:
    std::map<int, std::unique_ptr<CPUAllocation>> allocations;
    std::mutex allocationMutex;
    int totalCores;
    std::vector<bool> coreUsage; // Track which cores are in use

public:
    CPUResourceAllocator(int cores);
    ~CPUResourceAllocator();
    
    // CPU allocation
    bool allocateCPU(int channelIndex, float cpuQuota, int priority = 1);
    bool deallocateCPU(int channelIndex);
    bool updateCPUQuota(int channelIndex, float newQuota);
    
    // Core assignment
    std::vector<int> getAssignedCores(int channelIndex) const;
    bool assignSpecificCores(int channelIndex, const std::vector<int>& cores);
    void optimizeCoreAssignment();
    
    // Statistics
    float getTotalCPUUsage() const;
    float getChannelCPUUsage(int channelIndex) const;
    std::vector<int> getAvailableCores() const;

private:
    void assignCores(CPUAllocation* allocation);
    void releaseCores(CPUAllocation* allocation);
    std::vector<int> findOptimalCores(float cpuQuota);
};

/**
 * Resource Isolation Manager for ensuring proper resource isolation
 */
class ResourceIsolationManager {
public:
    enum IsolationLevel {
        NONE = 0,
        BASIC = 1,
        STRICT = 2,
        COMPLETE = 3
    };

    struct IsolationPolicy {
        IsolationLevel level;
        std::vector<ResourceManager::ResourceType> isolatedResources;
        bool allowResourceSharing;
        int maxSharedChannels;
        
        IsolationPolicy(IsolationLevel lvl = BASIC) 
            : level(lvl), allowResourceSharing(true), maxSharedChannels(4) {}
    };

private:
    std::map<int, IsolationPolicy> channelPolicies;
    std::mutex policyMutex;
    IsolationLevel defaultIsolationLevel;

public:
    ResourceIsolationManager(IsolationLevel defaultLevel = BASIC);
    
    // Policy management
    void setChannelIsolationPolicy(int channelIndex, const IsolationPolicy& policy);
    void setDefaultIsolationLevel(IsolationLevel level);
    IsolationPolicy getChannelIsolationPolicy(int channelIndex) const;
    
    // Isolation enforcement
    bool canShareResource(int channelIndex1, int channelIndex2, ResourceManager::ResourceType type) const;
    bool enforceIsolation(int channelIndex, ResourceManager::ResourceType type, long amount);
    void validateResourceAccess(int channelIndex, ResourceManager::ResourceType type);
    
    // Isolation monitoring
    std::vector<std::string> detectIsolationViolations() const;
    void reportIsolationStatus() const;

private:
    bool isResourceIsolated(int channelIndex, ResourceManager::ResourceType type) const;
    IsolationLevel getEffectiveIsolationLevel(int channelIndex) const;
};

#endif // AIBOX_RESOURCE_MANAGER_H
