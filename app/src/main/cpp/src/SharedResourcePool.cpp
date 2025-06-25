#include "SharedResourcePool.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

SharedResourcePool::SharedResourcePool()
    : sharedModelData(nullptr), sharedModelSize(0), eventListener(nullptr), threadsRunning(false) {
    LOGD("SharedResourcePool created");
}

SharedResourcePool::~SharedResourcePool() {
    cleanup();
    LOGD("SharedResourcePool destroyed");
}

bool SharedResourcePool::initialize(char* modelData, int modelSize) {
    if (!modelData || modelSize <= 0) {
        LOGE("Invalid model data provided");
        return false;
    }
    
    // Copy model data
    {
        std::lock_guard<std::mutex> lock(modelDataMutex);
        sharedModelData = new char[modelSize];
        memcpy(sharedModelData, modelData, modelSize);
        sharedModelSize = modelSize;
    }
    
    // Create default pools
    if (!createPool(YOLOV5_THREAD_POOL)) {
        LOGE("Failed to create YOLOv5 thread pool");
        return false;
    }
    
    if (!createPool(MPP_DECODER_POOL)) {
        LOGE("Failed to create MPP decoder pool");
        return false;
    }
    
    if (!createPool(MEMORY_BUFFER_POOL)) {
        LOGE("Failed to create memory buffer pool");
        return false;
    }
    
    if (!createPool(FRAME_BUFFER_POOL)) {
        LOGE("Failed to create frame buffer pool");
        return false;
    }
    
    // Start management threads
    threadsRunning = true;
    poolManagerThread = std::thread(&SharedResourcePool::poolManagerLoop, this);
    statisticsThread = std::thread(&SharedResourcePool::statisticsLoop, this);
    
    LOGD("SharedResourcePool initialized successfully");
    return true;
}

void SharedResourcePool::cleanup() {
    // Stop threads
    threadsRunning = false;
    poolManagerCv.notify_all();
    statisticsCv.notify_all();
    
    if (poolManagerThread.joinable()) {
        poolManagerThread.join();
    }
    
    if (statisticsThread.joinable()) {
        statisticsThread.join();
    }
    
    // Clear pools
    {
        std::lock_guard<std::mutex> lock(poolsMutex);
        resourcePools.clear();
        poolConfigs.clear();
        poolStats.clear();
    }
    
    // Clear affinity data
    {
        std::lock_guard<std::mutex> lock(affinityMutex);
        channelAffinity.clear();
    }
    
    // Clean up model data
    {
        std::lock_guard<std::mutex> lock(modelDataMutex);
        if (sharedModelData) {
            delete[] sharedModelData;
            sharedModelData = nullptr;
            sharedModelSize = 0;
        }
    }
    
    LOGD("SharedResourcePool cleanup completed");
}

bool SharedResourcePool::createPool(PoolType type, const PoolConfiguration& config) {
    std::lock_guard<std::mutex> lock(poolsMutex);
    
    if (resourcePools.find(type) != resourcePools.end()) {
        LOGW("Pool for type %s already exists", poolTypeToString(type).c_str());
        return false;
    }
    
    // Create pool vector
    resourcePools[type] = std::vector<std::unique_ptr<ResourceInstance>>();
    poolConfigs[type] = config;
    poolStats[type] = PoolStatistics(type);
    
    // Create initial instances
    auto& pool = resourcePools[type];
    for (int i = 0; i < config.initialSize; i++) {
        auto resource = createResourceInstance(type);
        if (resource) {
            auto instance = std::make_unique<ResourceInstance>(i, type, resource);
            pool.push_back(std::move(instance));
            poolStats[type].totalInstances++;
            poolStats[type].idleInstances++;
        }
    }
    
    LOGD("Created pool for %s with %d instances", 
         poolTypeToString(type).c_str(), config.initialSize);
    return true;
}

std::shared_ptr<void> SharedResourcePool::allocateResource(PoolType type, int channelIndex, int priority) {
    auto startTime = std::chrono::steady_clock::now();
    
    auto instance = findAvailableInstance(type, channelIndex);
    if (!instance) {
        // Try to expand pool if allowed
        auto& config = poolConfigs[type];
        if (config.enableDynamicResize) {
            std::lock_guard<std::mutex> lock(poolsMutex);
            auto& pool = resourcePools[type];
            if (pool.size() < config.maxSize) {
                auto resource = createResourceInstance(type);
                if (resource) {
                    int newId = pool.size();
                    auto newInstance = std::make_unique<ResourceInstance>(newId, type, resource);
                    instance = newInstance.get();
                    pool.push_back(std::move(newInstance));
                    
                    poolStats[type].totalInstances++;
                    poolStats[type].dynamicExpansions++;
                    
                    notifyPoolExpanded(type, pool.size());
                    LOGD("Expanded %s pool to %zu instances", poolTypeToString(type).c_str(), pool.size());
                }
            }
        }
    }
    
    if (instance) {
        std::lock_guard<std::mutex> instanceLock(instance->instanceMutex);
        instance->inUse = true;
        instance->assignedChannel = channelIndex;
        instance->usageCount++;
        instance->lastUsed = std::chrono::steady_clock::now();
        
        poolStats[type].activeInstances++;
        poolStats[type].idleInstances--;
        poolStats[type].totalRequests++;
        poolStats[type].successfulAllocations++;
        poolStats[type].channelUsage[channelIndex]++;
        
        // Record performance
        auto endTime = std::chrono::steady_clock::now();
        float responseTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        recordAllocationTime(type, responseTime);
        
        notifyResourceAllocated(type, instance->instanceId, channelIndex);
        
        LOGD("Allocated %s resource (instance %d) to channel %d", 
             poolTypeToString(type).c_str(), instance->instanceId, channelIndex);
        
        return instance->resource;
    } else {
        poolStats[type].totalRequests++;
        poolStats[type].failedAllocations++;
        
        notifyAllocationFailed(type, channelIndex);
        LOGW("Failed to allocate %s resource for channel %d", 
             poolTypeToString(type).c_str(), channelIndex);
        
        return nullptr;
    }
}

bool SharedResourcePool::releaseResource(PoolType type, std::shared_ptr<void> resource, int channelIndex) {
    if (!resource) {
        return false;
    }
    
    auto instance = findInstanceByResource(type, resource);
    if (!instance) {
        LOGW("Resource instance not found for release");
        return false;
    }
    
    std::lock_guard<std::mutex> instanceLock(instance->instanceMutex);
    
    if (instance->assignedChannel != channelIndex) {
        LOGW("Channel mismatch during resource release: expected %d, got %d", 
             instance->assignedChannel.load(), channelIndex);
    }
    
    instance->inUse = false;
    instance->assignedChannel = -1;
    instance->lastUsed = std::chrono::steady_clock::now();
    
    poolStats[type].activeInstances--;
    poolStats[type].idleInstances++;
    
    notifyResourceReleased(type, instance->instanceId, channelIndex);
    
    LOGD("Released %s resource (instance %d) from channel %d", 
         poolTypeToString(type).c_str(), instance->instanceId, channelIndex);
    
    return true;
}

std::shared_ptr<Yolov5ThreadPool> SharedResourcePool::allocateYolov5ThreadPool(int channelIndex, int priority) {
    auto resource = allocateResource(YOLOV5_THREAD_POOL, channelIndex, priority);
    return std::static_pointer_cast<Yolov5ThreadPool>(resource);
}

std::shared_ptr<MppDecoder> SharedResourcePool::allocateMppDecoder(int channelIndex, int priority) {
    auto resource = allocateResource(MPP_DECODER_POOL, channelIndex, priority);
    return std::static_pointer_cast<MppDecoder>(resource);
}

std::shared_ptr<void> SharedResourcePool::allocateMemoryBuffer(int channelIndex, size_t size) {
    // For memory buffers, we might want to create them on-demand with specific sizes
    auto resource = createMemoryBuffer(size);
    if (resource) {
        LOGD("Allocated memory buffer of size %zu for channel %d", size, channelIndex);
    }
    return resource;
}

std::shared_ptr<frame_data_t> SharedResourcePool::allocateFrameBuffer(int channelIndex) {
    auto resource = allocateResource(FRAME_BUFFER_POOL, channelIndex);
    return std::static_pointer_cast<frame_data_t>(resource);
}

SharedResourcePool::ResourceInstance* SharedResourcePool::findAvailableInstance(PoolType type, int channelIndex) {
    std::lock_guard<std::mutex> lock(poolsMutex);
    
    auto poolIt = resourcePools.find(type);
    if (poolIt == resourcePools.end()) {
        return nullptr;
    }
    
    auto& config = poolConfigs[type];
    return selectInstanceByStrategy(type, channelIndex, config.strategy);
}

SharedResourcePool::ResourceInstance* SharedResourcePool::selectInstanceByStrategy(PoolType type, int channelIndex, AllocationStrategy strategy) {
    switch (strategy) {
        case ROUND_ROBIN:
            return selectRoundRobin(type);
        case LEAST_LOADED:
            return selectLeastLoaded(type);
        case PRIORITY_BASED:
            return selectByPriority(type, channelIndex);
        case AFFINITY_BASED:
            return selectByAffinity(type, channelIndex);
        case ADAPTIVE:
            return selectAdaptive(type, channelIndex);
        default:
            return selectLeastLoaded(type);
    }
}

SharedResourcePool::ResourceInstance* SharedResourcePool::selectLeastLoaded(PoolType type) {
    auto& pool = resourcePools[type];
    ResourceInstance* bestInstance = nullptr;
    int minUsage = INT_MAX;
    
    for (auto& instance : pool) {
        if (!instance->inUse && instance->usageCount < minUsage) {
            minUsage = instance->usageCount;
            bestInstance = instance.get();
        }
    }
    
    return bestInstance;
}

SharedResourcePool::ResourceInstance* SharedResourcePool::selectByAffinity(PoolType type, int channelIndex) {
    // First try to find the preferred instance for this channel
    int preferredId = getChannelAffinity(channelIndex, type);
    if (preferredId >= 0) {
        auto instance = findInstanceById(type, preferredId);
        if (instance && !instance->inUse) {
            return instance;
        }
    }
    
    // Fall back to least loaded
    return selectLeastLoaded(type);
}

SharedResourcePool::ResourceInstance* SharedResourcePool::selectAdaptive(PoolType type, int channelIndex) {
    // Combine affinity and least loaded strategies
    auto affinityInstance = selectByAffinity(type, channelIndex);
    if (affinityInstance) {
        return affinityInstance;
    }
    
    return selectLeastLoaded(type);
}

std::shared_ptr<void> SharedResourcePool::createResourceInstance(PoolType type) {
    switch (type) {
        case YOLOV5_THREAD_POOL:
            return createYolov5ThreadPool();
        case MPP_DECODER_POOL:
            return createMppDecoder();
        case MEMORY_BUFFER_POOL:
            return createMemoryBuffer(1024 * 1024); // 1MB default
        case FRAME_BUFFER_POOL:
            return createFrameBuffer();
        default:
            LOGE("Unknown pool type: %d", type);
            return nullptr;
    }
}

std::shared_ptr<Yolov5ThreadPool> SharedResourcePool::createYolov5ThreadPool() {
    std::lock_guard<std::mutex> lock(modelDataMutex);
    
    if (!sharedModelData || sharedModelSize <= 0) {
        LOGE("Model data not available for YOLOv5 thread pool creation");
        return nullptr;
    }
    
    auto threadPool = std::make_shared<Yolov5ThreadPool>();
    if (threadPool->setUpWithModelData(4, sharedModelData, sharedModelSize) == NN_SUCCESS) {
        LOGD("Created YOLOv5 thread pool with 4 threads");
        return threadPool;
    } else {
        LOGE("Failed to initialize YOLOv5 thread pool");
        return nullptr;
    }
}

std::shared_ptr<MppDecoder> SharedResourcePool::createMppDecoder() {
    auto decoder = std::make_shared<MppDecoder>();
    if (decoder->Init(264, 25, nullptr) == 0) { // H264, 25fps, success returns 0
        LOGD("Created MPP decoder");
        return decoder;
    } else {
        LOGE("Failed to initialize MPP decoder");
        return nullptr;
    }
}

std::shared_ptr<void> SharedResourcePool::createMemoryBuffer(size_t size) {
    auto buffer = std::shared_ptr<void>(new uint8_t[size], [](void* p) { delete[] static_cast<uint8_t*>(p); });
    LOGD("Created memory buffer of size %zu", size);
    return buffer;
}

std::shared_ptr<frame_data_t> SharedResourcePool::createFrameBuffer() {
    auto frameBuffer = std::make_shared<frame_data_t>();
    frameBuffer->dataSize = 1920 * 1080 * 4; // Default RGBA buffer
    frameBuffer->data = std::make_unique<char[]>(frameBuffer->dataSize);
    LOGD("Created frame buffer");
    return frameBuffer;
}

void SharedResourcePool::poolManagerLoop() {
    while (threadsRunning) {
        std::unique_lock<std::mutex> lock(threadMutex);
        poolManagerCv.wait_for(lock, std::chrono::seconds(5), [this] { return !threadsRunning; });
        
        if (!threadsRunning) break;
        
        // Perform pool management tasks
        monitorPoolUtilization();
        performDynamicResize();
        reclaimIdleResources();
        
        if (poolConfigs.begin()->second.enableLoadBalancing) {
            balanceLoad();
        }
    }
}

void SharedResourcePool::statisticsLoop() {
    while (threadsRunning) {
        std::unique_lock<std::mutex> lock(threadMutex);
        statisticsCv.wait_for(lock, std::chrono::seconds(2), [this] { return !threadsRunning; });
        
        if (!threadsRunning) break;
        
        updatePoolStatistics();
        updatePerformanceMetrics();
    }
}

void SharedResourcePool::updatePoolStatistics() {
    std::lock_guard<std::mutex> lock(poolsMutex);
    
    for (auto& pair : poolStats) {
        auto& stats = pair.second;
        PoolType type = pair.first;
        
        // Calculate utilization
        if (stats.totalInstances > 0) {
            stats.utilizationRate = static_cast<float>(stats.activeInstances) / stats.totalInstances;
        }
        
        // Update average response time
        stats.averageResponseTime = getAverageResponseTime(type);
        
        LOGD("Pool %s statistics: %d total, %d active, %.2f%% utilization", 
             poolTypeToString(type).c_str(), stats.totalInstances, 
             stats.activeInstances, stats.utilizationRate * 100.0f);
    }
}

void SharedResourcePool::monitorPoolUtilization() {
    std::lock_guard<std::mutex> lock(poolsMutex);
    
    for (const auto& pair : poolStats) {
        PoolType type = pair.first;
        const auto& stats = pair.second;
        
        if (stats.utilizationRate > poolConfigs[type].utilizationThreshold) {
            LOGW("High utilization detected for %s pool: %.2f%%", 
                 poolTypeToString(type).c_str(), stats.utilizationRate * 100.0f);
            
            notifyUtilizationAlert(type, stats.utilizationRate);
            
            // Consider expanding pool
            if (poolConfigs[type].enableDynamicResize) {
                expandPool(type, 2);
            }
        }
    }
}

std::string SharedResourcePool::poolTypeToString(PoolType type) const {
    switch (type) {
        case YOLOV5_THREAD_POOL: return "YOLOv5ThreadPool";
        case MPP_DECODER_POOL: return "MppDecoderPool";
        case MEMORY_BUFFER_POOL: return "MemoryBufferPool";
        case FRAME_BUFFER_POOL: return "FrameBufferPool";
        case DETECTION_RESULT_POOL: return "DetectionResultPool";
        default: return "Unknown";
    }
}

SharedResourcePool::ResourceInstance* SharedResourcePool::findInstanceById(PoolType type, int instanceId) {
    auto poolIt = resourcePools.find(type);
    if (poolIt == resourcePools.end()) {
        return nullptr;
    }
    
    auto& pool = poolIt->second;
    for (auto& instance : pool) {
        if (instance->instanceId == instanceId) {
            return instance.get();
        }
    }
    
    return nullptr;
}

SharedResourcePool::ResourceInstance* SharedResourcePool::findInstanceByResource(PoolType type, std::shared_ptr<void> resource) {
    auto poolIt = resourcePools.find(type);
    if (poolIt == resourcePools.end()) {
        return nullptr;
    }
    
    auto& pool = poolIt->second;
    for (auto& instance : pool) {
        if (instance->resource == resource) {
            return instance.get();
        }
    }
    
    return nullptr;
}

void SharedResourcePool::recordAllocationTime(PoolType type, float responseTime) {
    std::lock_guard<std::mutex> lock(performanceMutex);
    
    auto& history = responseTimeHistory[type];
    history.push(responseTime);
    
    // Keep only recent history (last 100 allocations)
    while (history.size() > 100) {
        history.pop();
    }
    
    lastAllocationTime[type] = std::chrono::steady_clock::now();
}

float SharedResourcePool::getAverageResponseTime(PoolType type) const {
    std::lock_guard<std::mutex> lock(performanceMutex);
    
    auto it = responseTimeHistory.find(type);
    if (it == responseTimeHistory.end() || it->second.empty()) {
        return 0.0f;
    }
    
    auto history = it->second; // Copy queue
    float total = 0.0f;
    int count = 0;
    
    while (!history.empty()) {
        total += history.front();
        history.pop();
        count++;
    }
    
    return count > 0 ? total / count : 0.0f;
}

// Event notification methods
void SharedResourcePool::notifyResourceAllocated(PoolType type, int instanceId, int channelIndex) {
    if (eventListener) {
        eventListener->onResourceAllocated(type, instanceId, channelIndex);
    }
}

void SharedResourcePool::notifyResourceReleased(PoolType type, int instanceId, int channelIndex) {
    if (eventListener) {
        eventListener->onResourceReleased(type, instanceId, channelIndex);
    }
}

void SharedResourcePool::notifyAllocationFailed(PoolType type, int channelIndex) {
    if (eventListener) {
        eventListener->onAllocationFailed(type, channelIndex);
    }
}

void SharedResourcePool::notifyUtilizationAlert(PoolType type, float utilization) {
    if (eventListener) {
        eventListener->onUtilizationAlert(type, utilization);
    }
}

// Additional methods implementation
bool SharedResourcePool::expandPool(PoolType type, int additionalInstances) {
    std::lock_guard<std::mutex> lock(poolsMutex);

    auto poolIt = resourcePools.find(type);
    if (poolIt == resourcePools.end()) {
        return false;
    }

    auto& pool = poolIt->second;
    auto& config = poolConfigs[type];

    int actualAdded = 0;
    for (int i = 0; i < additionalInstances; i++) {
        if (pool.size() >= config.maxSize) {
            break;
        }

        auto resource = createResourceInstance(type);
        if (resource) {
            int newId = pool.size();
            auto instance = std::make_unique<ResourceInstance>(newId, type, resource);
            pool.push_back(std::move(instance));

            poolStats[type].totalInstances++;
            poolStats[type].idleInstances++;
            poolStats[type].dynamicExpansions++;
            actualAdded++;
        }
    }

    if (actualAdded > 0) {
        notifyPoolExpanded(type, pool.size());
        LOGD("Expanded %s pool by %d instances (total: %zu)",
             poolTypeToString(type).c_str(), actualAdded, pool.size());
    }

    return actualAdded > 0;
}

bool SharedResourcePool::shrinkPool(PoolType type, int targetSize) {
    std::lock_guard<std::mutex> lock(poolsMutex);

    auto poolIt = resourcePools.find(type);
    if (poolIt == resourcePools.end()) {
        return false;
    }

    auto& pool = poolIt->second;
    auto& config = poolConfigs[type];

    if (targetSize < config.minSize) {
        targetSize = config.minSize;
    }

    int toRemove = pool.size() - targetSize;
    int actualRemoved = 0;

    // Remove idle instances from the end
    auto it = pool.end();
    while (it != pool.begin() && actualRemoved < toRemove) {
        --it;
        if (!(*it)->inUse) {
            it = pool.erase(it);
            poolStats[type].totalInstances--;
            poolStats[type].idleInstances--;
            poolStats[type].dynamicShrinks++;
            actualRemoved++;
        }
    }

    if (actualRemoved > 0) {
        notifyPoolShrunk(type, pool.size());
        LOGD("Shrunk %s pool by %d instances (total: %zu)",
             poolTypeToString(type).c_str(), actualRemoved, pool.size());
    }

    return actualRemoved > 0;
}

void SharedResourcePool::performDynamicResize() {
    std::lock_guard<std::mutex> lock(poolsMutex);

    for (auto& pair : poolStats) {
        PoolType type = pair.first;
        auto& stats = pair.second;
        auto& config = poolConfigs[type];

        if (!config.enableDynamicResize) continue;

        auto& pool = resourcePools[type];

        // Expand if high utilization
        if (stats.utilizationRate > config.utilizationThreshold && pool.size() < config.maxSize) {
            expandPool(type, 1);
        }
        // Shrink if low utilization
        else if (stats.utilizationRate < 0.3f && pool.size() > config.minSize) {
            shrinkPool(type, pool.size() - 1);
        }
    }
}

void SharedResourcePool::reclaimIdleResources() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(poolsMutex);

    for (auto& poolPair : resourcePools) {
        PoolType type = poolPair.first;
        auto& pool = poolPair.second;
        auto& config = poolConfigs[type];

        for (auto& instance : pool) {
            if (!instance->inUse) {
                auto idleTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - instance->lastUsed);

                if (idleTime.count() > config.idleTimeoutMs) {
                    // Mark for potential removal in next shrink operation
                    LOGD("Instance %d in %s pool has been idle for %ldms",
                         instance->instanceId, poolTypeToString(type).c_str(), idleTime.count());
                }
            }
        }
    }
}

void SharedResourcePool::balanceLoad() {
    // Implement load balancing across pool instances
    std::lock_guard<std::mutex> lock(poolsMutex);

    for (auto& poolPair : resourcePools) {
        PoolType type = poolPair.first;
        auto& pool = poolPair.second;

        // Calculate average usage
        int totalUsage = 0;
        int activeInstances = 0;

        for (auto& instance : pool) {
            if (instance->inUse) {
                totalUsage += instance->usageCount;
                activeInstances++;
            }
        }

        if (activeInstances > 0) {
            float avgUsage = static_cast<float>(totalUsage) / activeInstances;
            LOGD("Load balancing %s pool: avg usage %.2f across %d active instances",
                 poolTypeToString(type).c_str(), avgUsage, activeInstances);
        }
    }
}

void SharedResourcePool::setChannelAffinity(int channelIndex, PoolType type, int instanceId) {
    std::lock_guard<std::mutex> lock(affinityMutex);
    channelAffinity[channelIndex][type] = instanceId;
    LOGD("Set affinity for channel %d to instance %d in %s pool",
         channelIndex, instanceId, poolTypeToString(type).c_str());
}

int SharedResourcePool::getChannelAffinity(int channelIndex, PoolType type) const {
    std::lock_guard<std::mutex> lock(affinityMutex);

    auto channelIt = channelAffinity.find(channelIndex);
    if (channelIt != channelAffinity.end()) {
        auto typeIt = channelIt->second.find(type);
        if (typeIt != channelIt->second.end()) {
            return typeIt->second;
        }
    }

    return -1; // No affinity set
}

void SharedResourcePool::clearChannelAffinity(int channelIndex) {
    std::lock_guard<std::mutex> lock(affinityMutex);
    channelAffinity.erase(channelIndex);
    LOGD("Cleared affinity for channel %d", channelIndex);
}

SharedResourcePool::PoolStatistics SharedResourcePool::getPoolStatistics(PoolType type) const {
    std::lock_guard<std::mutex> lock(poolsMutex);

    auto it = poolStats.find(type);
    if (it != poolStats.end()) {
        return it->second;
    }

    return PoolStatistics(type);
}

std::map<SharedResourcePool::PoolType, SharedResourcePool::PoolStatistics>
SharedResourcePool::getAllPoolStatistics() const {
    std::lock_guard<std::mutex> lock(poolsMutex);
    return poolStats;
}

float SharedResourcePool::getPoolUtilization(PoolType type) const {
    auto stats = getPoolStatistics(type);
    return stats.utilizationRate;
}

std::vector<int> SharedResourcePool::getActiveChannels() const {
    std::vector<int> activeChannels;
    std::lock_guard<std::mutex> lock(poolsMutex);

    for (const auto& poolPair : resourcePools) {
        for (const auto& instance : poolPair.second) {
            if (instance->inUse) {
                int channel = instance->assignedChannel;
                if (channel >= 0 && std::find(activeChannels.begin(), activeChannels.end(), channel) == activeChannels.end()) {
                    activeChannels.push_back(channel);
                }
            }
        }
    }

    return activeChannels;
}

void SharedResourcePool::setEventListener(PoolEventListener* listener) {
    eventListener = listener;
}

std::string SharedResourcePool::generatePoolReport() const {
    std::ostringstream report;

    report << "=== Shared Resource Pool Report ===\n";

    auto allStats = getAllPoolStatistics();
    for (const auto& pair : allStats) {
        const auto& stats = pair.second;

        report << "\n" << poolTypeToString(pair.first) << " Pool:\n";
        report << "  Total Instances: " << stats.totalInstances << "\n";
        report << "  Active Instances: " << stats.activeInstances << "\n";
        report << "  Idle Instances: " << stats.idleInstances << "\n";
        report << "  Utilization Rate: " << std::fixed << std::setprecision(2)
               << stats.utilizationRate * 100.0f << "%\n";
        report << "  Average Response Time: " << stats.averageResponseTime << "ms\n";
        report << "  Total Requests: " << stats.totalRequests << "\n";
        report << "  Successful Allocations: " << stats.successfulAllocations << "\n";
        report << "  Failed Allocations: " << stats.failedAllocations << "\n";
        report << "  Dynamic Expansions: " << stats.dynamicExpansions << "\n";
        report << "  Dynamic Shrinks: " << stats.dynamicShrinks << "\n";

        if (!stats.channelUsage.empty()) {
            report << "  Channel Usage:\n";
            for (const auto& usage : stats.channelUsage) {
                report << "    Channel " << usage.first << ": " << usage.second << " allocations\n";
            }
        }
    }

    return report.str();
}

std::vector<std::string> SharedResourcePool::getOptimizationRecommendations() const {
    std::vector<std::string> recommendations;

    auto allStats = getAllPoolStatistics();
    for (const auto& pair : allStats) {
        const auto& stats = pair.second;
        std::string poolName = poolTypeToString(pair.first);

        if (stats.utilizationRate > 0.9f) {
            recommendations.push_back(poolName + " pool is highly utilized. Consider expanding the pool size.");
        }

        if (stats.failedAllocations > stats.successfulAllocations * 0.1f) {
            recommendations.push_back(poolName + " pool has high allocation failure rate. Increase pool size or optimize allocation strategy.");
        }

        if (stats.averageResponseTime > 50.0f) {
            recommendations.push_back(poolName + " pool has high response time. Consider optimizing resource creation or allocation strategy.");
        }

        if (stats.utilizationRate < 0.2f && stats.totalInstances > 2) {
            recommendations.push_back(poolName + " pool has low utilization. Consider reducing pool size to save resources.");
        }
    }

    return recommendations;
}

void SharedResourcePool::notifyPoolExpanded(PoolType type, int newSize) {
    if (eventListener) {
        eventListener->onPoolExpanded(type, newSize);
    }
}

void SharedResourcePool::notifyPoolShrunk(PoolType type, int newSize) {
    if (eventListener) {
        eventListener->onPoolShrunk(type, newSize);
    }
}

// ResourcePoolManager implementation
ResourcePoolManager::ResourcePoolManager() {
    LOGD("ResourcePoolManager created");
}

ResourcePoolManager::~ResourcePoolManager() {
    cleanup();
    LOGD("ResourcePoolManager destroyed");
}

bool ResourcePoolManager::initialize(char* modelData, int modelSize, const SystemConfiguration& config) {
    systemConfig = config;

    // Initialize shared resource pool
    sharedPool = std::make_unique<SharedResourcePool>();
    if (!sharedPool->initialize(modelData, modelSize)) {
        LOGE("Failed to initialize shared resource pool");
        return false;
    }

    // Initialize resource manager
    resourceManager = std::make_unique<ResourceManager>();

    // Initialize decoder resource sharing
    decoderSharing = std::make_unique<DecoderResourceSharing>();
    if (!decoderSharing->initialize()) {
        LOGE("Failed to initialize decoder resource sharing");
        return false;
    }

    LOGD("ResourcePoolManager initialized successfully");
    return true;
}

void ResourcePoolManager::cleanup() {
    if (sharedPool) {
        sharedPool->cleanup();
        sharedPool.reset();
    }

    if (resourceManager) {
        resourceManager->cleanup();
        resourceManager.reset();
    }

    if (decoderSharing) {
        decoderSharing->cleanup();
        decoderSharing.reset();
    }

    LOGD("ResourcePoolManager cleanup completed");
}

bool ResourcePoolManager::allocateChannelResources(int channelIndex, int priority) {
    std::lock_guard<std::mutex> lock(managerMutex);

    if (!sharedPool || !resourceManager || !decoderSharing) {
        LOGE("Resource managers not initialized");
        return false;
    }

    // Allocate YOLOv5 thread pool
    auto yolov5Pool = sharedPool->allocateYolov5ThreadPool(channelIndex, priority);
    if (!yolov5Pool) {
        LOGE("Failed to allocate YOLOv5 thread pool for channel %d", channelIndex);
        return false;
    }

    // Allocate decoder
    if (!decoderSharing->addChannel(channelIndex, DecoderResourceSharing::H264_DECODER, priority)) {
        LOGE("Failed to add channel %d to decoder sharing", channelIndex);
        return false;
    }

    // Allocate other resources through resource manager
    if (!resourceManager->addChannel(channelIndex, priority)) {
        LOGE("Failed to add channel %d to resource manager", channelIndex);
        return false;
    }

    LOGD("Allocated resources for channel %d with priority %d", channelIndex, priority);
    return true;
}

bool ResourcePoolManager::releaseChannelResources(int channelIndex) {
    std::lock_guard<std::mutex> lock(managerMutex);

    bool success = true;

    // Release from shared pool
    if (sharedPool) {
        success &= sharedPool->releaseChannelResources(channelIndex);
    }

    // Release from decoder sharing
    if (decoderSharing) {
        success &= decoderSharing->removeChannel(channelIndex);
    }

    // Release from resource manager
    if (resourceManager) {
        success &= resourceManager->removeChannel(channelIndex);
    }

    LOGD("Released resources for channel %d: %s", channelIndex, success ? "SUCCESS" : "PARTIAL");
    return success;
}

std::shared_ptr<Yolov5ThreadPool> ResourcePoolManager::getYolov5ThreadPool(int channelIndex) {
    return sharedPool ? sharedPool->allocateYolov5ThreadPool(channelIndex) : nullptr;
}

std::shared_ptr<MppDecoder> ResourcePoolManager::getMppDecoder(int channelIndex) {
    return decoderSharing ? decoderSharing->acquireDecoder(channelIndex) : nullptr;
}

std::string ResourcePoolManager::generateSystemReport() const {
    std::ostringstream report;

    report << "=== Resource Pool Manager System Report ===\n";

    if (sharedPool) {
        report << "\n" << sharedPool->generatePoolReport();
    }

    if (decoderSharing) {
        report << "\n" << decoderSharing->generateResourceReport();
    }

    if (resourceManager) {
        report << "\n" << resourceManager->generateResourceReport();
    }

    return report.str();
}

// Missing method implementations for SharedResourcePool

void SharedResourcePool::updatePerformanceMetrics() {
    std::lock_guard<std::mutex> lock(poolsMutex);

    // Update performance metrics for all pool types
    for (auto& poolPair : resourcePools) {
        PoolType type = poolPair.first;
        auto& instances = poolPair.second;

        // Calculate utilization
        int activeInstances = 0;

        for (auto& instance : instances) {
            if (instance && instance->inUse.load()) {
                activeInstances++;
            }
        }

        // Update pool statistics
        auto& stats = poolStats[type];
        stats.utilizationRate = static_cast<float>(activeInstances) / instances.size();
    }

    LOGD("Performance metrics updated");
}

SharedResourcePool::ResourceInstance* SharedResourcePool::selectRoundRobin(PoolType type) {
    auto poolIt = resourcePools.find(type);
    if (poolIt == resourcePools.end() || poolIt->second.empty()) {
        return nullptr;
    }

    auto& instances = poolIt->second;

    // Simple round-robin selection - find first available instance
    for (auto& instance : instances) {
        if (instance && !instance->inUse.load() && instance->resource) {
            return instance.get();
        }
    }

    return nullptr;
}

SharedResourcePool::ResourceInstance* SharedResourcePool::selectByPriority(PoolType type, int channelIndex) {
    auto poolIt = resourcePools.find(type);
    if (poolIt == resourcePools.end() || poolIt->second.empty()) {
        return nullptr;
    }

    auto& instances = poolIt->second;
    ResourceInstance* bestInstance = nullptr;
    int lowestUsage = INT_MAX;

    // Find instance with lowest usage count (as a simple priority metric)
    for (auto& instance : instances) {
        if (instance && !instance->inUse.load() && instance->resource) {
            int usage = instance->usageCount.load();
            if (usage < lowestUsage) {
                lowestUsage = usage;
                bestInstance = instance.get();
            }
        }
    }

    return bestInstance;
}

bool SharedResourcePool::releaseChannelResources(int channelIndex) {
    std::lock_guard<std::mutex> lock(poolsMutex);
    bool success = true;

    // Release resources from all pools for this channel
    for (auto& poolPair : resourcePools) {
        auto& instances = poolPair.second;

        for (auto& instance : instances) {
            if (instance && instance->inUse.load() && instance->assignedChannel.load() == channelIndex) {
                instance->inUse.store(false);
                instance->assignedChannel.store(-1);
                instance->lastUsed = std::chrono::steady_clock::now();

                LOGD("Released resource from pool type %d for channel %d",
                     static_cast<int>(poolPair.first), channelIndex);
            }
        }
    }

    return success;
}
