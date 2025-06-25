#include "RTSPStreamManager.h"
#include "ChannelManager.h"
#include <algorithm>

// Static constant definitions
constexpr int RTSPStreamManager::MAX_RECONNECT_ATTEMPTS;
constexpr int RTSPStreamManager::RECONNECT_DELAY_MS;
constexpr int RTSPStreamManager::HEALTH_CHECK_INTERVAL_MS;
constexpr int RTSPStreamManager::FRAME_TIMEOUT_MS;

RTSPStreamManager::RTSPStreamManager() 
    : eventListener(nullptr), shouldStop(false) {
    
    // Initialize MediaKit environment
    mk_config config;
    memset(&config, 0, sizeof(mk_config));
    config.log_mask = LOG_CONSOLE;
    mk_env_init(&config);
    
    // Start health monitoring thread
    healthMonitorThread = std::thread(&RTSPStreamManager::healthMonitorLoop, this);
    
    // Start reconnection thread
    reconnectThread = std::thread(&RTSPStreamManager::reconnectLoop, this);
    
    LOGD("RTSPStreamManager initialized");
}

RTSPStreamManager::~RTSPStreamManager() {
    cleanup();
}

bool RTSPStreamManager::addStream(int channelIndex, const std::string& rtspUrl) {
    if (rtspUrl.empty()) {
        LOGE("Cannot add stream with empty URL for channel %d", channelIndex);
        return false;
    }
    
    auto lock = lockStreams();
    
    // Remove existing stream if present
    auto it = streams.find(channelIndex);
    if (it != streams.end()) {
        LOGW("Replacing existing stream for channel %d", channelIndex);
        disconnectStream(it->second.get());
        streams.erase(it);
    }
    
    // Create new stream info
    auto streamInfo = std::make_unique<StreamInfo>(channelIndex, rtspUrl);
    streams[channelIndex] = std::move(streamInfo);
    
    LOGD("Added stream for channel %d: %s", channelIndex, rtspUrl.c_str());
    return true;
}

bool RTSPStreamManager::removeStream(int channelIndex) {
    auto lock = lockStreams();
    
    auto it = streams.find(channelIndex);
    if (it == streams.end()) {
        return false;
    }
    
    // Disconnect and remove stream
    disconnectStream(it->second.get());
    streams.erase(it);
    
    LOGD("Removed stream for channel %d", channelIndex);
    return true;
}

bool RTSPStreamManager::startStream(int channelIndex) {
    auto lock = lockStreams();
    
    StreamInfo* streamInfo = getStreamInfo(channelIndex);
    if (!streamInfo) {
        LOGE("Stream not found for channel %d", channelIndex);
        return false;
    }
    
    if (streamInfo->state == STREAMING || streamInfo->state == CONNECTING) {
        LOGW("Stream already active for channel %d", channelIndex);
        return true;
    }
    
    return connectStream(streamInfo);
}

bool RTSPStreamManager::stopStream(int channelIndex) {
    auto lock = lockStreams();
    
    StreamInfo* streamInfo = getStreamInfo(channelIndex);
    if (!streamInfo) {
        return false;
    }
    
    disconnectStream(streamInfo);
    return true;
}

bool RTSPStreamManager::connectStream(StreamInfo* streamInfo) {
    if (!streamInfo) return false;
    
    LOGD("Connecting stream for channel %d: %s", 
         streamInfo->channelIndex, streamInfo->rtspUrl.c_str());
    
    updateStreamState(streamInfo->channelIndex, CONNECTING);
    
    // Create MediaKit player
    streamInfo->player = mk_player_create();
    if (!streamInfo->player) {
        LOGE("Failed to create player for channel %d", streamInfo->channelIndex);
        updateStreamState(streamInfo->channelIndex, ERROR);
        return false;
    }
    
    // Set callbacks
    mk_player_set_on_result(streamInfo->player, onPlayEvent, streamInfo);
    mk_player_set_on_shutdown(streamInfo->player, onShutdown, streamInfo);
    
    // Start playback
    mk_player_play(streamInfo->player, streamInfo->rtspUrl.c_str());
    
    streamInfo->connectionTime = std::chrono::steady_clock::now();
    streamInfo->reconnectAttempts = 0;
    
    return true;
}

void RTSPStreamManager::disconnectStream(StreamInfo* streamInfo) {
    if (!streamInfo) return;
    
    LOGD("Disconnecting stream for channel %d", streamInfo->channelIndex);
    
    if (streamInfo->player) {
        mk_player_release(streamInfo->player);
        streamInfo->player = nullptr;
    }
    
    updateStreamState(streamInfo->channelIndex, DISCONNECTED);
}

void RTSPStreamManager::updateStreamState(int channelIndex, StreamState newState) {
    StreamInfo* streamInfo = getStreamInfo(channelIndex);
    if (!streamInfo) return;
    
    StreamState oldState = streamInfo->state;
    if (oldState != newState) {
        streamInfo->state = newState;
        notifyStateChange(channelIndex, oldState, newState);
    }
}

void RTSPStreamManager::healthMonitorLoop() {
    while (!shouldStop) {
        std::unique_lock<std::mutex> lock(healthMonitorMutex);
        healthMonitorCv.wait_for(lock, std::chrono::milliseconds(HEALTH_CHECK_INTERVAL_MS));
        
        if (shouldStop) break;
        
        // Check health of all streams
        auto streamsLock = lockStreams();
        for (auto& pair : streams) {
            checkStreamHealth(pair.second.get());
        }
    }
}

void RTSPStreamManager::checkStreamHealth(StreamInfo* streamInfo) {
    if (!streamInfo || streamInfo->state != STREAMING) return;
    
    // Check for frame timeout
    if (isStreamTimedOut(streamInfo)) {
        LOGW("Stream timeout detected for channel %d", streamInfo->channelIndex);
        streamInfo->lastError = "Frame timeout";
        
        if (streamInfo->autoReconnect) {
            scheduleReconnect(streamInfo->channelIndex);
        } else {
            updateStreamState(streamInfo->channelIndex, ERROR);
        }
    }
    
    // Update FPS statistics
    updateStreamStats(streamInfo);
}

bool RTSPStreamManager::isStreamTimedOut(const StreamInfo* streamInfo) const {
    auto now = std::chrono::steady_clock::now();
    auto timeSinceLastFrame = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - streamInfo->lastFrameTime);
    
    return timeSinceLastFrame.count() > FRAME_TIMEOUT_MS;
}

void RTSPStreamManager::scheduleReconnect(int channelIndex) {
    std::lock_guard<std::mutex> lock(reconnectMutex);
    reconnectQueue.push(channelIndex);
    reconnectCv.notify_one();
    
    LOGD("Scheduled reconnect for channel %d", channelIndex);
}

void RTSPStreamManager::reconnectLoop() {
    while (!shouldStop) {
        std::unique_lock<std::mutex> lock(reconnectMutex);
        reconnectCv.wait(lock, [this] { return !reconnectQueue.empty() || shouldStop; });
        
        if (shouldStop) break;
        
        if (!reconnectQueue.empty()) {
            int channelIndex = reconnectQueue.front();
            reconnectQueue.pop();
            lock.unlock();
            
            // Wait before attempting reconnect
            std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_DELAY_MS));
            
            auto streamsLock = lockStreams();
            StreamInfo* streamInfo = getStreamInfo(channelIndex);
            if (streamInfo && streamInfo->autoReconnect) {
                attemptReconnect(streamInfo);
            }
        }
    }
}

bool RTSPStreamManager::attemptReconnect(StreamInfo* streamInfo) {
    if (!streamInfo) return false;
    
    if (streamInfo->reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
        LOGE("Max reconnect attempts reached for channel %d", streamInfo->channelIndex);
        streamInfo->lastError = "Max reconnect attempts exceeded";
        updateStreamState(streamInfo->channelIndex, ERROR);
        return false;
    }
    
    streamInfo->reconnectAttempts++;
    updateStreamState(streamInfo->channelIndex, RECONNECTING);
    
    LOGD("Attempting reconnect %d/%d for channel %d", 
         streamInfo->reconnectAttempts, MAX_RECONNECT_ATTEMPTS, streamInfo->channelIndex);
    
    // Disconnect current connection
    if (streamInfo->player) {
        mk_player_release(streamInfo->player);
        streamInfo->player = nullptr;
    }
    
    // Attempt new connection
    return connectStream(streamInfo);
}

// MediaKit callback implementations
void RTSPStreamManager::onPlayEvent(void* userData, int errCode, const char* errMsg, 
                                   mk_track tracks[], int trackCount) {
    StreamInfo* streamInfo = static_cast<StreamInfo*>(userData);
    if (!streamInfo) return;
    
    if (errCode == 0) {
        // Success
        LOGD("Stream connected successfully for channel %d", streamInfo->channelIndex);
        
        // Set up track callbacks
        for (int i = 0; i < trackCount; ++i) {
            if (mk_track_is_video(tracks[i])) {
                LOGD("Got video track for channel %d: %s", 
                     streamInfo->channelIndex, mk_track_codec_name(tracks[i]));
                mk_track_add_delegate(tracks[i], onTrackFrame, userData);
            }
        }
        
        streamInfo->lastFrameTime = std::chrono::steady_clock::now();
        streamInfo->reconnectAttempts = 0;
        
        // Update state through the manager instance
        // Note: We need to access the manager instance here
        // This is a simplified approach - in practice, we'd store a reference
        streamInfo->state = STREAMING;
        
    } else {
        LOGE("Stream connection failed for channel %d: %d %s", 
             streamInfo->channelIndex, errCode, errMsg ? errMsg : "Unknown error");
        
        streamInfo->lastError = errMsg ? errMsg : "Connection failed";
        streamInfo->state = ERROR;
    }
}

void RTSPStreamManager::onShutdown(void* userData, int errCode, const char* errMsg,
                                  mk_track tracks[], int trackCount) {
    StreamInfo* streamInfo = static_cast<StreamInfo*>(userData);
    if (!streamInfo) return;

    LOGD("Stream shutdown for channel %d: %d %s",
         streamInfo->channelIndex, errCode, errMsg ? errMsg : "");

    streamInfo->state = DISCONNECTED;
}

void RTSPStreamManager::onTrackFrame(void* userData, mk_frame frame) {
    StreamInfo* streamInfo = static_cast<StreamInfo*>(userData);
    if (!streamInfo) return;
    
    // Update frame statistics
    streamInfo->lastFrameTime = std::chrono::steady_clock::now();
    streamInfo->frameCount++;
    
    // Get frame data
    const char* frameData = mk_frame_get_data(frame);
    size_t frameSize = mk_frame_get_data_size(frame);
    
    // Forward to event listener (this would be the channel manager or player)
    // Note: This is a simplified approach - actual implementation would need
    // proper callback mechanism to the RTSPStreamManager instance
}

void RTSPStreamManager::cleanup() {
    LOGD("Cleaning up RTSPStreamManager");
    
    // Stop all threads
    shouldStop = true;
    healthMonitorCv.notify_all();
    reconnectCv.notify_all();
    
    if (healthMonitorThread.joinable()) {
        healthMonitorThread.join();
    }
    
    if (reconnectThread.joinable()) {
        reconnectThread.join();
    }
    
    // Disconnect all streams
    auto lock = lockStreams();
    for (auto& pair : streams) {
        disconnectStream(pair.second.get());
    }
    streams.clear();
    
    LOGD("RTSPStreamManager cleanup complete");
}

// Utility methods
RTSPStreamManager::StreamInfo* RTSPStreamManager::getStreamInfo(int channelIndex) {
    auto it = streams.find(channelIndex);
    return (it != streams.end()) ? it->second.get() : nullptr;
}

const RTSPStreamManager::StreamInfo* RTSPStreamManager::getStreamInfo(int channelIndex) const {
    auto it = streams.find(channelIndex);
    return (it != streams.end()) ? it->second.get() : nullptr;
}

void RTSPStreamManager::updateStreamStats(StreamInfo* streamInfo) {
    if (!streamInfo) return;

    auto now = std::chrono::steady_clock::now();
    static auto lastStatsUpdate = now;
    static std::map<int, int> lastFrameCounts;

    auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastStatsUpdate);

    if (timeSinceLastUpdate.count() >= 1000) { // Update every second
        int framesSinceLastUpdate = streamInfo->frameCount - lastFrameCounts[streamInfo->channelIndex];
        streamInfo->fps = framesSinceLastUpdate * 1000.0f / timeSinceLastUpdate.count();

        lastFrameCounts[streamInfo->channelIndex] = streamInfo->frameCount;
        lastStatsUpdate = now;
    }
}

// Public interface implementations
void RTSPStreamManager::setEventListener(StreamEventListener* listener) {
    eventListener = listener;
}

void RTSPStreamManager::setAutoReconnect(int channelIndex, bool enabled) {
    auto lock = lockStreams();
    StreamInfo* streamInfo = getStreamInfo(channelIndex);
    if (streamInfo) {
        streamInfo->autoReconnect = enabled;
        LOGD("Auto-reconnect %s for channel %d", enabled ? "enabled" : "disabled", channelIndex);
    }
}

RTSPStreamManager::StreamState RTSPStreamManager::getStreamState(int channelIndex) const {
    auto lock = const_cast<RTSPStreamManager*>(this)->lockStreams();
    const StreamInfo* streamInfo = getStreamInfo(channelIndex);
    return streamInfo ? streamInfo->state : DISCONNECTED;
}

float RTSPStreamManager::getStreamFps(int channelIndex) const {
    auto lock = const_cast<RTSPStreamManager*>(this)->lockStreams();
    const StreamInfo* streamInfo = getStreamInfo(channelIndex);
    return streamInfo ? streamInfo->fps : 0.0f;
}

int RTSPStreamManager::getStreamFrameCount(int channelIndex) const {
    auto lock = const_cast<RTSPStreamManager*>(this)->lockStreams();
    const StreamInfo* streamInfo = getStreamInfo(channelIndex);
    return streamInfo ? streamInfo->frameCount : 0;
}

std::string RTSPStreamManager::getStreamError(int channelIndex) const {
    auto lock = const_cast<RTSPStreamManager*>(this)->lockStreams();
    const StreamInfo* streamInfo = getStreamInfo(channelIndex);
    return streamInfo ? streamInfo->lastError : "";
}

bool RTSPStreamManager::isStreamHealthy(int channelIndex) const {
    auto lock = const_cast<RTSPStreamManager*>(this)->lockStreams();
    const StreamInfo* streamInfo = getStreamInfo(channelIndex);
    if (!streamInfo) return false;

    return streamInfo->state == STREAMING && !isStreamTimedOut(streamInfo);
}

int RTSPStreamManager::getActiveStreamCount() const {
    auto lock = const_cast<RTSPStreamManager*>(this)->lockStreams();
    int count = 0;
    for (const auto& pair : streams) {
        if (pair.second->state == STREAMING) {
            count++;
        }
    }
    return count;
}

int RTSPStreamManager::getTotalStreamCount() const {
    auto lock = const_cast<RTSPStreamManager*>(this)->lockStreams();
    return streams.size();
}

std::vector<int> RTSPStreamManager::getActiveChannels() const {
    auto lock = const_cast<RTSPStreamManager*>(this)->lockStreams();
    std::vector<int> activeChannels;
    for (const auto& pair : streams) {
        if (pair.second->state == STREAMING) {
            activeChannels.push_back(pair.first);
        }
    }
    return activeChannels;
}

void RTSPStreamManager::pauseStream(int channelIndex) {
    // Implementation would pause the stream without disconnecting
    LOGD("Pausing stream for channel %d", channelIndex);
    // This could be implemented by stopping frame processing while keeping connection
}

void RTSPStreamManager::resumeStream(int channelIndex) {
    // Implementation would resume the paused stream
    LOGD("Resuming stream for channel %d", channelIndex);
    // This could be implemented by resuming frame processing
}

void RTSPStreamManager::forceReconnect(int channelIndex) {
    auto lock = lockStreams();
    StreamInfo* streamInfo = getStreamInfo(channelIndex);
    if (streamInfo) {
        LOGD("Forcing reconnect for channel %d", channelIndex);
        streamInfo->reconnectAttempts = 0; // Reset attempt counter
        disconnectStream(streamInfo);
        scheduleReconnect(channelIndex);
    }
}

void RTSPStreamManager::stopAllStreams() {
    auto lock = lockStreams();
    for (auto& pair : streams) {
        disconnectStream(pair.second.get());
    }
    LOGD("All streams stopped");
}

void RTSPStreamManager::notifyStateChange(int channelIndex, StreamState oldState, StreamState newState) {
    if (eventListener) {
        eventListener->onStreamStateChanged(channelIndex, oldState, newState);
    }

    // Additional notifications based on state
    switch (newState) {
        case CONNECTED:
        case STREAMING:
            if (eventListener) {
                eventListener->onStreamConnected(channelIndex);
            }
            break;
        case DISCONNECTED:
            if (eventListener) {
                eventListener->onStreamDisconnected(channelIndex);
            }
            break;
        case ERROR:
            if (eventListener) {
                const StreamInfo* streamInfo = getStreamInfo(channelIndex);
                std::string error = streamInfo ? streamInfo->lastError : "Unknown error";
                eventListener->onStreamError(channelIndex, error);
            }
            break;
        default:
            break;
    }
}

// EnhancedMultiChannelZLPlayer implementation
EnhancedMultiChannelZLPlayer::EnhancedMultiChannelZLPlayer(int channelIndex, char* modelFileData,
                                                          int modelDataLen, NativeChannelManager* manager)
    : MultiChannelZLPlayer(channelIndex, modelFileData, modelDataLen, manager),
      streamingActive(false) {

    rtspManager = std::make_unique<RTSPStreamManager>();
    rtspManager->setEventListener(this);

    LOGD("EnhancedMultiChannelZLPlayer created for channel %d", channelIndex);
}

EnhancedMultiChannelZLPlayer::~EnhancedMultiChannelZLPlayer() {
    stopRTSPStream();
    rtspManager.reset();
    LOGD("EnhancedMultiChannelZLPlayer destroyed for channel %d", channelIndex);
}

bool EnhancedMultiChannelZLPlayer::startRTSPStream() {
    if (channelRtspUrl.empty()) {
        LOGE("Channel %d: RTSP URL not set", channelIndex);
        return false;
    }

    LOGD("Starting enhanced RTSP stream for channel %d: %s", channelIndex, channelRtspUrl.c_str());

    // Add stream to manager
    if (!rtspManager->addStream(channelIndex, channelRtspUrl)) {
        LOGE("Failed to add stream to manager for channel %d", channelIndex);
        return false;
    }

    // Start the stream
    if (rtspManager->startStream(channelIndex)) {
        streamingActive = true;
        return true;
    }

    return false;
}

void EnhancedMultiChannelZLPlayer::stopRTSPStream() {
    if (streamingActive) {
        LOGD("Stopping enhanced RTSP stream for channel %d", channelIndex);
        rtspManager->stopStream(channelIndex);
        streamingActive = false;
    }
}

bool EnhancedMultiChannelZLPlayer::isChannelActive() const {
    return streamingActive && rtspManager->isStreamHealthy(channelIndex);
}

void EnhancedMultiChannelZLPlayer::pauseStream() {
    rtspManager->pauseStream(channelIndex);
}

void EnhancedMultiChannelZLPlayer::resumeStream() {
    rtspManager->resumeStream(channelIndex);
}

void EnhancedMultiChannelZLPlayer::forceReconnect() {
    rtspManager->forceReconnect(channelIndex);
}

RTSPStreamManager::StreamState EnhancedMultiChannelZLPlayer::getStreamState() const {
    return rtspManager->getStreamState(channelIndex);
}

float EnhancedMultiChannelZLPlayer::getStreamFps() const {
    return rtspManager->getStreamFps(channelIndex);
}

bool EnhancedMultiChannelZLPlayer::isStreamHealthy() const {
    return rtspManager->isStreamHealthy(channelIndex);
}

// RTSPStreamManager::StreamEventListener implementation
void EnhancedMultiChannelZLPlayer::onStreamConnected(int channelIndex) {
    if (channelIndex == this->channelIndex) {
        LOGD("Stream connected for channel %d", channelIndex);
        notifyChannelManagerOfStateChange(RTSPStreamManager::STREAMING);
    }
}

void EnhancedMultiChannelZLPlayer::onStreamDisconnected(int channelIndex) {
    if (channelIndex == this->channelIndex) {
        LOGD("Stream disconnected for channel %d", channelIndex);
        streamingActive = false;
        notifyChannelManagerOfStateChange(RTSPStreamManager::DISCONNECTED);
    }
}

void EnhancedMultiChannelZLPlayer::onStreamError(int channelIndex, const std::string& error) {
    if (channelIndex == this->channelIndex) {
        LOGE("Stream error for channel %d: %s", channelIndex, error.c_str());
        notifyChannelManagerOfStateChange(RTSPStreamManager::ERROR);
    }
}

void EnhancedMultiChannelZLPlayer::onFrameReceived(int channelIndex, void* frameData, int size) {
    if (channelIndex == this->channelIndex) {
        handleStreamFrame(frameData, size);
    }
}

void EnhancedMultiChannelZLPlayer::onStreamStateChanged(int channelIndex,
                                                       RTSPStreamManager::StreamState oldState,
                                                       RTSPStreamManager::StreamState newState) {
    if (channelIndex == this->channelIndex) {
        LOGD("Stream state changed for channel %d: %d -> %d", channelIndex, oldState, newState);
        notifyChannelManagerOfStateChange(newState);
    }
}

void EnhancedMultiChannelZLPlayer::handleStreamFrame(void* frameData, int size) {
    // Process the received frame data
    // This would integrate with the existing frame processing pipeline
    if (frameData && size > 0) {
        // Forward to decoder or processing pipeline
        // Implementation depends on the frame format and processing requirements
        LOGD("Received frame for channel %d: %d bytes", channelIndex, size);
    }
}

void EnhancedMultiChannelZLPlayer::notifyChannelManagerOfStateChange(RTSPStreamManager::StreamState state) {
    if (channelManager) {
        // Convert stream state to channel state and notify manager
        switch (state) {
            case RTSPStreamManager::STREAMING:
                // channelManager->onChannelStateChanged(channelIndex, ACTIVE);
                break;
            case RTSPStreamManager::ERROR:
                // channelManager->onChannelStateChanged(channelIndex, ERROR);
                break;
            case RTSPStreamManager::DISCONNECTED:
                // channelManager->onChannelStateChanged(channelIndex, INACTIVE);
                break;
            default:
                break;
        }
    }
}

bool RTSPStreamManager::connectStreamByIndex(int channelIndex) {
    std::lock_guard<std::mutex> lock(streamsMutex);
    auto it = streams.find(channelIndex);
    if (it != streams.end()) {
        return connectStream(it->second.get());
    }
    return false;
}

void RTSPStreamManager::disconnectStreamByIndex(int channelIndex) {
    std::lock_guard<std::mutex> lock(streamsMutex);
    auto it = streams.find(channelIndex);
    if (it != streams.end()) {
        disconnectStream(it->second.get());
    }
}
