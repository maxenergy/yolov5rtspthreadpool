#include "DecoderResourceSharing.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

DecoderResourceSharing::DecoderResourceSharing()
    : eventListener(nullptr), threadsRunning(false) {
    LOGD("DecoderResourceSharing created");
}

DecoderResourceSharing::~DecoderResourceSharing() {
    cleanup();
    LOGD("DecoderResourceSharing destroyed");
}

bool DecoderResourceSharing::initialize(const DecoderResourceConfig& config) {
    this->config = config;
    
    // Create shared pools for each decoder type
    if (!createSharedPool(H264_DECODER, config.maxDecodersPerType / 2)) {
        LOGE("Failed to create H264 decoder pool");
        return false;
    }
    
    if (!createSharedPool(H265_DECODER, config.maxDecodersPerType / 2)) {
        LOGE("Failed to create H265 decoder pool");
        return false;
    }
    
    if (!createSharedPool(GENERIC_DECODER, config.maxDecodersPerType)) {
        LOGE("Failed to create generic decoder pool");
        return false;
    }
    
    // Start management threads
    threadsRunning = true;
    resourceManagerThread = std::thread(&DecoderResourceSharing::resourceManagerLoop, this);
    statisticsThread = std::thread(&DecoderResourceSharing::statisticsLoop, this);
    
    LOGD("DecoderResourceSharing initialized with strategy: %s", 
         sharingStrategyToString(config.strategy).c_str());
    return true;
}

void DecoderResourceSharing::cleanup() {
    // Stop threads
    threadsRunning = false;
    resourceManagerCv.notify_all();
    statisticsCv.notify_all();
    
    if (resourceManagerThread.joinable()) {
        resourceManagerThread.join();
    }
    
    if (statisticsThread.joinable()) {
        statisticsThread.join();
    }
    
    // Clear channels
    {
        std::lock_guard<std::mutex> lock(channelsMutex);
        channels.clear();
    }
    
    // Clear shared pools
    {
        std::lock_guard<std::mutex> lock(poolsMutex);
        sharedPools.clear();
    }
    
    LOGD("DecoderResourceSharing cleanup completed");
}

bool DecoderResourceSharing::addChannel(int channelIndex, DecoderType type, int priority) {
    if (!validateChannelIndex(channelIndex)) {
        LOGE("Invalid channel index: %d", channelIndex);
        return false;
    }
    
    std::lock_guard<std::mutex> lock(channelsMutex);
    
    // Check if channel already exists
    if (channels.find(channelIndex) != channels.end()) {
        LOGW("Channel %d already exists", channelIndex);
        return false;
    }
    
    // Create channel info
    auto channelInfo = std::make_unique<ChannelDecoderInfo>(channelIndex, type);
    channelInfo->priority = priority;
    
    channels[channelIndex] = std::move(channelInfo);
    
    LOGD("Added channel %d with type %s and priority %d", 
         channelIndex, decoderTypeToString(type).c_str(), priority);
    return true;
}

bool DecoderResourceSharing::removeChannel(int channelIndex) {
    std::lock_guard<std::mutex> lock(channelsMutex);
    
    auto it = channels.find(channelIndex);
    if (it == channels.end()) {
        LOGW("Channel %d not found", channelIndex);
        return false;
    }
    
    // Release all assigned decoders
    auto& channelInfo = it->second;
    for (auto& decoder : channelInfo->assignedDecoders) {
        releaseDecoder(channelIndex, decoder);
    }
    
    channels.erase(it);
    
    LOGD("Removed channel %d", channelIndex);
    return true;
}

std::shared_ptr<MppDecoder> DecoderResourceSharing::acquireDecoder(int channelIndex) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        LOGE("Channel %d not found", channelIndex);
        return nullptr;
    }
    
    std::shared_ptr<MppDecoder> decoder;
    
    // Select allocation strategy
    switch (config.strategy) {
        case EXCLUSIVE:
            decoder = allocateExclusiveDecoder(channelIndex);
            break;
        case SHARED_POOL:
            decoder = allocateFromSharedPool(channelIndex);
            break;
        case ADAPTIVE:
            decoder = allocateAdaptive(channelIndex);
            break;
        case PRIORITY_BASED:
            decoder = allocatePriorityBased(channelIndex);
            break;
        case LOAD_BALANCED:
            decoder = allocateLoadBalanced(channelIndex);
            break;
        default:
            decoder = allocateFromSharedPool(channelIndex);
            break;
    }
    
    if (decoder) {
        std::lock_guard<std::mutex> channelLock(channelInfo->channelMutex);
        channelInfo->assignedDecoders.push_back(decoder);
        channelInfo->activeDecoders++;
        channelInfo->lastUsed = std::chrono::steady_clock::now();
        
        notifyDecoderAssigned(channelIndex, decoder);
        LOGD("Acquired decoder for channel %d", channelIndex);
    } else {
        LOGW("Failed to acquire decoder for channel %d", channelIndex);
        notifyResourceContention(channelIndex, channelInfo->decoderType);
    }
    
    return decoder;
}

bool DecoderResourceSharing::releaseDecoder(int channelIndex, std::shared_ptr<MppDecoder> decoder) {
    if (!decoder) {
        return false;
    }
    
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return false;
    }
    
    std::lock_guard<std::mutex> channelLock(channelInfo->channelMutex);
    
    // Remove from assigned decoders
    auto it = std::find(channelInfo->assignedDecoders.begin(), 
                       channelInfo->assignedDecoders.end(), decoder);
    if (it != channelInfo->assignedDecoders.end()) {
        channelInfo->assignedDecoders.erase(it);
        channelInfo->activeDecoders--;
        
        // Return to appropriate pool if using shared strategy
        if (config.strategy == SHARED_POOL || config.strategy == ADAPTIVE || 
            config.strategy == PRIORITY_BASED || config.strategy == LOAD_BALANCED) {
            
            auto pool = getSharedPool(channelInfo->decoderType);
            if (pool) {
                std::lock_guard<std::mutex> poolLock(pool->poolMutex);
                pool->availableDecoders.push(decoder);
                pool->availableCount++;
                pool->activeCount--;
                
                // Remove from active assignments
                pool->activeAssignments.erase(channelIndex);
            }
        }
        
        notifyDecoderReleased(channelIndex, decoder);
        LOGD("Released decoder for channel %d", channelIndex);
        return true;
    }
    
    return false;
}

std::shared_ptr<MppDecoder> DecoderResourceSharing::allocateFromSharedPool(int channelIndex) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return nullptr;
    }
    
    auto pool = getSharedPool(channelInfo->decoderType);
    if (!pool) {
        LOGE("No shared pool available for decoder type %s", 
             decoderTypeToString(channelInfo->decoderType).c_str());
        return nullptr;
    }
    
    std::lock_guard<std::mutex> poolLock(pool->poolMutex);
    
    if (pool->availableDecoders.empty()) {
        // Try to expand pool if allowed
        if (config.enableDynamicAllocation && 
            pool->totalDecoders < config.maxSharedDecoders) {
            
            auto newDecoder = createDecoder(channelInfo->decoderType);
            if (newDecoder) {
                pool->decoders.push_back(newDecoder);
                pool->totalDecoders++;
                pool->availableDecoders.push(newDecoder);
                pool->availableCount++;
                
                LOGD("Expanded %s pool to %d decoders", 
                     decoderTypeToString(channelInfo->decoderType).c_str(), 
                     pool->totalDecoders.load());
            }
        }
    }
    
    if (!pool->availableDecoders.empty()) {
        auto decoder = pool->availableDecoders.front();
        pool->availableDecoders.pop();
        pool->availableCount--;
        pool->activeCount++;
        pool->activeAssignments[channelIndex] = decoder;
        
        return decoder;
    }
    
    return nullptr;
}

std::shared_ptr<MppDecoder> DecoderResourceSharing::allocateExclusiveDecoder(int channelIndex) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return nullptr;
    }
    
    // Check if channel already has enough exclusive decoders
    if (channelInfo->assignedDecoders.size() >= config.maxDecodersPerChannel) {
        LOGW("Channel %d already has maximum decoders", channelIndex);
        return nullptr;
    }
    
    // Create new exclusive decoder
    auto decoder = createDecoder(channelInfo->decoderType);
    if (decoder) {
        LOGD("Created exclusive decoder for channel %d", channelIndex);
    }
    
    return decoder;
}

std::shared_ptr<MppDecoder> DecoderResourceSharing::allocateAdaptive(int channelIndex) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return nullptr;
    }
    
    // Decide between exclusive and shared based on system load
    float systemUtilization = statistics.averageUtilization;
    
    if (systemUtilization < config.resourceUtilizationThreshold) {
        // Low utilization - try exclusive first
        auto decoder = allocateExclusiveDecoder(channelIndex);
        if (decoder) {
            return decoder;
        }
    }
    
    // High utilization or exclusive failed - use shared pool
    return allocateFromSharedPool(channelIndex);
}

std::shared_ptr<MppDecoder> DecoderResourceSharing::allocatePriorityBased(int channelIndex) {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return nullptr;
    }
    
    // High priority channels get preference
    if (channelInfo->priority >= 3) {
        auto decoder = allocateExclusiveDecoder(channelIndex);
        if (decoder) {
            return decoder;
        }
    }
    
    // Try shared pool
    auto decoder = allocateFromSharedPool(channelIndex);
    if (!decoder && config.enableResourcePreemption && channelInfo->priority >= 2) {
        // Try to preempt from lower priority channel
        auto lowPriorityChannels = identifyLowPriorityChannels();
        for (int lowPriorityChannel : lowPriorityChannels) {
            if (preemptDecoder(lowPriorityChannel, channelIndex)) {
                decoder = allocateFromSharedPool(channelIndex);
                break;
            }
        }
    }
    
    return decoder;
}

std::shared_ptr<MppDecoder> DecoderResourceSharing::allocateLoadBalanced(int channelIndex) {
    // Find the decoder type with lowest utilization
    DecoderType bestType = H264_DECODER;
    int minActiveDecoders = INT_MAX;
    
    std::lock_guard<std::mutex> poolLock(poolsMutex);
    for (const auto& pair : sharedPools) {
        if (pair.second->activeCount < minActiveDecoders) {
            minActiveDecoders = pair.second->activeCount;
            bestType = pair.first;
        }
    }
    
    // Temporarily change channel decoder type for load balancing
    auto channelInfo = getChannelInfo(channelIndex);
    if (channelInfo) {
        DecoderType originalType = channelInfo->decoderType;
        channelInfo->decoderType = bestType;
        
        auto decoder = allocateFromSharedPool(channelIndex);
        
        // Restore original type
        channelInfo->decoderType = originalType;
        
        return decoder;
    }
    
    return nullptr;
}

bool DecoderResourceSharing::createSharedPool(DecoderType type, int initialSize) {
    std::lock_guard<std::mutex> lock(poolsMutex);
    
    if (sharedPools.find(type) != sharedPools.end()) {
        LOGW("Shared pool for type %s already exists", decoderTypeToString(type).c_str());
        return false;
    }
    
    auto pool = std::make_unique<SharedDecoderPool>(type);
    
    // Create initial decoders
    for (int i = 0; i < initialSize; i++) {
        auto decoder = createDecoder(type);
        if (decoder) {
            pool->decoders.push_back(decoder);
            pool->availableDecoders.push(decoder);
            pool->totalDecoders++;
            pool->availableCount++;
        }
    }
    
    sharedPools[type] = std::move(pool);
    
    LOGD("Created shared pool for %s with %d decoders", 
         decoderTypeToString(type).c_str(), initialSize);
    return true;
}

std::shared_ptr<MppDecoder> DecoderResourceSharing::createDecoder(DecoderType type) {
    auto decoder = std::make_shared<MppDecoder>();
    
    // Initialize decoder based on type
    int videoType = (type == H264_DECODER) ? 264 : 
                   (type == H265_DECODER) ? 265 : 264; // Default to H264
    
    if (decoder->Init(videoType, 25, nullptr) == 1) {
        LOGD("Created new %s decoder", decoderTypeToString(type).c_str());
        return decoder;
    } else {
        LOGE("Failed to initialize %s decoder", decoderTypeToString(type).c_str());
        return nullptr;
    }
}

DecoderResourceSharing::SharedDecoderPool* DecoderResourceSharing::getSharedPool(DecoderType type) {
    std::lock_guard<std::mutex> lock(poolsMutex);
    
    auto it = sharedPools.find(type);
    return (it != sharedPools.end()) ? it->second.get() : nullptr;
}

DecoderResourceSharing::ChannelDecoderInfo* DecoderResourceSharing::getChannelInfo(int channelIndex) {
    std::lock_guard<std::mutex> lock(channelsMutex);

    auto it = channels.find(channelIndex);
    return (it != channels.end()) ? it->second.get() : nullptr;
}

const DecoderResourceSharing::ChannelDecoderInfo* DecoderResourceSharing::getChannelInfo(int channelIndex) const {
    std::lock_guard<std::mutex> lock(channelsMutex);

    auto it = channels.find(channelIndex);
    return (it != channels.end()) ? it->second.get() : nullptr;
}

bool DecoderResourceSharing::validateChannelIndex(int channelIndex) const {
    return channelIndex >= 0 && channelIndex < 16; // Support up to 16 channels
}

std::string DecoderResourceSharing::decoderTypeToString(DecoderType type) const {
    switch (type) {
        case H264_DECODER: return "H264";
        case H265_DECODER: return "H265";
        case GENERIC_DECODER: return "Generic";
        default: return "Unknown";
    }
}

std::string DecoderResourceSharing::sharingStrategyToString(SharingStrategy strategy) const {
    switch (strategy) {
        case EXCLUSIVE: return "Exclusive";
        case SHARED_POOL: return "Shared Pool";
        case ADAPTIVE: return "Adaptive";
        case PRIORITY_BASED: return "Priority Based";
        case LOAD_BALANCED: return "Load Balanced";
        default: return "Unknown";
    }
}

void DecoderResourceSharing::resourceManagerLoop() {
    while (threadsRunning) {
        std::unique_lock<std::mutex> lock(threadMutex);
        resourceManagerCv.wait_for(lock, std::chrono::seconds(5), [this] { return !threadsRunning; });
        
        if (!threadsRunning) break;
        
        // Perform resource management tasks
        monitorResourceUtilization();
        reclaimIdleDecoders();
        
        if (config.enableDynamicAllocation) {
            adaptPoolSizes();
        }
    }
}

void DecoderResourceSharing::statisticsLoop() {
    while (threadsRunning) {
        std::unique_lock<std::mutex> lock(threadMutex);
        statisticsCv.wait_for(lock, std::chrono::seconds(2), [this] { return !threadsRunning; });
        
        if (!threadsRunning) break;
        
        updateStatistics();
    }
}

void DecoderResourceSharing::updateStatistics() {
    std::lock_guard<std::mutex> statsLock(statisticsMutex);
    
    statistics.totalDecoders = 0;
    statistics.activeDecoders = 0;
    statistics.idleDecoders = 0;
    statistics.decodersByType.clear();
    
    // Collect pool statistics
    std::lock_guard<std::mutex> poolLock(poolsMutex);
    for (const auto& pair : sharedPools) {
        const auto& pool = pair.second;
        statistics.totalDecoders += pool->totalDecoders;
        statistics.activeDecoders += pool->activeCount;
        statistics.idleDecoders += pool->availableCount;
        statistics.decodersByType[pair.first] = pool->totalDecoders;
    }
    
    // Calculate utilization
    if (statistics.totalDecoders > 0) {
        statistics.averageUtilization = 
            static_cast<float>(statistics.activeDecoders) / statistics.totalDecoders;
    }
    
    LOGD("Resource statistics updated: %d total, %d active, %.2f%% utilization",
         statistics.totalDecoders, statistics.activeDecoders,
         statistics.averageUtilization * 100.0f);
}

void DecoderResourceSharing::monitorResourceUtilization() {
    // Monitor and detect resource contentions
    std::lock_guard<std::mutex> poolLock(poolsMutex);

    for (const auto& pair : sharedPools) {
        const auto& pool = pair.second;
        float poolUtilization = (pool->totalDecoders > 0) ?
            static_cast<float>(pool->activeCount) / pool->totalDecoders : 0.0f;

        if (poolUtilization > config.resourceUtilizationThreshold) {
            LOGW("High utilization detected for %s pool: %.2f%%",
                 decoderTypeToString(pair.first).c_str(), poolUtilization * 100.0f);

            // Consider expanding pool
            if (config.enableDynamicAllocation &&
                pool->totalDecoders < config.maxSharedDecoders) {
                expandPool(pair.first, 2); // Add 2 more decoders
            }
        }
    }
}

void DecoderResourceSharing::reclaimIdleDecoders() {
    auto now = std::chrono::steady_clock::now();
    std::vector<int> idleChannels;

    // Find idle channels
    {
        std::lock_guard<std::mutex> channelLock(channelsMutex);
        for (const auto& pair : channels) {
            const auto& channelInfo = pair.second;
            auto idleTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - channelInfo->lastUsed);

            if (idleTime.count() > config.idleTimeoutMs &&
                channelInfo->activeDecoders > 0) {
                idleChannels.push_back(pair.first);
            }
        }
    }

    // Reclaim decoders from idle channels
    for (int channelIndex : idleChannels) {
        auto channelInfo = getChannelInfo(channelIndex);
        if (channelInfo && !channelInfo->exclusiveAccess) {
            std::lock_guard<std::mutex> channelLock(channelInfo->channelMutex);

            // Release excess decoders (keep minimum)
            while (channelInfo->assignedDecoders.size() > config.minDecodersPerChannel) {
                auto decoder = channelInfo->assignedDecoders.back();
                channelInfo->assignedDecoders.pop_back();
                releaseDecoder(channelIndex, decoder);

                LOGD("Reclaimed idle decoder from channel %d", channelIndex);
            }
        }
    }
}

void DecoderResourceSharing::adaptPoolSizes() {
    std::lock_guard<std::mutex> poolLock(poolsMutex);

    for (const auto& pair : sharedPools) {
        auto& pool = pair.second;
        float utilization = (pool->totalDecoders > 0) ?
            static_cast<float>(pool->activeCount) / pool->totalDecoders : 0.0f;

        if (utilization > 0.9f && pool->totalDecoders < config.maxSharedDecoders) {
            // High utilization - expand pool
            expandPool(pair.first, 1);
        } else if (utilization < 0.3f && pool->totalDecoders > 2) {
            // Low utilization - shrink pool
            shrinkPool(pair.first, pool->totalDecoders - 1);
        }
    }
}

bool DecoderResourceSharing::expandPool(DecoderType type, int additionalDecoders) {
    auto pool = getSharedPool(type);
    if (!pool) {
        return false;
    }

    std::lock_guard<std::mutex> poolLock(pool->poolMutex);

    int actualAdded = 0;
    for (int i = 0; i < additionalDecoders; i++) {
        if (pool->totalDecoders >= config.maxSharedDecoders) {
            break;
        }

        auto decoder = createDecoder(type);
        if (decoder) {
            pool->decoders.push_back(decoder);
            pool->availableDecoders.push(decoder);
            pool->totalDecoders++;
            pool->availableCount++;
            actualAdded++;
        }
    }

    if (actualAdded > 0) {
        notifyPoolExpanded(type, pool->totalDecoders);
        LOGD("Expanded %s pool by %d decoders (total: %d)",
             decoderTypeToString(type).c_str(), actualAdded, pool->totalDecoders.load());
    }

    return actualAdded > 0;
}

bool DecoderResourceSharing::shrinkPool(DecoderType type, int targetSize) {
    auto pool = getSharedPool(type);
    if (!pool) {
        return false;
    }

    std::lock_guard<std::mutex> poolLock(pool->poolMutex);

    int toRemove = pool->totalDecoders - targetSize;
    int actualRemoved = 0;

    // Only remove available (unused) decoders
    while (actualRemoved < toRemove && !pool->availableDecoders.empty()) {
        pool->availableDecoders.pop();
        pool->availableCount--;
        pool->totalDecoders--;
        actualRemoved++;
    }

    // Remove from main decoders vector
    if (actualRemoved > 0) {
        pool->decoders.resize(pool->totalDecoders);
        notifyPoolShrunk(type, pool->totalDecoders);
        LOGD("Shrunk %s pool by %d decoders (total: %d)",
             decoderTypeToString(type).c_str(), actualRemoved, pool->totalDecoders.load());
    }

    return actualRemoved > 0;
}

bool DecoderResourceSharing::preemptDecoder(int fromChannel, int toChannel) {
    auto fromChannelInfo = getChannelInfo(fromChannel);
    auto toChannelInfo = getChannelInfo(toChannel);

    if (!fromChannelInfo || !toChannelInfo) {
        return false;
    }

    // Check if preemption is allowed
    if (!config.enableResourcePreemption ||
        fromChannelInfo->priority >= toChannelInfo->priority ||
        fromChannelInfo->exclusiveAccess) {
        return false;
    }

    std::lock_guard<std::mutex> fromLock(fromChannelInfo->channelMutex);

    if (fromChannelInfo->assignedDecoders.empty()) {
        return false;
    }

    // Take the last assigned decoder
    auto decoder = fromChannelInfo->assignedDecoders.back();
    fromChannelInfo->assignedDecoders.pop_back();
    fromChannelInfo->activeDecoders--;

    // Return to pool for the new channel to acquire
    auto pool = getSharedPool(fromChannelInfo->decoderType);
    if (pool) {
        std::lock_guard<std::mutex> poolLock(pool->poolMutex);
        pool->availableDecoders.push(decoder);
        pool->availableCount++;
        pool->activeCount--;
        pool->activeAssignments.erase(fromChannel);
    }

    statistics.preemptions++;
    notifyResourcePreemption(fromChannel, toChannel, decoder);

    LOGD("Preempted decoder from channel %d to channel %d", fromChannel, toChannel);
    return true;
}

std::vector<int> DecoderResourceSharing::identifyLowPriorityChannels() const {
    std::vector<int> lowPriorityChannels;
    std::lock_guard<std::mutex> lock(channelsMutex);

    for (const auto& pair : channels) {
        if (pair.second->priority <= 1 && !pair.second->exclusiveAccess) {
            lowPriorityChannels.push_back(pair.first);
        }
    }

    return lowPriorityChannels;
}

std::vector<int> DecoderResourceSharing::identifyHighUtilizationChannels() const {
    std::vector<int> highUtilizationChannels;
    std::lock_guard<std::mutex> lock(channelsMutex);

    for (const auto& pair : channels) {
        if (pair.second->activeDecoders >= config.maxDecodersPerChannel * 0.8f) {
            highUtilizationChannels.push_back(pair.first);
        }
    }

    return highUtilizationChannels;
}

DecoderResourceSharing::ResourceStatistics DecoderResourceSharing::getResourceStatistics() const {
    std::lock_guard<std::mutex> lock(statisticsMutex);
    return statistics;
}

float DecoderResourceSharing::getChannelUtilization(int channelIndex) const {
    auto channelInfo = getChannelInfo(channelIndex);
    if (!channelInfo) {
        return 0.0f;
    }

    return static_cast<float>(channelInfo->activeDecoders) / config.maxDecodersPerChannel;
}

std::vector<int> DecoderResourceSharing::getActiveChannels() const {
    std::vector<int> activeChannels;
    std::lock_guard<std::mutex> lock(channelsMutex);

    for (const auto& pair : channels) {
        if (pair.second->activeDecoders > 0) {
            activeChannels.push_back(pair.first);
        }
    }

    return activeChannels;
}

int DecoderResourceSharing::getAvailableDecoders(DecoderType type) const {
    auto pool = const_cast<DecoderResourceSharing*>(this)->getSharedPool(type);
    return pool ? pool->availableCount.load() : 0;
}

void DecoderResourceSharing::setSharingStrategy(SharingStrategy strategy) {
    std::lock_guard<std::mutex> lock(configMutex);
    config.strategy = strategy;
    LOGD("Sharing strategy changed to: %s", sharingStrategyToString(strategy).c_str());
}

DecoderResourceSharing::SharingStrategy DecoderResourceSharing::getSharingStrategy() const {
    std::lock_guard<std::mutex> lock(configMutex);
    return config.strategy;
}

void DecoderResourceSharing::setResourceConfig(const DecoderResourceConfig& newConfig) {
    std::lock_guard<std::mutex> lock(configMutex);
    config = newConfig;
    LOGD("Resource configuration updated");
}

DecoderResourceSharing::DecoderResourceConfig DecoderResourceSharing::getResourceConfig() const {
    std::lock_guard<std::mutex> lock(configMutex);
    return config;
}

void DecoderResourceSharing::setEventListener(ResourceSharingEventListener* listener) {
    eventListener = listener;
}

std::string DecoderResourceSharing::generateResourceReport() const {
    auto stats = getResourceStatistics();
    std::ostringstream report;

    report << "=== Decoder Resource Sharing Report ===\n";
    report << "Strategy: " << sharingStrategyToString(config.strategy) << "\n";
    report << "Total Decoders: " << stats.totalDecoders << "\n";
    report << "Active Decoders: " << stats.activeDecoders << "\n";
    report << "Idle Decoders: " << stats.idleDecoders << "\n";
    report << "Average Utilization: " << std::fixed << std::setprecision(2)
           << stats.averageUtilization * 100.0f << "%\n";
    report << "Peak Utilization: " << stats.peakUtilization * 100.0f << "%\n";
    report << "Resource Contentions: " << stats.resourceContentions << "\n";
    report << "Preemptions: " << stats.preemptions << "\n\n";

    report << "Decoders by Type:\n";
    for (const auto& pair : stats.decodersByType) {
        report << "  " << decoderTypeToString(pair.first) << ": " << pair.second << "\n";
    }

    return report.str();
}

// Event notification methods
void DecoderResourceSharing::notifyDecoderAssigned(int channelIndex, std::shared_ptr<MppDecoder> decoder) {
    if (eventListener) {
        eventListener->onDecoderAssigned(channelIndex, decoder);
    }
}

void DecoderResourceSharing::notifyDecoderReleased(int channelIndex, std::shared_ptr<MppDecoder> decoder) {
    if (eventListener) {
        eventListener->onDecoderReleased(channelIndex, decoder);
    }
}

void DecoderResourceSharing::notifyResourceContention(int channelIndex, DecoderType type) {
    statistics.resourceContentions++;
    if (eventListener) {
        eventListener->onResourceContention(channelIndex, type);
    }
}

void DecoderResourceSharing::notifyResourcePreemption(int fromChannel, int toChannel, std::shared_ptr<MppDecoder> decoder) {
    if (eventListener) {
        eventListener->onResourcePreemption(fromChannel, toChannel, decoder);
    }
}

void DecoderResourceSharing::notifyPoolExpanded(DecoderType type, int newSize) {
    if (eventListener) {
        eventListener->onPoolExpanded(type, newSize);
    }
}

void DecoderResourceSharing::notifyPoolShrunk(DecoderType type, int newSize) {
    if (eventListener) {
        eventListener->onPoolShrunk(type, newSize);
    }
}

// DecoderPerformanceOptimizer implementation
DecoderPerformanceOptimizer::DecoderPerformanceOptimizer(DecoderResourceSharing* sharing)
    : resourceSharing(sharing) {
    LOGD("DecoderPerformanceOptimizer created");
}

DecoderPerformanceOptimizer::~DecoderPerformanceOptimizer() {
    stopOptimization();
    LOGD("DecoderPerformanceOptimizer destroyed");
}

void DecoderPerformanceOptimizer::startOptimization() {
    // Start optimization thread
    LOGD("Decoder performance optimization started");
}

void DecoderPerformanceOptimizer::stopOptimization() {
    // Stop optimization thread
    LOGD("Decoder performance optimization stopped");
}

void DecoderPerformanceOptimizer::updateChannelMetrics(int channelIndex, const OptimizationMetrics& metrics) {
    std::lock_guard<std::mutex> lock(metricsMutex);
    channelMetrics[channelIndex] = metrics;

    LOGD("Updated performance metrics for channel %d: latency=%.2fms, throughput=%.2f",
         channelIndex, metrics.decodeLatency, metrics.throughput);
}

DecoderPerformanceOptimizer::OptimizationMetrics
DecoderPerformanceOptimizer::getChannelMetrics(int channelIndex) const {
    std::lock_guard<std::mutex> lock(metricsMutex);

    auto it = channelMetrics.find(channelIndex);
    if (it != channelMetrics.end()) {
        return it->second;
    }

    return OptimizationMetrics();
}

void DecoderPerformanceOptimizer::optimizeChannelPerformance(int channelIndex) {
    if (!resourceSharing) return;

    auto metrics = getChannelMetrics(channelIndex);

    // Optimize based on performance metrics
    if (metrics.decodeLatency > 100.0f) { // High latency
        LOGW("High decode latency detected for channel %d: %.2fms", channelIndex, metrics.decodeLatency);

        // Try to allocate more decoders
        auto decoder = resourceSharing->acquireDecoder(channelIndex);
        if (decoder) {
            LOGD("Allocated additional decoder for channel %d to reduce latency", channelIndex);
        }
    }

    if (metrics.resourceEfficiency < 0.5f) { // Low efficiency
        LOGW("Low resource efficiency for channel %d: %.2f", channelIndex, metrics.resourceEfficiency);

        // Consider reducing decoder allocation
        // This would require additional logic to release decoders
    }

    if (metrics.queueDepth > 10) { // High queue depth
        LOGW("High queue depth for channel %d: %d", channelIndex, metrics.queueDepth);

        // Try to allocate more processing resources
        auto decoder = resourceSharing->acquireDecoder(channelIndex);
        if (decoder) {
            LOGD("Allocated additional decoder for channel %d to reduce queue depth", channelIndex);
        }
    }
}

void DecoderPerformanceOptimizer::optimizeSystemPerformance() {
    if (!resourceSharing) return;

    auto stats = resourceSharing->getResourceStatistics();

    // System-wide optimizations
    if (stats.averageUtilization > 0.9f) {
        LOGW("High system utilization detected: %.2f%%", stats.averageUtilization * 100.0f);

        // Trigger load balancing
        resourceSharing->balanceLoad();
    }

    if (stats.resourceContentions > 10) {
        LOGW("High resource contention detected: %d contentions", stats.resourceContentions);

        // Expand pools to reduce contention
        resourceSharing->expandPool(DecoderResourceSharing::H264_DECODER, 2);
        resourceSharing->expandPool(DecoderResourceSharing::H265_DECODER, 2);
    }

    // Optimize individual channels
    std::lock_guard<std::mutex> lock(metricsMutex);
    for (const auto& pair : channelMetrics) {
        optimizeChannelPerformance(pair.first);
    }
}

std::vector<std::string> DecoderPerformanceOptimizer::generateOptimizationRecommendations() const {
    std::vector<std::string> recommendations;

    if (!resourceSharing) {
        recommendations.push_back("Resource sharing system not available");
        return recommendations;
    }

    auto stats = resourceSharing->getResourceStatistics();

    if (stats.averageUtilization > 0.8f) {
        recommendations.push_back("High system utilization. Consider adding more decoder resources.");
    }

    if (stats.resourceContentions > 5) {
        recommendations.push_back("Frequent resource contentions. Consider expanding decoder pools.");
    }

    if (stats.preemptions > 10) {
        recommendations.push_back("High preemption rate. Review channel priorities and resource allocation.");
    }

    // Channel-specific recommendations
    std::lock_guard<std::mutex> lock(metricsMutex);
    for (const auto& pair : channelMetrics) {
        const auto& metrics = pair.second;
        int channelIndex = pair.first;

        if (metrics.decodeLatency > 100.0f) {
            recommendations.push_back("Channel " + std::to_string(channelIndex) +
                                    ": High decode latency. Consider allocating more decoders.");
        }

        if (metrics.resourceEfficiency < 0.5f) {
            recommendations.push_back("Channel " + std::to_string(channelIndex) +
                                    ": Low resource efficiency. Review decoder allocation strategy.");
        }

        if (metrics.queueDepth > 10) {
            recommendations.push_back("Channel " + std::to_string(channelIndex) +
                                    ": High queue depth. Increase processing capacity.");
        }
    }

    return recommendations;
}

void DecoderPerformanceOptimizer::optimizationLoop() {
    // This would be the main optimization loop
    // Called periodically to analyze and optimize performance
    analyzePerformancePatterns();
    adjustResourceAllocation();
}

void DecoderPerformanceOptimizer::analyzePerformancePatterns() {
    // Analyze performance patterns across channels
    std::lock_guard<std::mutex> lock(metricsMutex);

    float totalLatency = 0.0f;
    float totalThroughput = 0.0f;
    int channelCount = 0;

    for (const auto& pair : channelMetrics) {
        const auto& metrics = pair.second;
        totalLatency += metrics.decodeLatency;
        totalThroughput += metrics.throughput;
        channelCount++;
    }

    if (channelCount > 0) {
        float avgLatency = totalLatency / channelCount;
        float avgThroughput = totalThroughput / channelCount;

        LOGD("Performance analysis: avg latency=%.2fms, avg throughput=%.2f",
             avgLatency, avgThroughput);

        // Identify performance bottlenecks
        for (const auto& pair : channelMetrics) {
            const auto& metrics = pair.second;
            if (metrics.decodeLatency > avgLatency * 1.5f) {
                LOGW("Channel %d has high latency: %.2fms (avg: %.2fms)",
                     pair.first, metrics.decodeLatency, avgLatency);
            }
        }
    }
}

void DecoderPerformanceOptimizer::adjustResourceAllocation() {
    // Adjust resource allocation based on performance analysis
    if (!resourceSharing) return;

    auto activeChannels = resourceSharing->getActiveChannels();

    for (int channelIndex : activeChannels) {
        auto metrics = getChannelMetrics(channelIndex);
        float utilization = resourceSharing->getChannelUtilization(channelIndex);

        // Adjust allocation based on utilization and performance
        if (utilization > 0.9f && metrics.decodeLatency > 50.0f) {
            // High utilization and latency - try to get more resources
            resourceSharing->acquireDecoder(channelIndex);
        } else if (utilization < 0.3f && metrics.resourceEfficiency > 0.8f) {
            // Low utilization but good efficiency - might be able to share resources
            // This would require additional logic to release decoders
        }
    }
}

void DecoderResourceSharing::balanceLoad() {
    std::lock_guard<std::mutex> lock(channelsMutex);

    // Collect channel utilization data
    std::vector<std::pair<int, float>> channelUtilizations;
    for (const auto& pair : channels) {
        int channelIndex = pair.first;
        float utilization = getChannelUtilization(channelIndex);
        channelUtilizations.push_back({channelIndex, utilization});
    }

    // Sort by utilization (highest first)
    std::sort(channelUtilizations.begin(), channelUtilizations.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    // Balance load by moving resources from low-utilization to high-utilization channels
    for (size_t i = 0; i < channelUtilizations.size() / 2; ++i) {
        int highUtilChannel = channelUtilizations[i].first;
        int lowUtilChannel = channelUtilizations[channelUtilizations.size() - 1 - i].first;

        float highUtil = channelUtilizations[i].second;
        float lowUtil = channelUtilizations[channelUtilizations.size() - 1 - i].second;

        // Only balance if there's significant difference
        if (highUtil > 0.8f && lowUtil < 0.3f) {
            preemptDecoder(lowUtilChannel, highUtilChannel);
        }
    }

    LOGD("Load balancing completed");
}
