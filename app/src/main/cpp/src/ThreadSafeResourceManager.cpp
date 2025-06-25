#include "ThreadSafeResourceManager.h"
#include "logging.h"
#include <algorithm>

ThreadSafeResourceManager::ThreadSafeResourceManager() {
    LOGD("ThreadSafeResourceManager created");
}

ThreadSafeResourceManager::~ThreadSafeResourceManager() {
    stopCleanupThread();
    
    // Clean up all resources
    std::lock_guard<std::mutex> lock(resourcesMutex);
    for (auto& pair : resources) {
        cleanupResourceInternal(pair.second.get());
    }
    resources.clear();

    // Clean up memory pools
    std::lock_guard<std::mutex> poolLock(poolsMutex);
    memoryPools.clear();
    
    LOGD("ThreadSafeResourceManager destroyed");
}

int ThreadSafeResourceManager::allocateResource(ResourceType type, size_t size, int channelIndex) {
    // Check memory limits
    if (totalMemoryUsage.load() + size > maxMemoryUsage.load()) {
        LOGW("Memory allocation would exceed limit: %zu + %zu > %zu", 
             totalMemoryUsage.load(), size, maxMemoryUsage.load());
        return -1;
    }
    
    // Check per-channel resource limits
    if (channelIndex >= 0) {
        auto channelResources = getResourcesByChannel(channelIndex);
        if (channelResources.size() >= maxResourcesPerChannel.load()) {
            LOGW("Channel %d resource allocation would exceed limit: %zu >= %d", 
                 channelIndex, channelResources.size(), maxResourcesPerChannel.load());
            return -1;
        }
    }
    
    int resourceId = generateResourceId();
    auto resource = std::make_unique<ResourceInfo>();
    
    resource->resourceId = resourceId;
    resource->type = type;
    resource->state = AVAILABLE;
    resource->resourceSize = size;
    resource->ownerChannelIndex = channelIndex;
    resource->createdTime = std::chrono::steady_clock::now();
    resource->lastUsedTime = resource->createdTime;
    
    // Allocate actual resource based on type
    switch (type) {
        case MEMORY_BUFFER:
            resource->resourcePtr = malloc(size);
            break;
        case GPU_MEMORY:
            // GPU memory allocation would be handled by GPU manager
            resource->resourcePtr = nullptr;
            break;
        default:
            resource->resourcePtr = nullptr;
            break;
    }
    
    if (type == MEMORY_BUFFER && !resource->resourcePtr) {
        LOGE("Failed to allocate memory buffer of size %zu", size);
        return -1;
    }
    
    // Set cleanup function
    resource->cleanupFunction = [this, type, ptr = resource->resourcePtr]() {
        if (type == MEMORY_BUFFER && ptr) {
            free(ptr);
        }
    };
    
    {
        std::lock_guard<std::mutex> lock(resourcesMutex);
        resources[resourceId] = std::move(resource);
    }
    
    totalMemoryUsage.fetch_add(size);
    activeResources.fetch_add(1);
    
    LOGD("Allocated resource %d (type: %d, size: %zu, channel: %d)", 
         resourceId, type, size, channelIndex);
    return resourceId;
}

bool ThreadSafeResourceManager::deallocateResource(int resourceId) {
    std::lock_guard<std::mutex> lock(resourcesMutex);
    
    auto it = resources.find(resourceId);
    if (it == resources.end()) {
        LOGW("Resource %d not found for deallocation", resourceId);
        return false;
    }
    
    auto& resource = it->second;
    
    // Check if resource is still in use
    if (resource->referenceCount.load() > 0) {
        LOGW("Cannot deallocate resource %d: still has %d references", 
             resourceId, resource->referenceCount.load());
        resource->state = CLEANUP_PENDING;
        return false;
    }
    
    cleanupResourceInternal(resource.get());
    totalMemoryUsage.fetch_sub(resource->resourceSize);
    activeResources.fetch_sub(1);
    
    resources.erase(it);
    
    LOGD("Deallocated resource %d", resourceId);
    return true;
}

bool ThreadSafeResourceManager::reserveResource(int resourceId, int channelIndex) {
    std::lock_guard<std::mutex> lock(resourcesMutex);
    
    auto it = resources.find(resourceId);
    if (it == resources.end()) {
        return false;
    }
    
    auto& resource = it->second;
    
    if (resource->state != AVAILABLE) {
        return false;
    }
    
    if (!validateResourceAccess(resourceId, channelIndex)) {
        return false;
    }
    
    resource->state = RESERVED;
    resource->ownerChannelIndex = channelIndex;
    resource->referenceCount.fetch_add(1);
    updateResourceUsage(resource.get());
    
    LOGD("Reserved resource %d for channel %d", resourceId, channelIndex);
    return true;
}

bool ThreadSafeResourceManager::releaseResource(int resourceId) {
    std::lock_guard<std::mutex> lock(resourcesMutex);
    
    auto it = resources.find(resourceId);
    if (it == resources.end()) {
        return false;
    }
    
    auto& resource = it->second;
    
    int refCount = resource->referenceCount.fetch_sub(1);
    if (refCount <= 1) {
        resource->state = AVAILABLE;
        resource->ownerChannelIndex = -1;
    }
    
    updateResourceUsage(resource.get());
    
    LOGD("Released resource %d (ref count: %d)", resourceId, refCount - 1);
    return true;
}

ThreadSafeResourceManager::ResourceInfo* ThreadSafeResourceManager::getResource(int resourceId) {
    std::lock_guard<std::mutex> lock(resourcesMutex);
    
    auto it = resources.find(resourceId);
    return (it != resources.end()) ? it->second.get() : nullptr;
}

std::vector<int> ThreadSafeResourceManager::getResourcesByChannel(int channelIndex) {
    std::lock_guard<std::mutex> lock(resourcesMutex);
    std::vector<int> channelResources;
    
    for (const auto& pair : resources) {
        if (pair.second->ownerChannelIndex == channelIndex) {
            channelResources.push_back(pair.first);
        }
    }
    
    return channelResources;
}

std::vector<int> ThreadSafeResourceManager::getResourcesByType(ResourceType type) const {
    std::lock_guard<std::mutex> lock(resourcesMutex);
    std::vector<int> typeResources;
    
    for (const auto& pair : resources) {
        if (pair.second->type == type) {
            typeResources.push_back(pair.first);
        }
    }
    
    return typeResources;
}

bool ThreadSafeResourceManager::createMemoryPool(ResourceType type, size_t blockSize, size_t maxBlocks) {
    std::lock_guard<std::mutex> lock(poolsMutex);
    
    if (memoryPools.find(type) != memoryPools.end()) {
        LOGW("Memory pool for type %d already exists", type);
        return false;
    }
    
    auto pool = std::make_unique<MemoryPool>(type, blockSize, maxBlocks);
    memoryPools[type] = std::move(pool);
    
    LOGD("Created memory pool for type %d (block size: %zu, max blocks: %zu)", 
         type, blockSize, maxBlocks);
    return true;
}

bool ThreadSafeResourceManager::destroyMemoryPool(ResourceType type) {
    std::lock_guard<std::mutex> lock(poolsMutex);
    
    auto it = memoryPools.find(type);
    if (it == memoryPools.end()) {
        return false;
    }
    
    // Clean up all blocks in the pool
    auto& pool = it->second;
    {
        std::lock_guard<std::mutex> poolLock(pool->poolMutex);
        
        for (auto& block : pool->availableBlocks) {
            if (block && block->cleanupFunction) {
                block->cleanupFunction();
            }
        }
        
        for (auto& block : pool->usedBlocks) {
            if (block && block->cleanupFunction) {
                block->cleanupFunction();
            }
        }
        
        pool->availableBlocks.clear();
        pool->usedBlocks.clear();
    }
    
    memoryPools.erase(it);
    
    LOGD("Destroyed memory pool for type %d", type);
    return true;
}

void* ThreadSafeResourceManager::allocateFromPool(ResourceType type, int channelIndex) {
    std::lock_guard<std::mutex> lock(poolsMutex);
    
    auto it = memoryPools.find(type);
    if (it == memoryPools.end()) {
        return nullptr;
    }
    
    auto& pool = it->second;
    std::lock_guard<std::mutex> poolLock(pool->poolMutex);
    
    // Try to reuse available block
    if (!pool->availableBlocks.empty()) {
        auto block = std::move(pool->availableBlocks.back());
        pool->availableBlocks.pop_back();
        
        block->state = IN_USE;
        block->ownerChannelIndex = channelIndex;
        block->lastUsedTime = std::chrono::steady_clock::now();
        
        void* ptr = block->resourcePtr;
        pool->usedBlocks.push_back(std::move(block));
        pool->totalUsed.fetch_add(1);
        
        return ptr;
    }
    
    // Create new block if under limit
    if (pool->totalAllocated.load() < pool->maxBlocks) {
        int resourceId = allocateResource(type, pool->blockSize, channelIndex);
        if (resourceId > 0) {
            auto resource = getResource(resourceId);
            if (resource) {
                pool->totalAllocated.fetch_add(1);
                pool->totalUsed.fetch_add(1);
                return resource->resourcePtr;
            }
        }
    }
    
    return nullptr; // Pool exhausted
}

bool ThreadSafeResourceManager::returnToPool(ResourceType type, void* ptr) {
    if (!ptr) return false;

    std::lock_guard<std::mutex> lock(poolsMutex);
    
    auto it = memoryPools.find(type);
    if (it == memoryPools.end()) {
        return false;
    }
    
    auto& pool = it->second;
    std::lock_guard<std::mutex> poolLock(pool->poolMutex);
    
    // Find the block in used blocks
    for (auto it = pool->usedBlocks.begin(); it != pool->usedBlocks.end(); ++it) {
        if ((*it)->resourcePtr == ptr) {
            auto block = std::move(*it);
            pool->usedBlocks.erase(it);
            
            block->state = AVAILABLE;
            block->ownerChannelIndex = -1;
            block->lastUsedTime = std::chrono::steady_clock::now();
            
            pool->availableBlocks.push_back(std::move(block));
            pool->totalUsed.fetch_sub(1);
            
            return true;
        }
    }
    
    return false;
}

void ThreadSafeResourceManager::startCleanupThread() {
    if (cleanupThreadRunning.load()) {
        return;
    }
    
    cleanupThreadRunning.store(true);
    cleanupThread = std::thread(&ThreadSafeResourceManager::cleanupLoop, this);
    LOGD("Cleanup thread started");
}

void ThreadSafeResourceManager::stopCleanupThread() {
    if (!cleanupThreadRunning.load()) {
        return;
    }
    
    cleanupThreadRunning.store(false);
    cleanupCv.notify_all();
    
    if (cleanupThread.joinable()) {
        cleanupThread.join();
    }
    
    LOGD("Cleanup thread stopped");
}

void ThreadSafeResourceManager::performCleanup() {
    cleanupExpiredResources();
    enforceMemoryLimits();
    enforceResourceLimits();
}

void ThreadSafeResourceManager::cleanupExpiredResources() {
    std::vector<int> expiredResources;
    
    {
        std::lock_guard<std::mutex> lock(resourcesMutex);
        for (const auto& pair : resources) {
            if (isResourceExpired(pair.second.get())) {
                expiredResources.push_back(pair.first);
            }
        }
    }
    
    for (int resourceId : expiredResources) {
        deallocateResource(resourceId);
    }
    
    if (!expiredResources.empty()) {
        LOGD("Cleaned up %zu expired resources", expiredResources.size());
    }
}

void ThreadSafeResourceManager::cleanupChannelResources(int channelIndex) {
    auto channelResources = getResourcesByChannel(channelIndex);
    
    for (int resourceId : channelResources) {
        deallocateResource(resourceId);
    }
    
    LOGD("Cleaned up %zu resources for channel %d", channelResources.size(), channelIndex);
}

int ThreadSafeResourceManager::generateResourceId() {
    return nextResourceId.fetch_add(1);
}

bool ThreadSafeResourceManager::validateResourceAccess(int resourceId, int channelIndex) {
    // Add access validation logic here
    // For now, allow all access
    return true;
}

void ThreadSafeResourceManager::updateResourceUsage(ResourceInfo* resource) {
    if (resource) {
        resource->lastUsedTime = std::chrono::steady_clock::now();
    }
}

void ThreadSafeResourceManager::cleanupResourceInternal(ResourceInfo* resource) {
    if (resource && resource->cleanupFunction) {
        resource->cleanupFunction();
        resource->resourcePtr = nullptr;
    }
}

void ThreadSafeResourceManager::cleanupLoop() {
    while (cleanupThreadRunning.load()) {
        std::unique_lock<std::mutex> lock(cleanupMutex);
        cleanupCv.wait_for(lock, std::chrono::milliseconds(cleanupIntervalMs.load()),
                          [this] { return !cleanupThreadRunning.load(); });
        
        if (!cleanupThreadRunning.load()) break;
        
        performCleanup();
    }
}

bool ThreadSafeResourceManager::isResourceExpired(const ResourceInfo* resource) const {
    if (!resource || resource->referenceCount.load() > 0) {
        return false;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastUse = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - resource->lastUsedTime);
    
    return timeSinceLastUse.count() > resourceTimeoutMs.load();
}

void ThreadSafeResourceManager::enforceMemoryLimits() {
    size_t currentUsage = totalMemoryUsage.load();
    size_t maxUsage = maxMemoryUsage.load();
    
    if (currentUsage > maxUsage) {
        LOGW("Memory usage exceeds limit: %zu > %zu, triggering cleanup", currentUsage, maxUsage);
        
        // Force cleanup of least recently used resources
        std::vector<std::pair<int, std::chrono::steady_clock::time_point>> resourcesByAge;
        
        {
            std::lock_guard<std::mutex> lock(resourcesMutex);
            for (const auto& pair : resources) {
                if (pair.second->referenceCount.load() == 0) {
                    resourcesByAge.push_back({pair.first, pair.second->lastUsedTime});
                }
            }
        }
        
        // Sort by age (oldest first)
        std::sort(resourcesByAge.begin(), resourcesByAge.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
        
        // Clean up oldest resources until under limit
        for (const auto& pair : resourcesByAge) {
            if (totalMemoryUsage.load() <= maxUsage) break;
            deallocateResource(pair.first);
        }
    }
}

void ThreadSafeResourceManager::enforceResourceLimits() {
    // Check per-channel resource limits
    for (int channel = 0; channel < 16; channel++) {
        auto channelResources = getResourcesByChannel(channel);
        int maxPerChannel = maxResourcesPerChannel.load();
        
        if (channelResources.size() > maxPerChannel) {
            LOGW("Channel %d exceeds resource limit: %zu > %d", 
                 channel, channelResources.size(), maxPerChannel);
            
            // Clean up excess resources (oldest first)
            // Implementation would sort by age and clean up excess
        }
    }
}

// Configuration methods
void ThreadSafeResourceManager::setMaxMemoryUsage(size_t maxMemory) {
    maxMemoryUsage.store(maxMemory);
    LOGD("Max memory usage set to %zu bytes", maxMemory);
}

void ThreadSafeResourceManager::setMaxResourcesPerChannel(int maxResources) {
    maxResourcesPerChannel.store(maxResources);
    LOGD("Max resources per channel set to %d", maxResources);
}

void ThreadSafeResourceManager::setCleanupInterval(int intervalMs) {
    cleanupIntervalMs.store(intervalMs);
    LOGD("Cleanup interval set to %d ms", intervalMs);
}

void ThreadSafeResourceManager::setResourceTimeout(int timeoutMs) {
    resourceTimeoutMs.store(timeoutMs);
    LOGD("Resource timeout set to %d ms", timeoutMs);
}

// Statistics methods
size_t ThreadSafeResourceManager::getTotalMemoryUsage() const {
    return totalMemoryUsage.load();
}

int ThreadSafeResourceManager::getActiveResourceCount() const {
    return activeResources.load();
}

int ThreadSafeResourceManager::getResourceCount(ResourceType type) const {
    auto typeResources = getResourcesByType(type);
    return static_cast<int>(typeResources.size());
}

float ThreadSafeResourceManager::getMemoryUtilization() const {
    size_t current = totalMemoryUsage.load();
    size_t max = maxMemoryUsage.load();
    return (max > 0) ? static_cast<float>(current) / max : 0.0f;
}

std::vector<std::string> ThreadSafeResourceManager::getResourceReport() const {
    std::vector<std::string> report;
    
    report.push_back("Thread-Safe Resource Manager Report:");
    report.push_back("Total Memory Usage: " + std::to_string(totalMemoryUsage.load()) + " bytes");
    report.push_back("Memory Utilization: " + std::to_string(getMemoryUtilization() * 100.0f) + "%");
    report.push_back("Active Resources: " + std::to_string(activeResources.load()));
    
    // Add per-type statistics
    for (int type = 0; type <= NETWORK_CONNECTION; type++) {
        int count = getResourceCount(static_cast<ResourceType>(type));
        if (count > 0) {
            report.push_back("Type " + std::to_string(type) + " Resources: " + std::to_string(count));
        }
    }
    
    return report;
}

// ResourceLock implementation
ThreadSafeResourceManager::ResourceLock::ResourceLock(ThreadSafeResourceManager* mgr, int id)
    : manager(mgr), resourceId(id), locked(false) {
    if (manager) {
        auto resource = manager->getResource(resourceId);
        if (resource && resource->referenceCount.fetch_add(1) >= 0) {
            locked = true;
        }
    }
}

ThreadSafeResourceManager::ResourceLock::~ResourceLock() {
    if (locked && manager) {
        manager->releaseResource(resourceId);
    }
}

ThreadSafeResourceManager::ResourceInfo* ThreadSafeResourceManager::ResourceLock::get() {
    return (locked && manager) ? manager->getResource(resourceId) : nullptr;
}

bool ThreadSafeResourceManager::ResourceLock::isValid() const {
    return locked && manager && manager->getResource(resourceId) != nullptr;
}

ThreadSafeResourceManager::ResourceLock ThreadSafeResourceManager::lockResource(int resourceId) {
    return ResourceLock(this, resourceId);
}

// ChannelSynchronizer implementation
ChannelSynchronizer::ChannelSynchronizer() {
    LOGD("ChannelSynchronizer created");
}

ChannelSynchronizer::~ChannelSynchronizer() {
    std::lock_guard<std::mutex> lock(syncPointsMutex);
    syncPoints.clear();
    LOGD("ChannelSynchronizer destroyed");
}

bool ChannelSynchronizer::createSyncPoint(const std::string& name, SyncType type, int requiredChannels) {
    std::lock_guard<std::mutex> lock(syncPointsMutex);

    if (syncPoints.find(name) != syncPoints.end()) {
        LOGW("Sync point '%s' already exists", name.c_str());
        return false;
    }

    auto syncPoint = std::make_unique<SyncPoint>();
    syncPoint->type = type;
    syncPoint->requiredChannels.store(requiredChannels);

    syncPoints[name] = std::move(syncPoint);

    LOGD("Created sync point '%s' (type: %d, required channels: %d)",
         name.c_str(), type, requiredChannels);
    return true;
}

bool ChannelSynchronizer::destroySyncPoint(const std::string& name) {
    std::lock_guard<std::mutex> lock(syncPointsMutex);

    auto it = syncPoints.find(name);
    if (it == syncPoints.end()) {
        return false;
    }

    // Wake up any waiting threads
    auto& syncPoint = it->second;
    syncPoint->cv.notify_all();

    syncPoints.erase(it);

    LOGD("Destroyed sync point '%s'", name.c_str());
    return true;
}

bool ChannelSynchronizer::acquireSync(const std::string& name, int channelIndex, int timeoutMs) {
    std::lock_guard<std::mutex> lock(syncPointsMutex);

    auto it = syncPoints.find(name);
    if (it == syncPoints.end()) {
        return false;
    }

    auto& syncPoint = it->second;

    switch (syncPoint->type) {
        case EXCLUSIVE:
        case SHARED_WRITE:
            if (timeoutMs < 0) {
                syncPoint->mutex.lock();
            } else {
                return syncPoint->mutex.try_lock();
            }
            break;

        case SHARED_READ:
            // For simplicity, treat shared read as exclusive in this implementation
            if (timeoutMs < 0) {
                syncPoint->mutex.lock();
            } else {
                return syncPoint->mutex.try_lock();
            }
            break;

        case BARRIER:
            // Barrier synchronization handled in waitForBarrier
            return false;
    }

    LOGD("Channel %d acquired sync '%s'", channelIndex, name.c_str());
    return true;
}

bool ChannelSynchronizer::releaseSync(const std::string& name, int channelIndex) {
    std::lock_guard<std::mutex> lock(syncPointsMutex);

    auto it = syncPoints.find(name);
    if (it == syncPoints.end()) {
        return false;
    }

    auto& syncPoint = it->second;

    switch (syncPoint->type) {
        case EXCLUSIVE:
        case SHARED_WRITE:
        case SHARED_READ:
            syncPoint->mutex.unlock();
            break;

        case BARRIER:
            // Barrier synchronization doesn't need explicit release
            break;
    }

    LOGD("Channel %d released sync '%s'", channelIndex, name.c_str());
    return true;
}

bool ChannelSynchronizer::waitForBarrier(const std::string& name, int channelIndex, int timeoutMs) {
    std::lock_guard<std::mutex> lock(syncPointsMutex);

    auto it = syncPoints.find(name);
    if (it == syncPoints.end()) {
        return false;
    }

    auto& syncPoint = it->second;
    if (syncPoint->type != BARRIER) {
        return false;
    }

    int waiting = syncPoint->waitingChannels.fetch_add(1) + 1;
    int required = syncPoint->requiredChannels.load();

    if (waiting >= required) {
        // Last channel to arrive - signal barrier
        syncPoint->barrierReached.store(true);
        syncPoint->cv.notify_all();
        LOGD("Channel %d triggered barrier '%s' (%d/%d)", channelIndex, name.c_str(), waiting, required);
        return true;
    } else {
        // Wait for barrier
        std::unique_lock<std::mutex> barrierLock(syncPoint->mutex);

        if (timeoutMs < 0) {
            syncPoint->cv.wait(barrierLock, [&syncPoint] {
                return syncPoint->barrierReached.load();
            });
        } else {
            bool result = syncPoint->cv.wait_for(barrierLock, std::chrono::milliseconds(timeoutMs),
                                                [&syncPoint] {
                                                    return syncPoint->barrierReached.load();
                                                });
            if (!result) {
                syncPoint->waitingChannels.fetch_sub(1);
                return false;
            }
        }

        LOGD("Channel %d passed barrier '%s'", channelIndex, name.c_str());
        return true;
    }
}

bool ChannelSynchronizer::signalBarrier(const std::string& name) {
    std::lock_guard<std::mutex> lock(syncPointsMutex);

    auto it = syncPoints.find(name);
    if (it == syncPoints.end()) {
        return false;
    }

    auto& syncPoint = it->second;
    if (syncPoint->type != BARRIER) {
        return false;
    }

    syncPoint->barrierReached.store(true);
    syncPoint->cv.notify_all();

    LOGD("Barrier '%s' signaled", name.c_str());
    return true;
}

std::vector<std::string> ChannelSynchronizer::getActiveSyncPoints() const {
    std::lock_guard<std::mutex> lock(syncPointsMutex);
    std::vector<std::string> activePoints;

    for (const auto& pair : syncPoints) {
        activePoints.push_back(pair.first);
    }

    return activePoints;
}

int ChannelSynchronizer::getWaitingChannels(const std::string& name) const {
    std::lock_guard<std::mutex> lock(syncPointsMutex);

    auto it = syncPoints.find(name);
    return (it != syncPoints.end()) ? it->second->waitingChannels.load() : 0;
}

bool ChannelSynchronizer::isSyncPointActive(const std::string& name) const {
    std::lock_guard<std::mutex> lock(syncPointsMutex);
    return syncPoints.find(name) != syncPoints.end();
}
