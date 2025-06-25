#ifndef AIBOX_RTSP_STREAM_MANAGER_H
#define AIBOX_RTSP_STREAM_MANAGER_H

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <map>
#include <queue>
#include <functional>

#include <mk_common.h>
#include <mk_player.h>
#include "log4c.h"
#include "user_comm.h"
#include "ChannelManager.h"

// Forward declarations

/**
 * RTSP Stream Manager for handling multiple concurrent RTSP connections
 * Provides independent stream processing with health monitoring and reconnection
 */
class RTSPStreamManager {
public:
    enum StreamState {
        DISCONNECTED = 0,
        CONNECTING = 1,
        CONNECTED = 2,
        STREAMING = 3,
        ERROR = 4,
        RECONNECTING = 5
    };

    struct StreamInfo {
        int channelIndex;
        std::string rtspUrl;
        StreamState state;
        mk_player player;
        std::chrono::steady_clock::time_point lastFrameTime;
        std::chrono::steady_clock::time_point connectionTime;
        int reconnectAttempts;
        int frameCount;
        float fps;
        std::string lastError;
        bool autoReconnect;
        
        StreamInfo(int index, const std::string& url) 
            : channelIndex(index), rtspUrl(url), state(DISCONNECTED), 
              player(nullptr), reconnectAttempts(0), frameCount(0), 
              fps(0.0f), autoReconnect(true) {
            lastFrameTime = std::chrono::steady_clock::now();
            connectionTime = std::chrono::steady_clock::now();
        }
    };

    // Callback interface for stream events
    class StreamEventListener {
    public:
        virtual ~StreamEventListener() = default;
        virtual void onStreamConnected(int channelIndex) = 0;
        virtual void onStreamDisconnected(int channelIndex) = 0;
        virtual void onStreamError(int channelIndex, const std::string& error) = 0;
        virtual void onFrameReceived(int channelIndex, void* frameData, int size) = 0;
        virtual void onStreamStateChanged(int channelIndex, StreamState oldState, StreamState newState) = 0;
    };

private:
    std::map<int, std::unique_ptr<StreamInfo>> streams;
    std::mutex streamsMutex;
    
    // Health monitoring
    std::thread healthMonitorThread;
    std::atomic<bool> shouldStop;
    std::condition_variable healthMonitorCv;
    std::mutex healthMonitorMutex;
    
    // Reconnection management
    std::thread reconnectThread;
    std::queue<int> reconnectQueue;
    std::mutex reconnectMutex;
    std::condition_variable reconnectCv;
    
    // Event listener
    StreamEventListener* eventListener;
    
    // Configuration
    static constexpr int MAX_RECONNECT_ATTEMPTS = 5;
    static constexpr int RECONNECT_DELAY_MS = 5000;
    static constexpr int HEALTH_CHECK_INTERVAL_MS = 1000;
    static constexpr int FRAME_TIMEOUT_MS = 10000;

public:
    RTSPStreamManager();
    ~RTSPStreamManager();
    
    // Stream management
    bool addStream(int channelIndex, const std::string& rtspUrl);
    bool removeStream(int channelIndex);
    bool startStream(int channelIndex);
    bool stopStream(int channelIndex);
    
    // Configuration
    void setEventListener(StreamEventListener* listener);
    void setAutoReconnect(int channelIndex, bool enabled);
    void setReconnectDelay(int delayMs);
    
    // Status queries
    StreamState getStreamState(int channelIndex) const;
    float getStreamFps(int channelIndex) const;
    int getStreamFrameCount(int channelIndex) const;
    std::string getStreamError(int channelIndex) const;
    bool isStreamHealthy(int channelIndex) const;
    
    // Statistics
    int getActiveStreamCount() const;
    int getTotalStreamCount() const;
    std::vector<int> getActiveChannels() const;
    
    // Control
    void pauseStream(int channelIndex);
    void resumeStream(int channelIndex);
    void forceReconnect(int channelIndex);
    
    // Cleanup
    void stopAllStreams();
    void cleanup();

    // Public stream connection methods
    bool connectStreamByIndex(int channelIndex);
    void disconnectStreamByIndex(int channelIndex);

private:
    // Internal stream management
    bool connectStream(StreamInfo* streamInfo);
    void disconnectStream(StreamInfo* streamInfo);
    void updateStreamState(int channelIndex, StreamState newState);
    
    // Health monitoring
    void healthMonitorLoop();
    void checkStreamHealth(StreamInfo* streamInfo);
    bool isStreamTimedOut(const StreamInfo* streamInfo) const;
    
    // Reconnection management
    void reconnectLoop();
    void scheduleReconnect(int channelIndex);
    bool attemptReconnect(StreamInfo* streamInfo);
    
    // MediaKit callbacks
    static void onPlayEvent(void* userData, int errCode, const char* errMsg,
                           mk_track tracks[], int trackCount);
    static void onShutdown(void* userData, int errCode, const char* errMsg,
                          mk_track tracks[], int trackCount);
    static void onTrackFrame(void* userData, mk_frame frame);
    
    // Utility methods
    StreamInfo* getStreamInfo(int channelIndex);
    const StreamInfo* getStreamInfo(int channelIndex) const;
    void notifyStateChange(int channelIndex, StreamState oldState, StreamState newState);
    void updateStreamStats(StreamInfo* streamInfo);
    
    // Thread safety helpers
    std::unique_lock<std::mutex> lockStreams() { return std::unique_lock<std::mutex>(streamsMutex); }
};

/**
 * Enhanced Multi-Channel ZLPlayer with advanced RTSP stream management
 */
class EnhancedMultiChannelZLPlayer : public MultiChannelZLPlayer, public RTSPStreamManager::StreamEventListener {
private:
    std::unique_ptr<RTSPStreamManager> rtspManager;
    std::atomic<bool> streamingActive;
    
public:
    EnhancedMultiChannelZLPlayer(int channelIndex, char* modelFileData, int modelDataLen, 
                                NativeChannelManager* manager);
    ~EnhancedMultiChannelZLPlayer();
    
    // RTSP methods
    bool startRTSPStream();
    void stopRTSPStream();
    bool isChannelActive() const;
    
    // Enhanced stream control
    void pauseStream();
    void resumeStream();
    void forceReconnect();
    
    // Stream status
    RTSPStreamManager::StreamState getStreamState() const;
    float getStreamFps() const;
    bool isStreamHealthy() const;
    
    // RTSPStreamManager::StreamEventListener implementation
    void onStreamConnected(int channelIndex) override;
    void onStreamDisconnected(int channelIndex) override;
    void onStreamError(int channelIndex, const std::string& error) override;
    void onFrameReceived(int channelIndex, void* frameData, int size) override;
    void onStreamStateChanged(int channelIndex, RTSPStreamManager::StreamState oldState, 
                             RTSPStreamManager::StreamState newState) override;

private:
    void handleStreamFrame(void* frameData, int size);
    void notifyChannelManagerOfStateChange(RTSPStreamManager::StreamState state);
};

#endif // AIBOX_RTSP_STREAM_MANAGER_H
