#ifndef AIBOX_DECODER_MANAGER_H
#define AIBOX_DECODER_MANAGER_H

#include <memory>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <queue>
#include <condition_variable>
#include <functional>

#include "mpp_decoder.h"
#include "user_comm.h"
#include "log4c.h"
#include "ZLPlayer.h"

/**
 * Decoder Manager for handling multiple independent decoder instances
 * Provides isolated decoding for each channel with resource management
 */
class DecoderManager {
public:
    enum DecoderState {
        IDLE = 0,
        INITIALIZING = 1,
        READY = 2,
        DECODING = 3,
        ERROR = 4,
        DESTROYED = 5
    };

    struct DecoderInfo {
        int channelIndex;
        std::unique_ptr<MppDecoder> decoder;
        DecoderState state;
        rknn_app_context_t* context;
        std::atomic<int> frameCount;
        std::atomic<int> errorCount;
        std::chrono::steady_clock::time_point lastFrameTime;
        std::chrono::steady_clock::time_point creationTime;
        std::string lastError;
        
        // Decoder configuration
        int codecType;
        int fps;
        int width;
        int height;
        
        DecoderInfo(int index, rknn_app_context_t* ctx) 
            : channelIndex(index), state(IDLE), context(ctx),
              frameCount(0), errorCount(0), codecType(264), fps(25),
              width(0), height(0) {
            lastFrameTime = std::chrono::steady_clock::now();
            creationTime = std::chrono::steady_clock::now();
        }
    };

    // Callback interface for decoder events
    class DecoderEventListener {
    public:
        virtual ~DecoderEventListener() = default;
        virtual void onDecoderReady(int channelIndex) = 0;
        virtual void onFrameDecoded(int channelIndex, void* frameData, int width, int height) = 0;
        virtual void onDecoderError(int channelIndex, const std::string& error) = 0;
        virtual void onDecoderDestroyed(int channelIndex) = 0;
    };

private:
    std::map<int, std::unique_ptr<DecoderInfo>> decoders;
    std::mutex decodersMutex;
    
    // Resource management
    std::atomic<int> activeDecoderCount;
    std::atomic<int> maxDecoders;
    std::atomic<long> totalMemoryUsage;
    
    // Health monitoring
    std::thread healthMonitorThread;
    std::atomic<bool> shouldStop;
    std::condition_variable healthMonitorCv;
    std::mutex healthMonitorMutex;
    
    // Event listener
    DecoderEventListener* eventListener;
    
    // Configuration
    static constexpr int DEFAULT_MAX_DECODERS = 16;
    static constexpr int HEALTH_CHECK_INTERVAL_MS = 2000;
    static constexpr int DECODER_TIMEOUT_MS = 30000;

public:
    DecoderManager(int maxDecoders = DEFAULT_MAX_DECODERS);
    ~DecoderManager();
    
    // Decoder lifecycle management
    bool createDecoder(int channelIndex, rknn_app_context_t* context, 
                      int codecType = 264, int fps = 25);
    bool destroyDecoder(int channelIndex);
    bool resetDecoder(int channelIndex);
    
    // Decoder operations
    bool initializeDecoder(int channelIndex);
    bool isDecoderReady(int channelIndex) const;
    DecoderState getDecoderState(int channelIndex) const;
    
    // Frame processing
    bool decodeFrame(int channelIndex, uint8_t* data, int size, int64_t timestamp);
    int getFrameCount(int channelIndex) const;
    int getErrorCount(int channelIndex) const;
    
    // Configuration
    void setEventListener(DecoderEventListener* listener);
    void setMaxDecoders(int maxDecoders);
    void setDecoderCallback(int channelIndex, MppDecoderFrameCallback callback);
    
    // Statistics and monitoring
    int getActiveDecoderCount() const { return activeDecoderCount.load(); }
    long getTotalMemoryUsage() const { return totalMemoryUsage.load(); }
    std::vector<int> getActiveChannels() const;
    
    // Resource management
    bool hasCapacityForNewDecoder() const;
    void optimizeMemoryUsage();
    void cleanupIdleDecoders();
    
    // Cleanup
    void cleanup();

private:
    // Internal management
    DecoderInfo* getDecoderInfo(int channelIndex);
    const DecoderInfo* getDecoderInfo(int channelIndex) const;
    void updateDecoderState(int channelIndex, DecoderState newState);
    
    // Health monitoring
    void healthMonitorLoop();
    void checkDecoderHealth(DecoderInfo* decoderInfo);
    bool isDecoderTimedOut(const DecoderInfo* decoderInfo) const;
    
    // Resource monitoring
    void updateMemoryUsage();
    long estimateDecoderMemoryUsage(const DecoderInfo* decoderInfo) const;
    
    // Error handling
    void handleDecoderError(int channelIndex, const std::string& error);
    
    // Thread safety helpers
    std::unique_lock<std::mutex> lockDecoders() { return std::unique_lock<std::mutex>(decodersMutex); }
};

/**
 * Decoder Pool for efficient decoder reuse
 */
class DecoderPool {
public:
    struct PooledDecoder {
        std::unique_ptr<MppDecoder> decoder;
        bool inUse;
        std::chrono::steady_clock::time_point lastUsed;
        int usageCount;
        
        PooledDecoder() : inUse(false), usageCount(0) {
            lastUsed = std::chrono::steady_clock::now();
        }
    };

private:
    std::vector<std::unique_ptr<PooledDecoder>> decoderPool;
    std::mutex poolMutex;
    int poolSize;
    int maxPoolSize;

public:
    DecoderPool(int maxSize = 8);
    ~DecoderPool();
    
    // Pool management
    MppDecoder* acquireDecoder();
    void releaseDecoder(MppDecoder* decoder);
    void expandPool(int additionalDecoders);
    void shrinkPool(int targetSize);
    
    // Statistics
    int getPoolSize() const { return poolSize; }
    int getAvailableDecoders() const;
    int getUsedDecoders() const;
    
    // Cleanup
    void cleanup();

private:
    PooledDecoder* findAvailableDecoder();
    PooledDecoder* findDecoderByInstance(MppDecoder* decoder);
    void createNewDecoder();
    void removeOldestUnusedDecoder();
};

/**
 * Enhanced Multi-Channel Decoder with advanced features
 */
class EnhancedMultiChannelDecoder {
private:
    std::unique_ptr<DecoderManager> decoderManager;
    std::unique_ptr<DecoderPool> decoderPool;
    std::map<int, std::function<void(int, void*, int, int)>> frameCallbacks;
    std::mutex callbacksMutex;
    
public:
    EnhancedMultiChannelDecoder(int maxChannels = 16);
    ~EnhancedMultiChannelDecoder();
    
    // Channel management
    bool addChannel(int channelIndex, rknn_app_context_t* context);
    bool removeChannel(int channelIndex);
    
    // Decoding operations
    bool decodeFrame(int channelIndex, uint8_t* data, int size, int64_t timestamp = 0);
    bool isChannelReady(int channelIndex) const;
    
    // Callback management
    void setFrameCallback(int channelIndex, std::function<void(int, void*, int, int)> callback);
    void removeFrameCallback(int channelIndex);
    
    // Statistics
    int getActiveChannelCount() const;
    std::vector<int> getActiveChannels() const;
    
    // Resource optimization
    void optimizeResources();
    void enablePooling(bool enabled);
    
    // Cleanup
    void cleanup();

private:
    void handleFrameDecoded(int channelIndex, void* frameData, int width, int height);
};

#endif // AIBOX_DECODER_MANAGER_H
