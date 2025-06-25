package com.wulala.myyolov5rtspthreadpool;

import android.content.Context;
import android.util.Log;
import android.view.Surface;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Multi-Channel Manager for coordinating multiple RTSP video streams
 * Manages up to 16 simultaneous channels with independent processing
 */
public class ChannelManager {

    private static final String TAG = "ChannelManager";

    public enum ChannelState {
        INACTIVE,       // Channel not configured
        CONNECTING,     // Attempting to connect to RTSP stream
        ACTIVE,         // Successfully streaming and processing
        ERROR,          // Connection or processing error
        RECONNECTING    // Attempting to reconnect after error
    }
    
    public static class ChannelConfig {
        public int channelIndex;
        public String rtspUrl;
        public boolean detectionEnabled;
        public boolean recordingEnabled;
        public String channelName;
        public int priority; // 0-highest, 10-lowest
        public int maxRetries;
        public int reconnectDelay; // seconds
        
        public ChannelConfig(int channelIndex, String rtspUrl) {
            this.channelIndex = channelIndex;
            this.rtspUrl = rtspUrl;
            this.detectionEnabled = true;
            this.recordingEnabled = false;
            this.channelName = "Channel " + (channelIndex + 1);
            this.priority = 5;
            this.maxRetries = 3;
            this.reconnectDelay = 5;
        }
    }
    
    public static class ChannelStatus {
        public int channelIndex;
        public ChannelState state;
        public String rtspUrl;
        public long lastFrameTime;
        public int frameCount;
        public float fps;
        public int detectionCount;
        public String errorMessage;
        public int retryCount;
        
        public ChannelStatus(int channelIndex) {
            this.channelIndex = channelIndex;
            this.state = ChannelState.INACTIVE;
            this.lastFrameTime = 0;
            this.frameCount = 0;
            this.fps = 0.0f;
            this.detectionCount = 0;
            this.retryCount = 0;
        }
    }
    
    public interface ChannelEventListener {
        void onChannelStateChanged(int channelIndex, ChannelState oldState, ChannelState newState);
        void onChannelError(int channelIndex, String errorMessage);
        void onChannelFrameReceived(int channelIndex, long frameTime);
        void onChannelDetection(int channelIndex, int detectionCount);
        void onSystemPerformanceUpdate(float overallFps, int activeChannels);
    }
    
    private static final int MAX_CHANNELS = 16;
    private static final int PERFORMANCE_UPDATE_INTERVAL = 1000; // ms
    
    private Context context;
    private Map<Integer, ChannelConfig> channelConfigs;
    private Map<Integer, ChannelStatus> channelStatuses;
    private Map<Integer, Long> nativePlayerObjects; // Native ZLPlayer instances
    private Map<Integer, Surface> channelSurfaces;
    private ChannelEventListener eventListener;
    
    // Shared resources
    private byte[] sharedModelData;
    private int sharedModelSize;
    private ExecutorService channelExecutor;
    private ExecutorService reconnectExecutor;
    private ScheduledExecutorService surfaceHealthMonitor;
    
    // Performance monitoring
    private AtomicInteger totalFrameCount;
    private long lastPerformanceUpdate;
    private float systemFps;
    private int activeChannelCount;
    
    // Thread safety
    private final Object channelLock = new Object();
    
    public ChannelManager(Context context) {
        this.context = context;
        this.channelConfigs = new ConcurrentHashMap<>();
        this.channelStatuses = new ConcurrentHashMap<>();
        this.nativePlayerObjects = new ConcurrentHashMap<>();
        this.channelSurfaces = new ConcurrentHashMap<>();
        this.totalFrameCount = new AtomicInteger(0);
        this.lastPerformanceUpdate = System.currentTimeMillis();
        
        // Initialize thread pools
        this.channelExecutor = Executors.newFixedThreadPool(MAX_CHANNELS);
        this.reconnectExecutor = Executors.newFixedThreadPool(4);
        this.surfaceHealthMonitor = Executors.newScheduledThreadPool(1);
        
        // Initialize all channel statuses
        for (int i = 0; i < MAX_CHANNELS; i++) {
            channelStatuses.put(i, new ChannelStatus(i));
        }

        // Start Surface health monitoring
        startSurfaceHealthMonitoring();
    }
    
    public void setSharedModelData(byte[] modelData, int modelSize) {
        this.sharedModelData = modelData;
        this.sharedModelSize = modelSize;

        // Initialize native channel manager
        if (!initializeNativeChannelManager(modelData)) {
            throw new RuntimeException("Failed to initialize native channel manager");
        }
    }
    
    public void setEventListener(ChannelEventListener listener) {
        this.eventListener = listener;
    }
    
    public boolean configureChannel(ChannelConfig config) {
        if (config.channelIndex < 0 || config.channelIndex >= MAX_CHANNELS) {
            return false;
        }
        
        synchronized (channelLock) {
            channelConfigs.put(config.channelIndex, config);
            ChannelStatus status = channelStatuses.get(config.channelIndex);
            if (status != null) {
                status.rtspUrl = config.rtspUrl;
            }
        }
        
        return true;
    }
    
    public boolean startChannel(int channelIndex) {
        ChannelConfig config = channelConfigs.get(channelIndex);
        if (config == null) {
            notifyChannelError(channelIndex, "Channel not configured");
            return false;
        }
        
        synchronized (channelLock) {
            ChannelStatus status = channelStatuses.get(channelIndex);
            if (status == null) return false;
            
            if (status.state == ChannelState.ACTIVE || status.state == ChannelState.CONNECTING) {
                return true; // Already active or connecting
            }
            
            updateChannelState(channelIndex, ChannelState.CONNECTING);
        }
        
        // Start channel asynchronously
        channelExecutor.submit(() -> startChannelInternal(channelIndex, config));
        return true;
    }
    
    private void startChannelInternal(int channelIndex, ChannelConfig config) {
        Log.d(TAG, "startChannelInternal called for channel " + channelIndex);
        try {
            // Create native player instance
            Log.d(TAG, "Creating native player for channel " + channelIndex);
            long nativePlayer = createNativePlayer(sharedModelData, sharedModelSize);
            if (nativePlayer == 0) {
                throw new RuntimeException("Failed to create native player");
            }
            Log.d(TAG, "Native player created successfully for channel " + channelIndex + ", pointer: " + nativePlayer);
            
            // Configure RTSP URL
            setChannelRTSPUrl(nativePlayer, config.rtspUrl);
            
            // Set surface if available
            Surface surface = channelSurfaces.get(channelIndex);
            Log.d(TAG, "Checking surface for channel " + channelIndex + ", surface: " + surface);

            if (surface != null) {
                Log.d(TAG, "Surface found for channel " + channelIndex + ", valid: " + surface.isValid());
                if (surface.isValid()) {
                    Log.d(TAG, "Setting valid surface on native player for channel " + channelIndex);
                    setChannelSurfaceNative(nativePlayer, surface);
                    Log.d(TAG, "Surface successfully set on native player for channel " + channelIndex);
                } else {
                    Log.e(TAG, "Surface for channel " + channelIndex + " is not valid, skipping");
                }
            } else {
                Log.w(TAG, "No surface available for channel " + channelIndex + " in channelSurfaces map");
                Log.d(TAG, "Current channelSurfaces map contents:");
                for (Map.Entry<Integer, Surface> entry : channelSurfaces.entrySet()) {
                    Log.d(TAG, "  Channel " + entry.getKey() + ": " + entry.getValue());
                }
            }
            
            // Configure detection settings
            setChannelDetectionEnabled(nativePlayer, config.detectionEnabled);
            
            // Start the player
            if (startNativePlayer(nativePlayer)) {
                synchronized (channelLock) {
                    nativePlayerObjects.put(channelIndex, nativePlayer);
                    updateChannelState(channelIndex, ChannelState.ACTIVE);
                    activeChannelCount++;
                }
            } else {
                destroyNativePlayer(nativePlayer);
                throw new RuntimeException("Failed to start native player");
            }
            
        } catch (Exception e) {
            updateChannelState(channelIndex, ChannelState.ERROR);
            notifyChannelError(channelIndex, e.getMessage());
            scheduleReconnect(channelIndex, config);
        }
    }
    
    public boolean stopChannel(int channelIndex) {
        synchronized (channelLock) {
            Long nativePlayer = nativePlayerObjects.remove(channelIndex);
            if (nativePlayer != null) {
                stopNativePlayer(nativePlayer);
                destroyNativePlayer(nativePlayer);
                activeChannelCount--;
            }
            
            updateChannelState(channelIndex, ChannelState.INACTIVE);
            
            ChannelStatus status = channelStatuses.get(channelIndex);
            if (status != null) {
                status.retryCount = 0;
                status.errorMessage = null;
            }
        }
        
        return true;
    }
    
    public void setChannelSurface(int channelIndex, Surface surface) {
        Log.d(TAG, "Setting surface for channel " + channelIndex + ", surface: " + surface);

        // Validate surface
        if (surface != null) {
            Log.d(TAG, "Channel " + channelIndex + " - Surface valid: " + surface.isValid());
            if (!surface.isValid()) {
                Log.e(TAG, "Channel " + channelIndex + " - Surface is not valid!");
                return;
            }
        }

        // Store surface reference (ConcurrentHashMap doesn't allow null values)
        Surface previousSurface = channelSurfaces.get(channelIndex);
        if (previousSurface != null) {
            Log.d(TAG, "Channel " + channelIndex + " - Replacing previous surface: " + previousSurface);
        }

        if (surface != null) {
            channelSurfaces.put(channelIndex, surface);
            Log.d(TAG, "Channel " + channelIndex + " - Surface stored in channelSurfaces map");
        } else {
            // Remove surface from map when it's null (surface destroyed)
            channelSurfaces.remove(channelIndex);
            Log.d(TAG, "Channel " + channelIndex + " - Surface removed from channelSurfaces map (surface destroyed)");
        }

        // Set surface on native player if it exists
        Long nativePlayer = nativePlayerObjects.get(channelIndex);
        if (nativePlayer != null && nativePlayer != 0) {
            Log.d(TAG, "Setting surface on existing native player for channel " + channelIndex + ", player pointer: " + nativePlayer);
            setChannelSurfaceNative(nativePlayer, surface);
            Log.d(TAG, "Surface set on native player for channel " + channelIndex);
        } else {
            Log.d(TAG, "No native player yet for channel " + channelIndex + " (pointer: " + nativePlayer + "), surface will be set when player is created");
        }
    }
    
    public ChannelStatus getChannelStatus(int channelIndex) {
        return channelStatuses.get(channelIndex);
    }
    
    public List<ChannelStatus> getAllChannelStatuses() {
        return new ArrayList<>(channelStatuses.values());
    }

    // Debug method to check surface states
    public void debugSurfaceStates() {
        Log.d(TAG, "=== Surface States Debug ===");
        Log.d(TAG, "Total surfaces in map: " + channelSurfaces.size());

        for (Map.Entry<Integer, Surface> entry : channelSurfaces.entrySet()) {
            int channelIndex = entry.getKey();
            Surface surface = entry.getValue();

            Log.d(TAG, "Channel " + channelIndex + ":");
            Log.d(TAG, "  Surface: " + surface);
            Log.d(TAG, "  Valid: " + (surface != null && surface.isValid()));

            Long nativePlayer = nativePlayerObjects.get(channelIndex);
            Log.d(TAG, "  Native Player: " + nativePlayer);
            Log.d(TAG, "  Player exists: " + (nativePlayer != null && nativePlayer != 0));
        }
        Log.d(TAG, "=== End Surface States Debug ===");
    }

    // Check if a channel is currently running
    public boolean isChannelRunning(int channelIndex) {
        Long nativePlayer = nativePlayerObjects.get(channelIndex);
        return nativePlayer != null && nativePlayer != 0;
    }
    
    public int getActiveChannelCount() {
        return activeChannelCount;
    }
    
    public float getSystemFps() {
        return systemFps;
    }
    
    private void updateChannelState(int channelIndex, ChannelState newState) {
        ChannelStatus status = channelStatuses.get(channelIndex);
        if (status != null) {
            ChannelState oldState = status.state;
            status.state = newState;
            
            if (eventListener != null) {
                eventListener.onChannelStateChanged(channelIndex, oldState, newState);
            }
        }
    }
    
    private void notifyChannelError(int channelIndex, String errorMessage) {
        ChannelStatus status = channelStatuses.get(channelIndex);
        if (status != null) {
            status.errorMessage = errorMessage;
        }
        
        if (eventListener != null) {
            eventListener.onChannelError(channelIndex, errorMessage);
        }
    }
    
    private void scheduleReconnect(int channelIndex, ChannelConfig config) {
        ChannelStatus status = channelStatuses.get(channelIndex);
        if (status == null || status.retryCount >= config.maxRetries) {
            return;
        }
        
        status.retryCount++;
        updateChannelState(channelIndex, ChannelState.RECONNECTING);
        
        reconnectExecutor.submit(() -> {
            try {
                Thread.sleep(config.reconnectDelay * 1000);
                startChannelInternal(channelIndex, config);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        });
    }
    
    // Performance monitoring
    public void onFrameReceived(int channelIndex) {
        ChannelStatus status = channelStatuses.get(channelIndex);
        if (status != null) {
            status.lastFrameTime = System.currentTimeMillis();
            status.frameCount++;
        }
        
        totalFrameCount.incrementAndGet();
        updatePerformanceMetrics();
        
        if (eventListener != null) {
            eventListener.onChannelFrameReceived(channelIndex, System.currentTimeMillis());
        }
    }
    
    public void onDetectionReceived(int channelIndex, int detectionCount) {
        ChannelStatus status = channelStatuses.get(channelIndex);
        if (status != null) {
            status.detectionCount += detectionCount;
        }
        
        if (eventListener != null) {
            eventListener.onChannelDetection(channelIndex, detectionCount);
        }
    }
    
    private void updatePerformanceMetrics() {
        long currentTime = System.currentTimeMillis();
        if (currentTime - lastPerformanceUpdate >= PERFORMANCE_UPDATE_INTERVAL) {
            // Calculate system FPS
            int frameCount = totalFrameCount.getAndSet(0);
            float deltaTime = (currentTime - lastPerformanceUpdate) / 1000.0f;
            systemFps = frameCount / deltaTime;
            lastPerformanceUpdate = currentTime;
            
            // Update individual channel FPS
            for (ChannelStatus status : channelStatuses.values()) {
                if (status.state == ChannelState.ACTIVE) {
                    status.fps = status.frameCount / deltaTime;
                    status.frameCount = 0;
                }
            }
            
            if (eventListener != null) {
                eventListener.onSystemPerformanceUpdate(systemFps, activeChannelCount);
            }
        }
    }
    
    public void cleanup() {
        // Stop all channels
        for (int i = 0; i < MAX_CHANNELS; i++) {
            stopChannel(i);
        }

        // Cleanup native resources
        cleanupNative();

        // Shutdown thread pools
        channelExecutor.shutdown();
        reconnectExecutor.shutdown();
        if (surfaceHealthMonitor != null) {
            surfaceHealthMonitor.shutdown();
        }

        // Clear collections
        channelConfigs.clear();
        channelStatuses.clear();
        nativePlayerObjects.clear();
        channelSurfaces.clear();
    }

    // Surface health monitoring and recovery
    private Map<Integer, Long> lastRecoveryAttemptTime = new HashMap<>();
    private Map<Integer, Integer> recoveryAttemptCount = new HashMap<>();
    private static final long RECOVERY_ATTEMPT_INTERVAL_MS = 5000; // 5 seconds
    private static final int MAX_RECOVERY_ATTEMPTS = 3;
    private static final long SURFACE_HEALTH_CHECK_INTERVAL_MS = 2000; // 2 seconds

    private void startSurfaceHealthMonitoring() {
        Log.d(TAG, "Starting Surface health monitoring with interval: " + SURFACE_HEALTH_CHECK_INTERVAL_MS + "ms");

        surfaceHealthMonitor.scheduleAtFixedRate(() -> {
            try {
                checkSurfaceHealth();
            } catch (Exception e) {
                Log.e(TAG, "Error in Surface health monitoring", e);
            }
        }, SURFACE_HEALTH_CHECK_INTERVAL_MS, SURFACE_HEALTH_CHECK_INTERVAL_MS, TimeUnit.MILLISECONDS);
    }

    public void checkSurfaceHealth() {
        long currentTime = System.currentTimeMillis();

        // Log health check activity (only occasionally to avoid spam)
        if (currentTime % 10000 < SURFACE_HEALTH_CHECK_INTERVAL_MS) { // Log every ~10 seconds
            Log.d(TAG, "Performing Surface health check for " + nativePlayerObjects.size() + " channels");
        }

        for (Map.Entry<Integer, Long> entry : nativePlayerObjects.entrySet()) {
            int channelIndex = entry.getKey();
            Long nativePlayer = entry.getValue();

            if (nativePlayer != null && nativePlayer != 0) {
                try {
                    // Check if surface recovery is requested
                    if (isSurfaceRecoveryRequested(nativePlayer)) {
                        Log.w(TAG, "Surface recovery requested for channel " + channelIndex);

                        // Check if enough time has passed since last recovery attempt
                        Long lastAttempt = lastRecoveryAttemptTime.get(channelIndex);
                        if (lastAttempt == null || (currentTime - lastAttempt) > RECOVERY_ATTEMPT_INTERVAL_MS) {

                            Integer attemptCount = recoveryAttemptCount.getOrDefault(channelIndex, 0);
                            if (attemptCount < MAX_RECOVERY_ATTEMPTS) {
                                Log.d(TAG, "Attempting surface recovery for channel " + channelIndex +
                                      " (attempt " + (attemptCount + 1) + "/" + MAX_RECOVERY_ATTEMPTS + ")");

                                recoverChannelSurface(channelIndex);
                                lastRecoveryAttemptTime.put(channelIndex, currentTime);
                                recoveryAttemptCount.put(channelIndex, attemptCount + 1);
                            } else {
                                Log.e(TAG, "Maximum recovery attempts reached for channel " + channelIndex +
                                      ", forcing surface reset");
                                forceSurfaceReset(nativePlayer);
                                // Reset counters after force reset
                                lastRecoveryAttemptTime.remove(channelIndex);
                                recoveryAttemptCount.remove(channelIndex);
                            }
                        }
                    } else {
                        // Reset recovery counters if no recovery is requested
                        if (recoveryAttemptCount.containsKey(channelIndex)) {
                            recoveryAttemptCount.remove(channelIndex);
                            lastRecoveryAttemptTime.remove(channelIndex);
                        }
                    }

                    // Validate surface health
                    if (!validateSurfaceHealth(nativePlayer)) {
                        Log.w(TAG, "Surface health check failed for channel " + channelIndex);
                        // Health check failure is logged but doesn't trigger immediate recovery
                        // Recovery is only triggered by explicit recovery requests
                    }
                } catch (Exception e) {
                    Log.e(TAG, "Error checking surface health for channel " + channelIndex, e);
                }
            }
        }
    }

    private void recoverChannelSurface(int channelIndex) {
        Log.d(TAG, "Attempting to recover surface for channel " + channelIndex);

        try {
            // Get current surface from the surface manager
            Surface currentSurface = channelSurfaces.get(channelIndex);

            if (currentSurface != null && currentSurface.isValid()) {
                Log.d(TAG, "Re-setting valid surface for channel " + channelIndex);
                setChannelSurface(channelIndex, currentSurface);

                // Clear recovery request
                Long nativePlayer = nativePlayerObjects.get(channelIndex);
                if (nativePlayer != null && nativePlayer != 0) {
                    clearSurfaceRecoveryRequest(nativePlayer);
                }
            } else {
                Log.w(TAG, "No valid surface available for recovery on channel " + channelIndex);
                // Request surface re-initialization from MainActivity
                if (surfaceRecoveryListener != null) {
                    surfaceRecoveryListener.onSurfaceRecoveryNeeded(channelIndex);
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to recover surface for channel " + channelIndex, e);
        }
    }

    // Interface for surface recovery callbacks
    public interface SurfaceRecoveryListener {
        void onSurfaceRecoveryNeeded(int channelIndex);
    }

    private SurfaceRecoveryListener surfaceRecoveryListener;

    public void setSurfaceRecoveryListener(SurfaceRecoveryListener listener) {
        this.surfaceRecoveryListener = listener;
    }

    // Native method declarations (to be implemented in C++)
    private native boolean initializeNativeChannelManager(byte[] modelData);
    private native long createNativePlayer(byte[] modelData, int modelSize);
    private native void destroyNativePlayer(long nativePlayer);
    private native boolean startNativePlayer(long nativePlayer);
    private native void stopNativePlayer(long nativePlayer);
    private native void setChannelRTSPUrl(long nativePlayer, String rtspUrl);
    private native void setChannelSurfaceNative(long nativePlayer, Surface surface);
    private native void setChannelDetectionEnabled(long nativePlayer, boolean enabled);

    // Surface recovery monitoring
    private native boolean isSurfaceRecoveryRequested(long nativePlayer);
    private native void clearSurfaceRecoveryRequest(long nativePlayer);
    private native boolean validateSurfaceHealth(long nativePlayer);
    private native void forceSurfaceReset(long nativePlayer);

    // Channel Manager specific native methods
    private native boolean createChannel(int channelIndex);
    private native boolean destroyChannel(int channelIndex);
    private native boolean startChannel(int channelIndex, String rtspUrl);
    private native void setChannelSurfaceByIndex(int channelIndex, Surface surface);
    private native int getChannelState(int channelIndex);
    private native float getChannelFps(int channelIndex);
    private native void cleanupNative();

    // Callback methods called from native code
    public void onNativeFrameReceived(int channelIndex) {
        // Update internal frame statistics
        totalFrameCount.incrementAndGet();
        updatePerformanceMetrics();

        if (eventListener != null) {
            eventListener.onChannelFrameReceived(channelIndex, System.currentTimeMillis());
        }
    }

    public void onNativeDetectionReceived(int channelIndex, int detectionCount) {
        // Update detection statistics
        ChannelStatus status = channelStatuses.get(channelIndex);
        if (status != null) {
            status.detectionCount += detectionCount;
        }

        if (eventListener != null) {
            eventListener.onChannelDetection(channelIndex, detectionCount);
        }
    }

    public void onChannelStateChanged(int channelIndex, int newState) {
        ChannelStatus status = channelStatuses.get(channelIndex);
        if (status != null) {
            ChannelState oldState = status.state;
            status.state = ChannelState.values()[newState];

            if (eventListener != null) {
                eventListener.onChannelStateChanged(channelIndex, oldState, status.state);
            }
        }
    }

    public void onChannelError(int channelIndex, String errorMessage) {
        ChannelStatus status = channelStatuses.get(channelIndex);
        if (status != null) {
            status.errorMessage = errorMessage;
            status.state = ChannelState.ERROR;
        }

        if (eventListener != null) {
            eventListener.onChannelError(channelIndex, errorMessage);
        }
    }
}
