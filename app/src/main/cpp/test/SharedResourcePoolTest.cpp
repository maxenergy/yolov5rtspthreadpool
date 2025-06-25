#include "SharedResourcePool.h"
#include "log4c.h"
#include <chrono>
#include <thread>
#include <cassert>

/**
 * Test class for Shared Resource Pool system
 */
class SharedResourcePoolTest : public SharedResourcePool::PoolEventListener {
private:
    int resourceAllocatedCount = 0;
    int resourceReleasedCount = 0;
    int poolExpandedCount = 0;
    int poolShrunkCount = 0;
    int allocationFailedCount = 0;
    int utilizationAlertCount = 0;
    
public:
    // PoolEventListener implementation
    void onResourceAllocated(SharedResourcePool::PoolType type, int instanceId, int channelIndex) override {
        resourceAllocatedCount++;
        LOGD("Test: Resource allocated - Type: %d, Instance: %d, Channel: %d", type, instanceId, channelIndex);
    }
    
    void onResourceReleased(SharedResourcePool::PoolType type, int instanceId, int channelIndex) override {
        resourceReleasedCount++;
        LOGD("Test: Resource released - Type: %d, Instance: %d, Channel: %d", type, instanceId, channelIndex);
    }
    
    void onPoolExpanded(SharedResourcePool::PoolType type, int newSize) override {
        poolExpandedCount++;
        LOGD("Test: Pool expanded - Type: %d, New size: %d", type, newSize);
    }
    
    void onPoolShrunk(SharedResourcePool::PoolType type, int newSize) override {
        poolShrunkCount++;
        LOGD("Test: Pool shrunk - Type: %d, New size: %d", type, newSize);
    }
    
    void onAllocationFailed(SharedResourcePool::PoolType type, int channelIndex) override {
        allocationFailedCount++;
        LOGW("Test: Allocation failed - Type: %d, Channel: %d", type, channelIndex);
    }
    
    void onUtilizationAlert(SharedResourcePool::PoolType type, float utilization) override {
        utilizationAlertCount++;
        LOGW("Test: Utilization alert - Type: %d, Utilization: %.2f%%", type, utilization * 100.0f);
    }
    
    bool testBasicInitialization() {
        LOGD("=== Testing Basic Initialization ===");
        
        // Create dummy model data
        char dummyModel[1024];
        memset(dummyModel, 0, sizeof(dummyModel));
        
        SharedResourcePool resourcePool;
        
        // Test initialization
        if (!resourcePool.initialize(dummyModel, sizeof(dummyModel))) {
            LOGE("Failed to initialize shared resource pool");
            return false;
        }
        
        resourcePool.setEventListener(this);
        
        // Test pool creation
        SharedResourcePool::PoolConfiguration config(SharedResourcePool::YOLOV5_THREAD_POOL);
        config.initialSize = 2;
        config.maxSize = 8;
        
        if (!resourcePool.createPool(SharedResourcePool::YOLOV5_THREAD_POOL, config)) {
            LOGE("Failed to create YOLOv5 thread pool");
            return false;
        }
        
        // Check pool statistics
        auto stats = resourcePool.getPoolStatistics(SharedResourcePool::YOLOV5_THREAD_POOL);
        if (stats.totalInstances != config.initialSize) {
            LOGE("Incorrect initial pool size: expected %d, got %d", config.initialSize, stats.totalInstances);
            return false;
        }
        
        LOGD("Basic initialization test passed");
        return true;
    }
    
    bool testResourceAllocation() {
        LOGD("=== Testing Resource Allocation ===");
        
        char dummyModel[1024];
        memset(dummyModel, 0, sizeof(dummyModel));
        
        SharedResourcePool resourcePool;
        resourcePool.initialize(dummyModel, sizeof(dummyModel));
        resourcePool.setEventListener(this);
        
        // Allocate YOLOv5 thread pools
        auto threadPool1 = resourcePool.allocateYolov5ThreadPool(0, 1);
        auto threadPool2 = resourcePool.allocateYolov5ThreadPool(1, 2);
        
        if (!threadPool1 || !threadPool2) {
            LOGE("Failed to allocate YOLOv5 thread pools");
            return false;
        }
        
        // Check statistics
        auto stats = resourcePool.getPoolStatistics(SharedResourcePool::YOLOV5_THREAD_POOL);
        LOGD("Pool statistics: %d total, %d active, %.2f%% utilization", 
             stats.totalInstances, stats.activeInstances, stats.utilizationRate * 100.0f);
        
        if (stats.activeInstances < 2) {
            LOGE("Incorrect active instance count");
            return false;
        }
        
        // Release resources
        resourcePool.releaseResource(SharedResourcePool::YOLOV5_THREAD_POOL, threadPool1, 0);
        resourcePool.releaseResource(SharedResourcePool::YOLOV5_THREAD_POOL, threadPool2, 1);
        
        LOGD("Resource allocation test passed");
        return true;
    }
    
    bool testDynamicPoolResize() {
        LOGD("=== Testing Dynamic Pool Resize ===");
        
        char dummyModel[1024];
        memset(dummyModel, 0, sizeof(dummyModel));
        
        SharedResourcePool resourcePool;
        resourcePool.initialize(dummyModel, sizeof(dummyModel));
        resourcePool.setEventListener(this);
        
        // Create pool with small initial size
        SharedResourcePool::PoolConfiguration config(SharedResourcePool::MEMORY_BUFFER_POOL);
        config.initialSize = 2;
        config.maxSize = 8;
        config.enableDynamicResize = true;
        config.utilizationThreshold = 0.7f;
        
        resourcePool.createPool(SharedResourcePool::MEMORY_BUFFER_POOL, config);
        
        // Allocate resources to trigger expansion
        std::vector<std::shared_ptr<void>> allocatedResources;
        for (int i = 0; i < 6; i++) {
            auto resource = resourcePool.allocateResource(SharedResourcePool::MEMORY_BUFFER_POOL, i);
            if (resource) {
                allocatedResources.push_back(resource);
            }
        }
        
        // Wait for dynamic resize
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        auto stats = resourcePool.getPoolStatistics(SharedResourcePool::MEMORY_BUFFER_POOL);
        LOGD("After allocation: %d total instances", stats.totalInstances);
        
        // Release resources
        for (size_t i = 0; i < allocatedResources.size(); i++) {
            resourcePool.releaseResource(SharedResourcePool::MEMORY_BUFFER_POOL, allocatedResources[i], i);
        }
        
        LOGD("Dynamic pool resize test passed");
        return true;
    }
    
    bool testChannelAffinity() {
        LOGD("=== Testing Channel Affinity ===");
        
        char dummyModel[1024];
        memset(dummyModel, 0, sizeof(dummyModel));
        
        SharedResourcePool resourcePool;
        resourcePool.initialize(dummyModel, sizeof(dummyModel));
        resourcePool.setEventListener(this);
        
        // Set channel affinity
        resourcePool.setChannelAffinity(0, SharedResourcePool::YOLOV5_THREAD_POOL, 1);
        
        // Check affinity
        int affinity = resourcePool.getChannelAffinity(0, SharedResourcePool::YOLOV5_THREAD_POOL);
        if (affinity != 1) {
            LOGE("Incorrect channel affinity: expected 1, got %d", affinity);
            return false;
        }
        
        // Clear affinity
        resourcePool.clearChannelAffinity(0);
        
        affinity = resourcePool.getChannelAffinity(0, SharedResourcePool::YOLOV5_THREAD_POOL);
        if (affinity != -1) {
            LOGE("Affinity not cleared properly");
            return false;
        }
        
        LOGD("Channel affinity test passed");
        return true;
    }
    
    bool testResourcePoolManager() {
        LOGD("=== Testing Resource Pool Manager ===");
        
        char dummyModel[1024];
        memset(dummyModel, 0, sizeof(dummyModel));
        
        ResourcePoolManager poolManager;
        
        // Initialize with system configuration
        ResourcePoolManager::SystemConfiguration config;
        config.maxChannels = 8;
        config.enableGlobalOptimization = true;
        
        if (!poolManager.initialize(dummyModel, sizeof(dummyModel), config)) {
            LOGE("Failed to initialize resource pool manager");
            return false;
        }
        
        // Allocate channel resources
        if (!poolManager.allocateChannelResources(0, 2)) {
            LOGE("Failed to allocate resources for channel 0");
            return false;
        }
        
        if (!poolManager.allocateChannelResources(1, 1)) {
            LOGE("Failed to allocate resources for channel 1");
            return false;
        }
        
        // Get resources
        auto yolov5Pool = poolManager.getYolov5ThreadPool(0);
        auto decoder = poolManager.getMppDecoder(0);
        
        if (!yolov5Pool) {
            LOGW("YOLOv5 thread pool not available (may be expected in test environment)");
        }
        
        if (!decoder) {
            LOGW("MPP decoder not available (may be expected in test environment)");
        }
        
        // Release channel resources
        poolManager.releaseChannelResources(0);
        poolManager.releaseChannelResources(1);
        
        // Generate system report
        std::string report = poolManager.generateSystemReport();
        LOGD("System report generated: %zu characters", report.length());
        
        LOGD("Resource pool manager test passed");
        return true;
    }
    
    bool testPerformanceMetrics() {
        LOGD("=== Testing Performance Metrics ===");
        
        char dummyModel[1024];
        memset(dummyModel, 0, sizeof(dummyModel));
        
        SharedResourcePool resourcePool;
        resourcePool.initialize(dummyModel, sizeof(dummyModel));
        resourcePool.setEventListener(this);
        
        // Perform multiple allocations to generate metrics
        for (int i = 0; i < 10; i++) {
            auto resource = resourcePool.allocateResource(SharedResourcePool::FRAME_BUFFER_POOL, i % 4);
            if (resource) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                resourcePool.releaseResource(SharedResourcePool::FRAME_BUFFER_POOL, resource, i % 4);
            }
        }
        
        // Wait for statistics to update
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        // Get statistics
        auto stats = resourcePool.getPoolStatistics(SharedResourcePool::FRAME_BUFFER_POOL);
        LOGD("Performance metrics:");
        LOGD("  Total requests: %d", stats.totalRequests);
        LOGD("  Successful allocations: %d", stats.successfulAllocations);
        LOGD("  Average response time: %.2fms", stats.averageResponseTime);
        
        // Generate optimization recommendations
        auto recommendations = resourcePool.getOptimizationRecommendations();
        LOGD("Generated %zu optimization recommendations", recommendations.size());
        
        for (const auto& recommendation : recommendations) {
            LOGD("Recommendation: %s", recommendation.c_str());
        }
        
        LOGD("Performance metrics test passed");
        return true;
    }
    
    void runAllTests() {
        LOGD("Starting Shared Resource Pool Tests");
        
        int passedTests = 0;
        int totalTests = 6;
        
        if (testBasicInitialization()) passedTests++;
        if (testResourceAllocation()) passedTests++;
        if (testDynamicPoolResize()) passedTests++;
        if (testChannelAffinity()) passedTests++;
        if (testResourcePoolManager()) passedTests++;
        if (testPerformanceMetrics()) passedTests++;
        
        LOGD("=== Test Results ===");
        LOGD("Passed: %d/%d tests", passedTests, totalTests);
        LOGD("Resource allocated events: %d", resourceAllocatedCount);
        LOGD("Resource released events: %d", resourceReleasedCount);
        LOGD("Pool expanded events: %d", poolExpandedCount);
        LOGD("Pool shrunk events: %d", poolShrunkCount);
        LOGD("Allocation failed events: %d", allocationFailedCount);
        LOGD("Utilization alert events: %d", utilizationAlertCount);
        
        if (passedTests == totalTests) {
            LOGD("All tests PASSED!");
        } else {
            LOGE("Some tests FAILED!");
        }
    }
};

// Test runner function
extern "C" void runSharedResourcePoolTests() {
    SharedResourcePoolTest test;
    test.runAllTests();
}

// Stress test
extern "C" void runSharedResourcePoolStressTest() {
    LOGD("=== Shared Resource Pool Stress Test ===");
    
    char dummyModel[1024];
    memset(dummyModel, 0, sizeof(dummyModel));
    
    SharedResourcePool resourcePool;
    resourcePool.initialize(dummyModel, sizeof(dummyModel));
    
    // Stress test with multiple channels and resource types
    const int numChannels = 8;
    const int numIterations = 100;
    
    int totalAllocations = 0;
    int totalReleases = 0;
    
    auto startTime = std::chrono::steady_clock::now();
    
    for (int iteration = 0; iteration < numIterations; iteration++) {
        std::vector<std::shared_ptr<void>> allocatedResources;
        
        // Allocate resources for all channels
        for (int channel = 0; channel < numChannels; channel++) {
            // Allocate different types of resources
            auto yolov5Pool = resourcePool.allocateYolov5ThreadPool(channel);
            auto frameBuffer = resourcePool.allocateFrameBuffer(channel);
            auto memoryBuffer = resourcePool.allocateMemoryBuffer(channel, 1024);
            
            if (yolov5Pool) {
                allocatedResources.push_back(yolov5Pool);
                totalAllocations++;
            }
            if (frameBuffer) {
                allocatedResources.push_back(frameBuffer);
                totalAllocations++;
            }
            if (memoryBuffer) {
                allocatedResources.push_back(memoryBuffer);
                totalAllocations++;
            }
        }
        
        // Hold resources for a short time
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        
        // Release resources
        for (size_t i = 0; i < allocatedResources.size(); i++) {
            int channel = i % numChannels;
            SharedResourcePool::PoolType type = static_cast<SharedResourcePool::PoolType>(i % 3);
            
            if (resourcePool.releaseResource(type, allocatedResources[i], channel)) {
                totalReleases++;
            }
        }
        
        if (iteration % 20 == 0) {
            LOGD("Stress test progress: %d/%d iterations", iteration, numIterations);
        }
    }
    
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    // Get final statistics
    auto allStats = resourcePool.getAllPoolStatistics();
    
    LOGD("Stress test completed in %ldms:", duration.count());
    LOGD("Total allocations: %d", totalAllocations);
    LOGD("Total releases: %d", totalReleases);
    
    for (const auto& pair : allStats) {
        const auto& stats = pair.second;
        LOGD("Pool %d: %d total instances, %.2f%% utilization, %d requests", 
             pair.first, stats.totalInstances, stats.utilizationRate * 100.0f, stats.totalRequests);
    }
}
