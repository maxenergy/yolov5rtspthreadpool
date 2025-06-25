#include "ResourceManager.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

ResourceManager::ResourceManager(long systemMemory, int cpuCores, int maxChannels)
    : currentStrategy(ADAPTIVE), eventListener(nullptr), shouldStop(false),
      totalSystemMemory(systemMemory), totalCpuCores(cpuCores), maxConcurrentChannels(maxChannels) {
    
    // Initialize resource quotas
    resourceQuotas[MEMORY] = std::make_unique<ResourceQuota>(MEMORY, systemMemory * 0.8); // 80% of system memory
    resourceQuotas[CPU] = std::make_unique<ResourceQuota>(CPU, cpuCores * 100); // 100% per core
    resourceQuotas[GPU] = std::make_unique<ResourceQuota>(GPU, 100); // 100% GPU usage
    resourceQuotas[DECODER] = std::make_unique<ResourceQuota>(DECODER, maxChannels);
    resourceQuotas[ENCODER] = std::make_unique<ResourceQuota>(ENCODER, maxChannels / 2); // Fewer encoders
    resourceQuotas[NETWORK] = std::make_unique<ResourceQuota>(NETWORK, 1000 * 1024 * 1024); // 1Gbps
    resourceQuotas[STORAGE] = std::make_unique<ResourceQuota>(STORAGE, 10 * 1024 * 1024 * 1024); // 10GB
    
    // Start monitoring thread
    monitorThread = std::thread(&ResourceManager::monitorLoop, this);
    
    LOGD("ResourceManager initialized: Memory=%ldMB, CPU=%d cores, MaxChannels=%d", 
         systemMemory / (1024 * 1024), cpuCores, maxChannels);
}

ResourceManager::~ResourceManager() {
    cleanup();
}

void ResourceManager::setResourceLimit(ResourceType type, long maxAmount) {
    auto lock = lockResources();
    
    ResourceQuota* quota = getResourceQuota(type);
    if (quota) {
        quota->maxAmount = maxAmount;
        LOGD("Updated resource limit for %s: %ld", resourceTypeToString(type).c_str(), maxAmount);
    }
}

void ResourceManager::setAllocationStrategy(AllocationStrategy strategy) {
    currentStrategy = strategy;
    LOGD("Set allocation strategy to %d", strategy);
}

void ResourceManager::setChannelPriority(int channelIndex, int priority) {
    auto lock = lockResources();
    
    ChannelResourceInfo* channelInfo = getChannelResourceInfo(channelIndex);
    if (channelInfo) {
        channelInfo->priority = priority;
        LOGD("Set priority for channel %d: %d", channelIndex, priority);
    }
}

bool ResourceManager::addChannel(int channelIndex, int priority) {
    auto lock = lockResources();
    
    if (channelResources.find(channelIndex) != channelResources.end()) {
        LOGW("Channel %d already exists", channelIndex);
        return false;
    }
    
    if (channelResources.size() >= static_cast<size_t>(maxConcurrentChannels)) {
        LOGE("Cannot add channel %d: maximum channels (%d) reached", channelIndex, maxConcurrentChannels);
        return false;
    }
    
    auto channelInfo = std::make_unique<ChannelResourceInfo>(channelIndex, priority);
    channelResources[channelIndex] = std::move(channelInfo);
    
    LOGD("Added channel %d with priority %d", channelIndex, priority);
    return true;
}

bool ResourceManager::removeChannel(int channelIndex) {
    auto lock = lockResources();
    
    auto it = channelResources.find(channelIndex);
    if (it == channelResources.end()) {
        return false;
    }
    
    ChannelResourceInfo* channelInfo = it->second.get();
    
    // Deallocate all resources for this channel
    for (auto& allocation : channelInfo->allocatedResources) {
        performDeallocation(channelIndex, allocation.first, allocation.second);
    }
    
    channelResources.erase(it);
    
    LOGD("Removed channel %d", channelIndex);
    return true;
}

bool ResourceManager::activateChannel(int channelIndex) {
    auto lock = lockResources();
    
    ChannelResourceInfo* channelInfo = getChannelResourceInfo(channelIndex);
    if (!channelInfo) {
        return false;
    }
    
    channelInfo->isActive = true;
    channelInfo->lastUpdate = std::chrono::steady_clock::now();
    
    LOGD("Activated channel %d", channelIndex);
    return true;
}

bool ResourceManager::deactivateChannel(int channelIndex) {
    auto lock = lockResources();
    
    ChannelResourceInfo* channelInfo = getChannelResourceInfo(channelIndex);
    if (!channelInfo) {
        return false;
    }
    
    channelInfo->isActive = false;
    
    LOGD("Deactivated channel %d", channelIndex);
    return true;
}

bool ResourceManager::allocateResource(int channelIndex, ResourceType type, long amount) {
    auto lock = lockResources();
    
    ChannelResourceInfo* channelInfo = getChannelResourceInfo(channelIndex);
    if (!channelInfo) {
        LOGE("Channel %d not found", channelIndex);
        return false;
    }
    
    return performAllocation(channelIndex, type, amount);
}

bool ResourceManager::deallocateResource(int channelIndex, ResourceType type, long amount) {
    auto lock = lockResources();
    
    ChannelResourceInfo* channelInfo = getChannelResourceInfo(channelIndex);
    if (!channelInfo) {
        return false;
    }
    
    return performDeallocation(channelIndex, type, amount);
}

bool ResourceManager::requestResource(int channelIndex, ResourceType type, long amount) {
    auto lock = lockResources();
    
    ChannelResourceInfo* channelInfo = getChannelResourceInfo(channelIndex);
    if (!channelInfo) {
        return false;
    }
    
    // Calculate optimal allocation based on strategy
    long optimalAmount = calculateOptimalAllocation(channelIndex, type, amount);
    
    if (optimalAmount > 0) {
        channelInfo->requestedResources[type] = amount;
        return performAllocation(channelIndex, type, optimalAmount);
    }
    
    return false;
}

void ResourceManager::updateResourceUsage(int channelIndex, ResourceType type, long actualUsage) {
    auto lock = lockResources();
    
    ChannelResourceInfo* channelInfo = getChannelResourceInfo(channelIndex);
    if (channelInfo) {
        channelInfo->actualUsage[type] = actualUsage;
        channelInfo->lastUpdate = std::chrono::steady_clock::now();
    }
}

long ResourceManager::getAvailableResource(ResourceType type) const {
    auto lock = const_cast<ResourceManager*>(this)->lockResources();
    
    const ResourceQuota* quota = getResourceQuota(type);
    return quota ? (quota->maxAmount - quota->currentUsage) : 0;
}

long ResourceManager::getAllocatedResource(int channelIndex, ResourceType type) const {
    auto lock = const_cast<ResourceManager*>(this)->lockResources();
    
    const ChannelResourceInfo* channelInfo = getChannelResourceInfo(channelIndex);
    if (!channelInfo) {
        return 0;
    }
    
    auto it = channelInfo->allocatedResources.find(type);
    return (it != channelInfo->allocatedResources.end()) ? it->second : 0;
}

long ResourceManager::getTotalResourceUsage(ResourceType type) const {
    auto lock = const_cast<ResourceManager*>(this)->lockResources();
    
    const ResourceQuota* quota = getResourceQuota(type);
    return quota ? quota->currentUsage : 0;
}

float ResourceManager::getResourceUtilization(ResourceType type) const {
    auto lock = const_cast<ResourceManager*>(this)->lockResources();
    
    const ResourceQuota* quota = getResourceQuota(type);
    if (!quota || quota->maxAmount == 0) {
        return 0.0f;
    }
    
    return static_cast<float>(quota->currentUsage) / quota->maxAmount;
}

bool ResourceManager::performAllocation(int channelIndex, ResourceType type, long amount) {
    ResourceQuota* quota = getResourceQuota(type);
    ChannelResourceInfo* channelInfo = getChannelResourceInfo(channelIndex);
    
    if (!quota || !channelInfo) {
        return false;
    }
    
    // Check if enough resources are available
    long available = quota->maxAmount - quota->currentUsage;
    if (amount > available) {
        LOGW("Insufficient resources for channel %d: requested %ld, available %ld", 
             channelIndex, amount, available);
        
        if (eventListener) {
            eventListener->onResourceExhausted(type, amount, available);
        }
        return false;
    }
    
    // Allocate resources
    quota->currentUsage += amount;
    quota->channelAllocations[channelIndex] += amount;
    channelInfo->allocatedResources[type] += amount;
    
    if (eventListener) {
        eventListener->onResourceAllocated(channelIndex, type, amount);
    }
    
    LOGD("Allocated %ld %s to channel %d", amount, resourceTypeToString(type).c_str(), channelIndex);
    return true;
}

bool ResourceManager::performDeallocation(int channelIndex, ResourceType type, long amount) {
    ResourceQuota* quota = getResourceQuota(type);
    ChannelResourceInfo* channelInfo = getChannelResourceInfo(channelIndex);
    
    if (!quota || !channelInfo) {
        return false;
    }
    
    // Check current allocation
    long currentAllocation = channelInfo->allocatedResources[type];
    long actualDeallocation = std::min(amount, currentAllocation);
    
    if (actualDeallocation > 0) {
        quota->currentUsage -= actualDeallocation;
        quota->channelAllocations[channelIndex] -= actualDeallocation;
        channelInfo->allocatedResources[type] -= actualDeallocation;
        
        if (eventListener) {
            eventListener->onResourceDeallocated(channelIndex, type, actualDeallocation);
        }
        
        LOGD("Deallocated %ld %s from channel %d", actualDeallocation, resourceTypeToString(type).c_str(), channelIndex);
    }
    
    return actualDeallocation > 0;
}

long ResourceManager::calculateOptimalAllocation(int channelIndex, ResourceType type, long requested) {
    switch (currentStrategy) {
        case FAIR_SHARE:
            return fairShareAllocation(type, requested, channelResources.size());
        case PRIORITY_BASED:
            return priorityBasedAllocation(channelIndex, type, requested);
        case DEMAND_BASED:
            return demandBasedAllocation(channelIndex, type, requested);
        case ADAPTIVE:
            return adaptiveAllocation(channelIndex, type, requested);
        default:
            return requested;
    }
}

long ResourceManager::fairShareAllocation(ResourceType type, long requested, int totalChannels) {
    if (totalChannels == 0) return 0;
    
    long available = getAvailableResource(type);
    long fairShare = available / totalChannels;
    
    return std::min(requested, fairShare);
}

long ResourceManager::priorityBasedAllocation(int channelIndex, ResourceType type, long requested) {
    ChannelResourceInfo* channelInfo = getChannelResourceInfo(channelIndex);
    if (!channelInfo) return 0;
    
    // Calculate total priority weight
    int totalPriorityWeight = 0;
    for (const auto& pair : channelResources) {
        if (pair.second->isActive) {
            totalPriorityWeight += pair.second->priority;
        }
    }
    
    if (totalPriorityWeight == 0) return 0;
    
    long available = getAvailableResource(type);
    long priorityShare = (available * channelInfo->priority) / totalPriorityWeight;
    
    return std::min(requested, priorityShare);
}

long ResourceManager::demandBasedAllocation(int channelIndex, ResourceType type, long requested) {
    // Calculate total demand
    long totalDemand = 0;
    for (const auto& pair : channelResources) {
        if (pair.second->isActive) {
            auto it = pair.second->requestedResources.find(type);
            if (it != pair.second->requestedResources.end()) {
                totalDemand += it->second;
            }
        }
    }
    
    if (totalDemand == 0) return requested;
    
    long available = getAvailableResource(type);
    long demandShare = (available * requested) / totalDemand;
    
    return std::min(requested, demandShare);
}

long ResourceManager::adaptiveAllocation(int channelIndex, ResourceType type, long requested) {
    ChannelResourceInfo* channelInfo = getChannelResourceInfo(channelIndex);
    if (!channelInfo) return 0;
    
    // Combine priority and demand-based allocation
    long priorityAllocation = priorityBasedAllocation(channelIndex, type, requested);
    long demandAllocation = demandBasedAllocation(channelIndex, type, requested);
    
    // Weight based on channel activity and performance
    float priorityWeight = 0.6f;
    float demandWeight = 0.4f;
    
    // Adjust weights based on actual usage efficiency
    auto usageIt = channelInfo->actualUsage.find(type);
    auto allocatedIt = channelInfo->allocatedResources.find(type);
    
    if (usageIt != channelInfo->actualUsage.end() && allocatedIt != channelInfo->allocatedResources.end()) {
        if (allocatedIt->second > 0) {
            float efficiency = static_cast<float>(usageIt->second) / allocatedIt->second;
            if (efficiency > 0.8f) {
                // High efficiency, favor demand-based allocation
                demandWeight = 0.7f;
                priorityWeight = 0.3f;
            }
        }
    }
    
    long adaptiveAllocation = static_cast<long>(priorityAllocation * priorityWeight + demandAllocation * demandWeight);
    return std::min(requested, adaptiveAllocation);
}

// Resource monitoring and optimization
void ResourceManager::monitorLoop() {
    LOGD("Resource monitor thread started");

    while (!shouldStop) {
        std::unique_lock<std::mutex> lock(monitorMutex);
        monitorCv.wait_for(lock, std::chrono::seconds(5)); // Monitor every 5 seconds

        if (shouldStop) break;

        updateSystemResourceUsage();
        detectResourceLeaks();
        enforceQuotas();
    }

    LOGD("Resource monitor thread stopped");
}

void ResourceManager::updateSystemResourceUsage() {
    auto lock = lockResources();

    // Update resource usage statistics
    for (auto& pair : resourceQuotas) {
        ResourceQuota* quota = pair.second.get();

        // Recalculate current usage from channel allocations
        long totalUsage = 0;
        for (const auto& allocation : quota->channelAllocations) {
            totalUsage += allocation.second;
        }
        quota->currentUsage = totalUsage;
    }
}

void ResourceManager::detectResourceLeaks() {
    auto lock = lockResources();

    auto now = std::chrono::steady_clock::now();

    for (auto& pair : channelResources) {
        ChannelResourceInfo* channelInfo = pair.second.get();

        // Check for inactive channels with allocated resources
        auto timeSinceUpdate = std::chrono::duration_cast<std::chrono::minutes>(
            now - channelInfo->lastUpdate);

        if (!channelInfo->isActive && timeSinceUpdate.count() > 10) { // 10 minutes
            LOGW("Potential resource leak detected for inactive channel %d", channelInfo->channelIndex);

            // Force cleanup of inactive channel resources
            for (auto& allocation : channelInfo->allocatedResources) {
                if (allocation.second > 0) {
                    performDeallocation(channelInfo->channelIndex, allocation.first, allocation.second);
                }
            }
        }
    }
}

void ResourceManager::enforceQuotas() {
    auto lock = lockResources();

    for (auto& pair : resourceQuotas) {
        ResourceQuota* quota = pair.second.get();

        if (quota->currentUsage > quota->maxAmount) {
            LOGW("Resource quota exceeded for %s: %ld/%ld",
                 resourceTypeToString(quota->type).c_str(), quota->currentUsage, quota->maxAmount);

            // Force rebalancing
            rebalanceResources();
            break;
        }
    }
}

void ResourceManager::rebalanceResources() {
    LOGD("Rebalancing resources");

    std::vector<int> affectedChannels;

    // Identify channels that need resource adjustment
    for (auto& pair : channelResources) {
        ChannelResourceInfo* channelInfo = pair.second.get();

        // Check if channel is using more than allocated
        for (auto& usage : channelInfo->actualUsage) {
            ResourceType type = usage.first;
            long actualUsage = usage.second;
            long allocated = channelInfo->allocatedResources[type];

            if (actualUsage > allocated * 1.2f) { // 20% over-allocation threshold
                affectedChannels.push_back(channelInfo->channelIndex);

                // Reduce allocation for over-consuming channels
                long reduction = actualUsage - allocated;
                performDeallocation(channelInfo->channelIndex, type, reduction);
            }
        }
    }

    if (!affectedChannels.empty() && eventListener) {
        eventListener->onResourceRebalanced(affectedChannels);
    }
}

void ResourceManager::optimizeResourceAllocation() {
    auto lock = lockResources();

    LOGD("Optimizing resource allocation");

    // Analyze resource usage patterns and adjust allocations
    for (auto& pair : channelResources) {
        ChannelResourceInfo* channelInfo = pair.second.get();

        if (!channelInfo->isActive) continue;

        for (auto& allocation : channelInfo->allocatedResources) {
            ResourceType type = allocation.first;
            long allocated = allocation.second;

            auto usageIt = channelInfo->actualUsage.find(type);
            if (usageIt != channelInfo->actualUsage.end()) {
                long actualUsage = usageIt->second;

                // If usage is significantly lower than allocation, reduce allocation
                if (actualUsage < allocated * 0.5f && allocated > 0) {
                    long reduction = allocated - actualUsage * 1.2f; // Keep 20% buffer
                    if (reduction > 0) {
                        performDeallocation(channelInfo->channelIndex, type, reduction);
                        LOGD("Optimized allocation for channel %d, %s: reduced by %ld",
                             channelInfo->channelIndex, resourceTypeToString(type).c_str(), reduction);
                    }
                }
            }
        }
    }
}

std::vector<int> ResourceManager::getActiveChannels() const {
    auto lock = const_cast<ResourceManager*>(this)->lockResources();

    std::vector<int> activeChannels;
    for (const auto& pair : channelResources) {
        if (pair.second->isActive) {
            activeChannels.push_back(pair.first);
        }
    }
    return activeChannels;
}

std::map<ResourceManager::ResourceType, float> ResourceManager::getSystemResourceUtilization() const {
    auto lock = const_cast<ResourceManager*>(this)->lockResources();

    std::map<ResourceType, float> utilization;
    for (const auto& pair : resourceQuotas) {
        utilization[pair.first] = getResourceUtilization(pair.first);
    }
    return utilization;
}

std::string ResourceManager::generateResourceReport() const {
    auto lock = const_cast<ResourceManager*>(this)->lockResources();

    std::ostringstream report;
    report << "=== Resource Manager Report ===\n";
    report << "Total Channels: " << channelResources.size() << "\n";
    report << "Active Channels: " << getActiveChannels().size() << "\n";
    report << "Allocation Strategy: " << currentStrategy << "\n\n";

    report << "Resource Utilization:\n";
    for (const auto& pair : resourceQuotas) {
        const ResourceQuota* quota = pair.second.get();
        float utilization = getResourceUtilization(pair.first);

        report << "  " << resourceTypeToString(pair.first) << ": "
               << quota->currentUsage << "/" << quota->maxAmount
               << " (" << std::fixed << std::setprecision(1) << (utilization * 100) << "%)\n";
    }

    report << "\nChannel Resource Allocations:\n";
    for (const auto& pair : channelResources) {
        const ChannelResourceInfo* channelInfo = pair.second.get();
        report << "  Channel " << channelInfo->channelIndex
               << " (Priority: " << channelInfo->priority
               << ", Active: " << (channelInfo->isActive ? "Yes" : "No") << "):\n";

        for (const auto& allocation : channelInfo->allocatedResources) {
            if (allocation.second > 0) {
                report << "    " << resourceTypeToString(allocation.first)
                       << ": " << allocation.second << "\n";
            }
        }
    }

    return report.str();
}

// Utility methods
ResourceManager::ResourceQuota* ResourceManager::getResourceQuota(ResourceType type) {
    auto it = resourceQuotas.find(type);
    return (it != resourceQuotas.end()) ? it->second.get() : nullptr;
}

const ResourceManager::ResourceQuota* ResourceManager::getResourceQuota(ResourceType type) const {
    auto it = resourceQuotas.find(type);
    return (it != resourceQuotas.end()) ? it->second.get() : nullptr;
}

ResourceManager::ChannelResourceInfo* ResourceManager::getChannelResourceInfo(int channelIndex) {
    auto it = channelResources.find(channelIndex);
    return (it != channelResources.end()) ? it->second.get() : nullptr;
}

const ResourceManager::ChannelResourceInfo* ResourceManager::getChannelResourceInfo(int channelIndex) const {
    auto it = channelResources.find(channelIndex);
    return (it != channelResources.end()) ? it->second.get() : nullptr;
}

std::string ResourceManager::resourceTypeToString(ResourceType type) const {
    switch (type) {
        case MEMORY: return "Memory";
        case CPU: return "CPU";
        case GPU: return "GPU";
        case DECODER: return "Decoder";
        case ENCODER: return "Encoder";
        case NETWORK: return "Network";
        case STORAGE: return "Storage";
        default: return "Unknown";
    }
}

void ResourceManager::setEventListener(ResourceEventListener* listener) {
    eventListener = listener;
}

void ResourceManager::cleanup() {
    LOGD("Cleaning up ResourceManager");

    // Stop monitor thread
    shouldStop = true;
    monitorCv.notify_all();

    if (monitorThread.joinable()) {
        monitorThread.join();
    }

    // Clear all data
    auto lock = lockResources();
    channelResources.clear();
    resourceQuotas.clear();

    LOGD("ResourceManager cleanup complete");
}

// MemoryPoolManager implementation
MemoryPoolManager::MemoryPoolManager(size_t maxSize, size_t blockSize)
    : totalPoolSize(0), maxPoolSize(maxSize), blockSize(blockSize) {
    LOGD("MemoryPoolManager initialized: MaxSize=%zuMB, BlockSize=%zuKB",
         maxSize / (1024 * 1024), blockSize / 1024);
}

MemoryPoolManager::~MemoryPoolManager() {
    std::lock_guard<std::mutex> lock(poolMutex);
    memoryBlocks.clear();
    LOGD("MemoryPoolManager destroyed");
}

void* MemoryPoolManager::allocateBlock(int channelIndex, size_t size) {
    std::lock_guard<std::mutex> lock(poolMutex);

    MemoryBlock* block = findAvailableBlock(size);

    if (!block) {
        // Create new block if pool has capacity
        if (totalPoolSize + size <= maxPoolSize) {
            createNewBlock(size);
            block = findAvailableBlock(size);
        } else {
            // Try to free up space by removing unused blocks
            cleanupUnusedBlocks();
            if (totalPoolSize + size <= maxPoolSize) {
                createNewBlock(size);
                block = findAvailableBlock(size);
            }
        }
    }

    if (block) {
        block->inUse = true;
        block->channelIndex = channelIndex;
        block->lastUsed = std::chrono::steady_clock::now();
        return block->data;
    }

    LOGW("Failed to allocate memory block of size %zu for channel %d", size, channelIndex);
    return nullptr;
}

void MemoryPoolManager::deallocateBlock(void* ptr) {
    std::lock_guard<std::mutex> lock(poolMutex);

    MemoryBlock* block = findBlockByPointer(ptr);
    if (block) {
        block->inUse = false;
        block->channelIndex = -1;
        block->lastUsed = std::chrono::steady_clock::now();
    }
}

void MemoryPoolManager::deallocateChannelBlocks(int channelIndex) {
    std::lock_guard<std::mutex> lock(poolMutex);

    for (auto& block : memoryBlocks) {
        if (block->channelIndex == channelIndex) {
            block->inUse = false;
            block->channelIndex = -1;
            block->lastUsed = std::chrono::steady_clock::now();
        }
    }

    LOGD("Deallocated all blocks for channel %d", channelIndex);
}

MemoryPoolManager::MemoryBlock* MemoryPoolManager::findAvailableBlock(size_t size) {
    for (auto& block : memoryBlocks) {
        if (!block->inUse && block->size >= size) {
            return block.get();
        }
    }
    return nullptr;
}

MemoryPoolManager::MemoryBlock* MemoryPoolManager::findBlockByPointer(void* ptr) {
    for (auto& block : memoryBlocks) {
        if (block->data == ptr) {
            return block.get();
        }
    }
    return nullptr;
}

void MemoryPoolManager::createNewBlock(size_t size) {
    // Round up to block size
    size_t actualSize = ((size + blockSize - 1) / blockSize) * blockSize;

    auto block = std::make_unique<MemoryBlock>(actualSize);
    if (block->data) {
        totalPoolSize += actualSize;
        memoryBlocks.push_back(std::move(block));
        LOGD("Created new memory block of size %zu", actualSize);
    }
}

void MemoryPoolManager::cleanupUnusedBlocks() {
    auto now = std::chrono::steady_clock::now();

    memoryBlocks.erase(
        std::remove_if(memoryBlocks.begin(), memoryBlocks.end(),
                      [&now](const std::unique_ptr<MemoryBlock>& block) {
                          if (!block->inUse) {
                              auto timeSinceLastUse = std::chrono::duration_cast<std::chrono::minutes>(
                                  now - block->lastUsed);
                              return timeSinceLastUse.count() > 5; // Remove blocks unused for 5 minutes
                          }
                          return false;
                      }),
        memoryBlocks.end()
    );
}

size_t MemoryPoolManager::getUsedPoolSize() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(poolMutex));

    size_t usedSize = 0;
    for (const auto& block : memoryBlocks) {
        if (block->inUse) {
            usedSize += block->size;
        }
    }
    return usedSize;
}

size_t MemoryPoolManager::getAvailablePoolSize() const {
    return totalPoolSize - getUsedPoolSize();
}

int MemoryPoolManager::getBlockCount() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(poolMutex));
    return memoryBlocks.size();
}

int MemoryPoolManager::getUsedBlockCount() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(poolMutex));

    int usedCount = 0;
    for (const auto& block : memoryBlocks) {
        if (block->inUse) {
            usedCount++;
        }
    }
    return usedCount;
}

// CPUResourceAllocator implementation
CPUResourceAllocator::CPUResourceAllocator(int cores) : totalCores(cores) {
    coreUsage.resize(cores, false);
    LOGD("CPUResourceAllocator initialized with %d cores", cores);
}

CPUResourceAllocator::~CPUResourceAllocator() {
    std::lock_guard<std::mutex> lock(allocationMutex);
    allocations.clear();
}

bool CPUResourceAllocator::allocateCPU(int channelIndex, float cpuQuota, int priority) {
    std::lock_guard<std::mutex> lock(allocationMutex);

    if (allocations.find(channelIndex) != allocations.end()) {
        LOGW("CPU already allocated for channel %d", channelIndex);
        return false;
    }

    auto allocation = std::make_unique<CPUAllocation>(channelIndex, cpuQuota, priority);
    assignCores(allocation.get());

    allocations[channelIndex] = std::move(allocation);

    LOGD("Allocated %.1f%% CPU to channel %d", cpuQuota, channelIndex);
    return true;
}

bool CPUResourceAllocator::deallocateCPU(int channelIndex) {
    std::lock_guard<std::mutex> lock(allocationMutex);

    auto it = allocations.find(channelIndex);
    if (it == allocations.end()) {
        return false;
    }

    releaseCores(it->second.get());
    allocations.erase(it);

    LOGD("Deallocated CPU for channel %d", channelIndex);
    return true;
}

void CPUResourceAllocator::assignCores(CPUAllocation* allocation) {
    if (!allocation) return;

    // Calculate number of cores needed based on quota
    int coresNeeded = std::max(1, static_cast<int>(allocation->cpuQuota / 100.0f * totalCores));

    std::vector<int> availableCores;
    for (int i = 0; i < totalCores; ++i) {
        if (!coreUsage[i]) {
            availableCores.push_back(i);
        }
    }

    // Assign cores up to the needed amount
    int assigned = 0;
    for (int core : availableCores) {
        if (assigned >= coresNeeded) break;

        allocation->assignedCores.push_back(core);
        coreUsage[core] = true;
        assigned++;
    }
}

void CPUResourceAllocator::releaseCores(CPUAllocation* allocation) {
    if (!allocation) return;

    for (int core : allocation->assignedCores) {
        if (core >= 0 && core < totalCores) {
            coreUsage[core] = false;
        }
    }
    allocation->assignedCores.clear();
}

std::vector<int> CPUResourceAllocator::getAssignedCores(int channelIndex) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(allocationMutex));

    auto it = allocations.find(channelIndex);
    return (it != allocations.end()) ? it->second->assignedCores : std::vector<int>();
}

float CPUResourceAllocator::getTotalCPUUsage() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(allocationMutex));

    float totalUsage = 0.0f;
    for (const auto& pair : allocations) {
        totalUsage += pair.second->cpuQuota;
    }
    return std::min(100.0f, totalUsage);
}

std::vector<int> CPUResourceAllocator::getAvailableCores() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(allocationMutex));

    std::vector<int> availableCores;
    for (int i = 0; i < totalCores; ++i) {
        if (!coreUsage[i]) {
            availableCores.push_back(i);
        }
    }
    return availableCores;
}

// ResourceIsolationManager implementation
ResourceIsolationManager::ResourceIsolationManager(IsolationLevel defaultLevel)
    : defaultIsolationLevel(defaultLevel) {
    LOGD("ResourceIsolationManager initialized with default level %d", defaultLevel);
}

void ResourceIsolationManager::setChannelIsolationPolicy(int channelIndex, const IsolationPolicy& policy) {
    std::lock_guard<std::mutex> lock(policyMutex);
    channelPolicies[channelIndex] = policy;
    LOGD("Set isolation policy for channel %d: level %d", channelIndex, policy.level);
}

void ResourceIsolationManager::setDefaultIsolationLevel(IsolationLevel level) {
    defaultIsolationLevel = level;
    LOGD("Set default isolation level to %d", level);
}

ResourceIsolationManager::IsolationPolicy ResourceIsolationManager::getChannelIsolationPolicy(int channelIndex) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(policyMutex));

    auto it = channelPolicies.find(channelIndex);
    if (it != channelPolicies.end()) {
        return it->second;
    }

    // Return default policy
    IsolationPolicy defaultPolicy(defaultIsolationLevel);
    return defaultPolicy;
}

bool ResourceIsolationManager::canShareResource(int channelIndex1, int channelIndex2,
                                               ResourceManager::ResourceType type) const {
    IsolationPolicy policy1 = getChannelIsolationPolicy(channelIndex1);
    IsolationPolicy policy2 = getChannelIsolationPolicy(channelIndex2);

    // Check if either channel has strict isolation for this resource type
    bool resource1Isolated = isResourceIsolated(channelIndex1, type);
    bool resource2Isolated = isResourceIsolated(channelIndex2, type);

    if (resource1Isolated || resource2Isolated) {
        return false;
    }

    // Check if sharing is allowed by policies
    if (!policy1.allowResourceSharing || !policy2.allowResourceSharing) {
        return false;
    }

    // Check isolation levels
    IsolationLevel level1 = getEffectiveIsolationLevel(channelIndex1);
    IsolationLevel level2 = getEffectiveIsolationLevel(channelIndex2);

    if (level1 >= STRICT || level2 >= STRICT) {
        return false;
    }

    return true;
}

bool ResourceIsolationManager::enforceIsolation(int channelIndex, ResourceManager::ResourceType type, long amount) {
    IsolationPolicy policy = getChannelIsolationPolicy(channelIndex);

    // Check if this resource type should be isolated
    if (isResourceIsolated(channelIndex, type)) {
        LOGD("Enforcing isolation for channel %d, resource %d", channelIndex, type);

        // In strict isolation, each channel gets dedicated resources
        // Implementation would depend on the specific resource type
        return true;
    }

    return false; // No isolation enforcement needed
}

void ResourceIsolationManager::validateResourceAccess(int channelIndex, ResourceManager::ResourceType type) {
    IsolationLevel level = getEffectiveIsolationLevel(channelIndex);

    if (level >= COMPLETE) {
        // Complete isolation - validate that no other channels are using this resource
        LOGD("Validating complete isolation for channel %d, resource %d", channelIndex, type);

        // Implementation would check for resource conflicts
    }
}

std::vector<std::string> ResourceIsolationManager::detectIsolationViolations() const {
    std::vector<std::string> violations;

    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(policyMutex));

    // Check for isolation violations between channels
    for (const auto& pair1 : channelPolicies) {
        for (const auto& pair2 : channelPolicies) {
            if (pair1.first >= pair2.first) continue; // Avoid duplicate checks

            int channel1 = pair1.first;
            int channel2 = pair2.first;
            const IsolationPolicy& policy1 = pair1.second;
            const IsolationPolicy& policy2 = pair2.second;

            // Check if channels with strict isolation are sharing resources
            if (policy1.level >= STRICT || policy2.level >= STRICT) {
                // Implementation would check actual resource sharing
                // For now, just log potential violations

                for (ResourceManager::ResourceType type : policy1.isolatedResources) {
                    if (std::find(policy2.isolatedResources.begin(), policy2.isolatedResources.end(), type)
                        != policy2.isolatedResources.end()) {

                        std::ostringstream oss;
                        oss << "Potential isolation violation between channels "
                            << channel1 << " and " << channel2 << " for resource type " << static_cast<int>(type);
                        violations.push_back(oss.str());
                    }
                }
            }
        }
    }

    return violations;
}

void ResourceIsolationManager::reportIsolationStatus() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(policyMutex));

    LOGD("=== Resource Isolation Status ===");
    LOGD("Default Isolation Level: %d", defaultIsolationLevel);
    LOGD("Channel Policies:");

    for (const auto& pair : channelPolicies) {
        const IsolationPolicy& policy = pair.second;
        LOGD("  Channel %d: Level=%d, Sharing=%s, MaxShared=%d, IsolatedResources=%zu",
             pair.first, policy.level,
             policy.allowResourceSharing ? "Yes" : "No",
             policy.maxSharedChannels,
             policy.isolatedResources.size());
    }

    auto violations = detectIsolationViolations();
    if (!violations.empty()) {
        LOGW("Isolation Violations Detected:");
        for (const auto& violation : violations) {
            LOGW("  %s", violation.c_str());
        }
    }
}

bool ResourceIsolationManager::isResourceIsolated(int channelIndex, ResourceManager::ResourceType type) const {
    IsolationPolicy policy = getChannelIsolationPolicy(channelIndex);

    return std::find(policy.isolatedResources.begin(), policy.isolatedResources.end(), type)
           != policy.isolatedResources.end();
}

ResourceIsolationManager::IsolationLevel ResourceIsolationManager::getEffectiveIsolationLevel(int channelIndex) const {
    IsolationPolicy policy = getChannelIsolationPolicy(channelIndex);
    return policy.level;
}
