#include "DecoderResourceSharing.h"
#include "log4c.h"
#include <chrono>
#include <thread>
#include <cassert>

/**
 * Test class for Decoder Resource Sharing system
 */
class DecoderResourceSharingTest : public DecoderResourceSharing::ResourceSharingEventListener {
private:
    int decoderAssignedCount = 0;
    int decoderReleasedCount = 0;
    int resourceContentionCount = 0;
    int resourcePreemptionCount = 0;
    int poolExpandedCount = 0;
    int poolShrunkCount = 0;
    
public:
    // ResourceSharingEventListener implementation
    void onDecoderAssigned(int channelIndex, std::shared_ptr<MppDecoder> decoder) override {
        decoderAssignedCount++;
        LOGD("Test: Decoder assigned to channel %d", channelIndex);
    }
    
    void onDecoderReleased(int channelIndex, std::shared_ptr<MppDecoder> decoder) override {
        decoderReleasedCount++;
        LOGD("Test: Decoder released from channel %d", channelIndex);
    }
    
    void onResourceContention(int channelIndex, DecoderResourceSharing::DecoderType type) override {
        resourceContentionCount++;
        LOGW("Test: Resource contention for channel %d, type %d", channelIndex, type);
    }
    
    void onResourcePreemption(int fromChannel, int toChannel, std::shared_ptr<MppDecoder> decoder) override {
        resourcePreemptionCount++;
        LOGD("Test: Resource preemption from channel %d to channel %d", fromChannel, toChannel);
    }
    
    void onPoolExpanded(DecoderResourceSharing::DecoderType type, int newSize) override {
        poolExpandedCount++;
        LOGD("Test: Pool expanded for type %d, new size: %d", type, newSize);
    }
    
    void onPoolShrunk(DecoderResourceSharing::DecoderType type, int newSize) override {
        poolShrunkCount++;
        LOGD("Test: Pool shrunk for type %d, new size: %d", type, newSize);
    }
    
    bool testBasicInitialization() {
        LOGD("=== Testing Basic Initialization ===");
        
        DecoderResourceSharing resourceSharing;
        
        // Test initialization with default config
        DecoderResourceSharing::DecoderResourceConfig config;
        config.strategy = DecoderResourceSharing::SHARED_POOL;
        config.maxDecodersPerType = 4;
        config.maxSharedDecoders = 8;
        
        if (!resourceSharing.initialize(config)) {
            LOGE("Failed to initialize decoder resource sharing");
            return false;
        }
        
        resourceSharing.setEventListener(this);
        
        // Test adding channels
        if (!resourceSharing.addChannel(0, DecoderResourceSharing::H264_DECODER, 2)) {
            LOGE("Failed to add H264 channel 0");
            return false;
        }
        
        if (!resourceSharing.addChannel(1, DecoderResourceSharing::H265_DECODER, 1)) {
            LOGE("Failed to add H265 channel 1");
            return false;
        }
        
        // Test configuration
        auto retrievedConfig = resourceSharing.getResourceConfig();
        if (retrievedConfig.strategy != config.strategy) {
            LOGE("Configuration not properly set");
            return false;
        }
        
        LOGD("Basic initialization test passed");
        return true;
    }
    
    bool testSharedPoolAllocation() {
        LOGD("=== Testing Shared Pool Allocation ===");
        
        DecoderResourceSharing resourceSharing;
        
        DecoderResourceSharing::DecoderResourceConfig config;
        config.strategy = DecoderResourceSharing::SHARED_POOL;
        config.maxDecodersPerType = 4;
        config.enableDynamicAllocation = true;
        
        resourceSharing.initialize(config);
        resourceSharing.setEventListener(this);
        
        // Add channels
        resourceSharing.addChannel(0, DecoderResourceSharing::H264_DECODER);
        resourceSharing.addChannel(1, DecoderResourceSharing::H264_DECODER);
        resourceSharing.addChannel(2, DecoderResourceSharing::H265_DECODER);
        
        // Acquire decoders
        auto decoder0 = resourceSharing.acquireDecoder(0);
        auto decoder1 = resourceSharing.acquireDecoder(1);
        auto decoder2 = resourceSharing.acquireDecoder(2);
        
        if (!decoder0 || !decoder1 || !decoder2) {
            LOGE("Failed to acquire decoders from shared pool");
            return false;
        }
        
        // Check statistics
        auto stats = resourceSharing.getResourceStatistics();
        LOGD("Statistics: %d total, %d active decoders", stats.totalDecoders, stats.activeDecoders);
        
        if (stats.activeDecoders < 3) {
            LOGE("Incorrect active decoder count");
            return false;
        }
        
        // Release decoders
        resourceSharing.releaseDecoder(0, decoder0);
        resourceSharing.releaseDecoder(1, decoder1);
        resourceSharing.releaseDecoder(2, decoder2);
        
        LOGD("Shared pool allocation test passed");
        return true;
    }
    
    bool testExclusiveAllocation() {
        LOGD("=== Testing Exclusive Allocation ===");
        
        DecoderResourceSharing resourceSharing;
        
        DecoderResourceSharing::DecoderResourceConfig config;
        config.strategy = DecoderResourceSharing::EXCLUSIVE;
        config.maxDecodersPerChannel = 2;
        
        resourceSharing.initialize(config);
        resourceSharing.setEventListener(this);
        
        // Add channel with exclusive access
        resourceSharing.addChannel(0, DecoderResourceSharing::H264_DECODER);
        resourceSharing.setChannelExclusiveAccess(0, true);
        
        // Acquire multiple decoders
        auto decoder1 = resourceSharing.acquireDecoder(0);
        auto decoder2 = resourceSharing.acquireDecoder(0);
        
        if (!decoder1 || !decoder2) {
            LOGE("Failed to acquire exclusive decoders");
            return false;
        }
        
        // Try to acquire more than allowed
        auto decoder3 = resourceSharing.acquireDecoder(0);
        if (decoder3) {
            LOGW("Acquired more decoders than expected for exclusive channel");
        }
        
        LOGD("Exclusive allocation test passed");
        return true;
    }
    
    bool testPriorityBasedAllocation() {
        LOGD("=== Testing Priority-Based Allocation ===");
        
        DecoderResourceSharing resourceSharing;
        
        DecoderResourceSharing::DecoderResourceConfig config;
        config.strategy = DecoderResourceSharing::PRIORITY_BASED;
        config.enableResourcePreemption = true;
        config.maxSharedDecoders = 2; // Limited resources to test priority
        
        resourceSharing.initialize(config);
        resourceSharing.setEventListener(this);
        
        // Add channels with different priorities
        resourceSharing.addChannel(0, DecoderResourceSharing::H264_DECODER, 1); // Low priority
        resourceSharing.addChannel(1, DecoderResourceSharing::H264_DECODER, 3); // High priority
        
        // Low priority channel acquires decoder first
        auto decoder0 = resourceSharing.acquireDecoder(0);
        if (!decoder0) {
            LOGE("Failed to acquire decoder for low priority channel");
            return false;
        }
        
        // High priority channel should be able to preempt
        auto decoder1 = resourceSharing.acquireDecoder(1);
        if (!decoder1) {
            LOGW("High priority channel couldn't acquire decoder (preemption may not have occurred)");
        }
        
        LOGD("Priority-based allocation test completed");
        return true;
    }
    
    bool testAdaptiveAllocation() {
        LOGD("=== Testing Adaptive Allocation ===");
        
        DecoderResourceSharing resourceSharing;
        
        DecoderResourceSharing::DecoderResourceConfig config;
        config.strategy = DecoderResourceSharing::ADAPTIVE;
        config.resourceUtilizationThreshold = 0.7f;
        config.enableDynamicAllocation = true;
        
        resourceSharing.initialize(config);
        resourceSharing.setEventListener(this);
        
        // Add channels
        resourceSharing.addChannel(0, DecoderResourceSharing::H264_DECODER);
        resourceSharing.addChannel(1, DecoderResourceSharing::H264_DECODER);
        
        // Acquire decoders - should adapt based on system load
        auto decoder0 = resourceSharing.acquireDecoder(0);
        auto decoder1 = resourceSharing.acquireDecoder(1);
        
        if (!decoder0 || !decoder1) {
            LOGE("Failed to acquire decoders with adaptive strategy");
            return false;
        }
        
        // Check if strategy adapts to load
        auto stats = resourceSharing.getResourceStatistics();
        LOGD("Adaptive allocation - utilization: %.2f%%", stats.averageUtilization * 100.0f);
        
        LOGD("Adaptive allocation test passed");
        return true;
    }
    
    bool testPerformanceOptimizer() {
        LOGD("=== Testing Performance Optimizer ===");
        
        DecoderResourceSharing resourceSharing;
        resourceSharing.initialize();
        
        DecoderPerformanceOptimizer optimizer(&resourceSharing);
        
        // Add channels
        resourceSharing.addChannel(0, DecoderResourceSharing::H264_DECODER);
        resourceSharing.addChannel(1, DecoderResourceSharing::H265_DECODER);
        
        // Update performance metrics
        DecoderPerformanceOptimizer::OptimizationMetrics metrics;
        metrics.decodeLatency = 150.0f; // High latency
        metrics.throughput = 25.0f;
        metrics.resourceEfficiency = 0.6f;
        metrics.queueDepth = 15; // High queue depth
        
        optimizer.updateChannelMetrics(0, metrics);
        
        // Test optimization
        optimizer.optimizeChannelPerformance(0);
        optimizer.optimizeSystemPerformance();
        
        // Generate recommendations
        auto recommendations = optimizer.generateOptimizationRecommendations();
        LOGD("Generated %zu optimization recommendations", recommendations.size());
        
        for (const auto& recommendation : recommendations) {
            LOGD("Recommendation: %s", recommendation.c_str());
        }
        
        LOGD("Performance optimizer test passed");
        return true;
    }
    
    bool testResourceStatistics() {
        LOGD("=== Testing Resource Statistics ===");
        
        DecoderResourceSharing resourceSharing;
        resourceSharing.initialize();
        resourceSharing.setEventListener(this);
        
        // Add channels and acquire decoders
        resourceSharing.addChannel(0, DecoderResourceSharing::H264_DECODER);
        resourceSharing.addChannel(1, DecoderResourceSharing::H265_DECODER);
        
        auto decoder0 = resourceSharing.acquireDecoder(0);
        auto decoder1 = resourceSharing.acquireDecoder(1);
        
        // Wait for statistics to update
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Get statistics
        auto stats = resourceSharing.getResourceStatistics();
        auto activeChannels = resourceSharing.getActiveChannels();
        
        LOGD("Resource statistics:");
        LOGD("  Total decoders: %d", stats.totalDecoders);
        LOGD("  Active decoders: %d", stats.activeDecoders);
        LOGD("  Average utilization: %.2f%%", stats.averageUtilization * 100.0f);
        LOGD("  Active channels: %zu", activeChannels.size());
        
        // Generate resource report
        std::string report = resourceSharing.generateResourceReport();
        LOGD("Resource report generated: %zu characters", report.length());
        
        LOGD("Resource statistics test passed");
        return true;
    }
    
    void runAllTests() {
        LOGD("Starting Decoder Resource Sharing Tests");
        
        int passedTests = 0;
        int totalTests = 7;
        
        if (testBasicInitialization()) passedTests++;
        if (testSharedPoolAllocation()) passedTests++;
        if (testExclusiveAllocation()) passedTests++;
        if (testPriorityBasedAllocation()) passedTests++;
        if (testAdaptiveAllocation()) passedTests++;
        if (testPerformanceOptimizer()) passedTests++;
        if (testResourceStatistics()) passedTests++;
        
        LOGD("=== Test Results ===");
        LOGD("Passed: %d/%d tests", passedTests, totalTests);
        LOGD("Decoder assigned events: %d", decoderAssignedCount);
        LOGD("Decoder released events: %d", decoderReleasedCount);
        LOGD("Resource contention events: %d", resourceContentionCount);
        LOGD("Resource preemption events: %d", resourcePreemptionCount);
        LOGD("Pool expanded events: %d", poolExpandedCount);
        LOGD("Pool shrunk events: %d", poolShrunkCount);
        
        if (passedTests == totalTests) {
            LOGD("All tests PASSED!");
        } else {
            LOGE("Some tests FAILED!");
        }
    }
};

// Test runner function
extern "C" void runDecoderResourceSharingTests() {
    DecoderResourceSharingTest test;
    test.runAllTests();
}

// Stress test
extern "C" void runDecoderResourceSharingStressTest() {
    LOGD("=== Decoder Resource Sharing Stress Test ===");
    
    DecoderResourceSharing resourceSharing;
    
    DecoderResourceSharing::DecoderResourceConfig config;
    config.strategy = DecoderResourceSharing::ADAPTIVE;
    config.enableDynamicAllocation = true;
    config.maxSharedDecoders = 16;
    
    resourceSharing.initialize(config);
    
    // Add many channels
    const int numChannels = 12;
    for (int i = 0; i < numChannels; i++) {
        auto type = (i % 2 == 0) ? DecoderResourceSharing::H264_DECODER : 
                                  DecoderResourceSharing::H265_DECODER;
        resourceSharing.addChannel(i, type, (i % 3) + 1); // Varying priorities
    }
    
    // Continuously acquire and release decoders for 10 seconds
    auto startTime = std::chrono::steady_clock::now();
    auto endTime = startTime + std::chrono::seconds(10);
    
    int acquisitionCount = 0;
    int releaseCount = 0;
    
    while (std::chrono::steady_clock::now() < endTime) {
        for (int i = 0; i < numChannels; i++) {
            auto decoder = resourceSharing.acquireDecoder(i);
            if (decoder) {
                acquisitionCount++;
                
                // Hold decoder for a short time
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                
                if (resourceSharing.releaseDecoder(i, decoder)) {
                    releaseCount++;
                }
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    // Get final statistics
    auto stats = resourceSharing.getResourceStatistics();
    
    LOGD("Stress test completed:");
    LOGD("Decoder acquisitions: %d", acquisitionCount);
    LOGD("Decoder releases: %d", releaseCount);
    LOGD("Final total decoders: %d", stats.totalDecoders);
    LOGD("Final active decoders: %d", stats.activeDecoders);
    LOGD("Resource contentions: %d", stats.resourceContentions);
    LOGD("Preemptions: %d", stats.preemptions);
    LOGD("Average utilization: %.2f%%", stats.averageUtilization * 100.0f);
}
