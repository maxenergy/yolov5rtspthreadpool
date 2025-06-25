#ifndef THREAD_SAFE_RESOURCE_MANAGER_H
#define THREAD_SAFE_RESOURCE_MANAGER_H

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <functional>

/**
 * Thread-Safe Resource Manager for Multi-Channel Processing
 * Provides comprehensive resource management with thread safety guarantees
 */
class ThreadSafeResourceManager {
public:
    enum ResourceType {
        MEMORY_BUFFER = 0,
        GPU_MEMORY = 1,
        DECODER_INSTANCE = 2,
        RENDER_SURFACE = 3,
        THREAD_POOL = 4,
        FILE_HANDLE = 5,
        NETWORK_CONNECTION = 6
    };

    enum ResourceState {
        AVAILABLE = 0,
        IN_USE = 1,
        RESERVED = 2,
        ERROR = 3,
        CLEANUP_PENDING = 4
    };

    struct ResourceInfo {
        int resourceId;
        ResourceType type;
        ResourceState state;
        void* resourcePtr;
        size_t resourceSize;
        int ownerChannelIndex;
        std::chrono::steady_clock::time_point createdTime;
        std::chrono::steady_clock::time_point lastUsedTime;
        std::atomic<int> referenceCount{0};
        std::function<void()> cleanupFunction;
        
        ResourceInfo() : resourceId(-1), type(MEMORY_BUFFER), state(AVAILABLE),
                        resourcePtr(nullptr), resourceSize(0), ownerChannelIndex(-1),
                        createdTime(std::chrono::steady_clock::now()),
                        lastUsedTime(std::chrono::steady_clock::now()) {}
    };

    struct MemoryPool {
        ResourceType poolType;
        size_t blockSize;
        size_t maxBlocks;
        std::vector<std::unique_ptr<ResourceInfo>> availableBlocks;
        std::vector<std::unique_ptr<ResourceInfo>> usedBlocks;
        std::atomic<size_t> totalAllocated{0};
        std::atomic<size_t> totalUsed{0};
        mutable std::mutex poolMutex;
        
        MemoryPool(ResourceType type, size_t size, size_t max) 
            : poolType(type), blockSize(size), maxBlocks(max) {}
    };

private:
    std::unordered_map<int, std::unique_ptr<ResourceInfo>> resources;
    std::unordered_map<ResourceType, std::unique_ptr<MemoryPool>> memoryPools;
    mutable std::mutex resourcesMutex;
    mutable std::mutex poolsMutex;
    
    // Resource tracking
    std::atomic<int> nextResourceId{1};
    std::atomic<size_t> totalMemoryUsage{0};
    std::atomic<int> activeResources{0};
    
    // Cleanup management
    std::atomic<bool> cleanupThreadRunning{false};
    std::thread cleanupThread;
    std::condition_variable cleanupCv;
    std::mutex cleanupMutex;
    
    // Configuration
    std::atomic<size_t> maxMemoryUsage{512 * 1024 * 1024}; // 512MB default
    std::atomic<int> maxResourcesPerChannel{100};
    std::atomic<int> cleanupIntervalMs{5000}; // 5 seconds
    std::atomic<int> resourceTimeoutMs{30000}; // 30 seconds

public:
    ThreadSafeResourceManager();
    ~ThreadSafeResourceManager();

    // Resource allocation and deallocation
    int allocateResource(ResourceType type, size_t size, int channelIndex = -1);
    bool deallocateResource(int resourceId);
    bool reserveResource(int resourceId, int channelIndex);
    bool releaseResource(int resourceId);
    
    // Resource access
    ResourceInfo* getResource(int resourceId);
    std::vector<int> getResourcesByChannel(int channelIndex);
    std::vector<int> getResourcesByType(ResourceType type) const;
    
    // Memory pool management
    bool createMemoryPool(ResourceType type, size_t blockSize, size_t maxBlocks);
    bool destroyMemoryPool(ResourceType type);
    void* allocateFromPool(ResourceType type, int channelIndex = -1);
    bool returnToPool(ResourceType type, void* ptr);
    
    // Thread safety utilities
    class ResourceLock {
    private:
        ThreadSafeResourceManager* manager;
        int resourceId;
        bool locked;
        
    public:
        ResourceLock(ThreadSafeResourceManager* mgr, int id);
        ~ResourceLock();
        ResourceInfo* get();
        bool isValid() const;
    };
    
    ResourceLock lockResource(int resourceId);
    
    // Cleanup and maintenance
    void startCleanupThread();
    void stopCleanupThread();
    void performCleanup();
    void cleanupExpiredResources();
    void cleanupChannelResources(int channelIndex);
    
    // Configuration
    void setMaxMemoryUsage(size_t maxMemory);
    void setMaxResourcesPerChannel(int maxResources);
    void setCleanupInterval(int intervalMs);
    void setResourceTimeout(int timeoutMs);
    
    // Statistics and monitoring
    size_t getTotalMemoryUsage() const;
    int getActiveResourceCount() const;
    int getResourceCount(ResourceType type) const;
    float getMemoryUtilization() const;
    std::vector<std::string> getResourceReport() const;

private:
    // Internal helper methods
    int generateResourceId();
    bool validateResourceAccess(int resourceId, int channelIndex);
    void updateResourceUsage(ResourceInfo* resource);
    void cleanupResourceInternal(ResourceInfo* resource);
    void cleanupLoop();
    bool isResourceExpired(const ResourceInfo* resource) const;
    void enforceMemoryLimits();
    void enforceResourceLimits();
};

/**
 * RAII Resource Guard for automatic resource management
 */
template<typename T>
class ResourceGuard {
private:
    T* resource;
    std::function<void(T*)> deleter;
    bool released;

public:
    ResourceGuard(T* res, std::function<void(T*)> del) 
        : resource(res), deleter(del), released(false) {}
    
    ~ResourceGuard() {
        if (!released && resource && deleter) {
            deleter(resource);
        }
    }
    
    T* get() { return resource; }
    T* operator->() { return resource; }
    T& operator*() { return *resource; }
    
    T* release() {
        released = true;
        return resource;
    }
    
    void reset(T* newResource = nullptr) {
        if (!released && resource && deleter) {
            deleter(resource);
        }
        resource = newResource;
        released = false;
    }
    
    // Non-copyable
    ResourceGuard(const ResourceGuard&) = delete;
    ResourceGuard& operator=(const ResourceGuard&) = delete;
    
    // Movable
    ResourceGuard(ResourceGuard&& other) noexcept 
        : resource(other.resource), deleter(std::move(other.deleter)), released(other.released) {
        other.resource = nullptr;
        other.released = true;
    }
    
    ResourceGuard& operator=(ResourceGuard&& other) noexcept {
        if (this != &other) {
            reset();
            resource = other.resource;
            deleter = std::move(other.deleter);
            released = other.released;
            other.resource = nullptr;
            other.released = true;
        }
        return *this;
    }
};

/**
 * Thread-Safe Channel Synchronizer
 * Provides synchronization primitives for multi-channel operations
 */
class ChannelSynchronizer {
public:
    enum SyncType {
        EXCLUSIVE = 0,      // Only one channel can access
        SHARED_READ = 1,    // Multiple channels can read
        SHARED_WRITE = 2,   // Multiple channels can write
        BARRIER = 3         // All channels must reach barrier
    };

private:
    struct SyncPoint {
        SyncType type;
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<int> waitingChannels{0};
        std::atomic<int> requiredChannels{0};
        std::atomic<bool> barrierReached{false};
    };
    
    std::unordered_map<std::string, std::unique_ptr<SyncPoint>> syncPoints;
    mutable std::mutex syncPointsMutex;

public:
    ChannelSynchronizer();
    ~ChannelSynchronizer();

    // Synchronization operations
    bool createSyncPoint(const std::string& name, SyncType type, int requiredChannels = 1);
    bool destroySyncPoint(const std::string& name);
    
    bool acquireSync(const std::string& name, int channelIndex, int timeoutMs = -1);
    bool releaseSync(const std::string& name, int channelIndex);
    
    bool waitForBarrier(const std::string& name, int channelIndex, int timeoutMs = -1);
    bool signalBarrier(const std::string& name);
    
    // Utility methods
    std::vector<std::string> getActiveSyncPoints() const;
    int getWaitingChannels(const std::string& name) const;
    bool isSyncPointActive(const std::string& name) const;
};

/**
 * Memory Pool with Thread Safety
 * Optimized memory allocation for multi-channel processing
 */
template<typename T>
class ThreadSafeMemoryPool {
private:
    struct PoolBlock {
        T* data;
        bool inUse;
        std::chrono::steady_clock::time_point lastUsed;
        
        PoolBlock() : data(nullptr), inUse(false), lastUsed(std::chrono::steady_clock::now()) {}
    };
    
    std::vector<std::unique_ptr<PoolBlock>> blocks;
    mutable std::mutex poolMutex;
    size_t blockSize;
    size_t maxBlocks;
    std::atomic<size_t> allocatedBlocks{0};
    std::atomic<size_t> usedBlocks{0};

public:
    ThreadSafeMemoryPool(size_t blockSz, size_t maxBlks) 
        : blockSize(blockSz), maxBlocks(maxBlks) {
        blocks.reserve(maxBlocks);
    }
    
    ~ThreadSafeMemoryPool() {
        std::lock_guard<std::mutex> lock(poolMutex);
        for (auto& block : blocks) {
            if (block && block->data) {
                delete[] block->data;
            }
        }
    }
    
    T* allocate() {
        std::lock_guard<std::mutex> lock(poolMutex);
        
        // Find available block
        for (auto& block : blocks) {
            if (block && !block->inUse) {
                block->inUse = true;
                block->lastUsed = std::chrono::steady_clock::now();
                usedBlocks.fetch_add(1);
                return block->data;
            }
        }
        
        // Create new block if under limit
        if (allocatedBlocks.load() < maxBlocks) {
            auto newBlock = std::make_unique<PoolBlock>();
            newBlock->data = new T[blockSize];
            newBlock->inUse = true;
            newBlock->lastUsed = std::chrono::steady_clock::now();
            
            T* result = newBlock->data;
            blocks.push_back(std::move(newBlock));
            allocatedBlocks.fetch_add(1);
            usedBlocks.fetch_add(1);
            
            return result;
        }
        
        return nullptr; // Pool exhausted
    }
    
    void deallocate(T* ptr) {
        if (!ptr) return;
        
        std::lock_guard<std::mutex> lock(poolMutex);
        
        for (auto& block : blocks) {
            if (block && block->data == ptr && block->inUse) {
                block->inUse = false;
                usedBlocks.fetch_sub(1);
                break;
            }
        }
    }
    
    size_t getUsedCount() const { return usedBlocks.load(); }
    size_t getAllocatedCount() const { return allocatedBlocks.load(); }
    float getUtilization() const {
        size_t allocated = allocatedBlocks.load();
        return (allocated > 0) ? static_cast<float>(usedBlocks.load()) / allocated : 0.0f;
    }
};

#endif // THREAD_SAFE_RESOURCE_MANAGER_H
