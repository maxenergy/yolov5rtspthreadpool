package com.wulala.myyolov5rtspthreadpool;

import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Multi-Surface Manager for handling multiple SurfaceView instances
 * Used for multi-channel video rendering in NVR system
 */
public class MultiSurfaceManager implements SurfaceHolder.Callback {

    private static final String TAG = "MultiSurfaceManager";

    public interface SurfaceEventListener {
        void onSurfaceCreated(int channelIndex, Surface surface);
        void onSurfaceChanged(int channelIndex, Surface surface, int format, int width, int height);
        void onSurfaceDestroyed(int channelIndex, Surface surface);
    }
    
    private List<SurfaceView> surfaceViews;
    private Map<SurfaceHolder, Integer> holderToChannelMap;
    private Map<Integer, Surface> channelSurfaces;
    private SurfaceEventListener listener;
    
    public MultiSurfaceManager() {
        surfaceViews = new ArrayList<>();
        holderToChannelMap = new HashMap<>();
        channelSurfaces = new HashMap<>();
    }
    
    public void addSurface(int channelIndex, SurfaceView surfaceView) {
        if (surfaceView == null) return;
        
        // Ensure the list is large enough
        while (surfaceViews.size() <= channelIndex) {
            surfaceViews.add(null);
        }
        
        surfaceViews.set(channelIndex, surfaceView);
        
        SurfaceHolder holder = surfaceView.getHolder();
        holder.addCallback(this);
        holderToChannelMap.put(holder, channelIndex);
    }
    
    public void removeSurface(int channelIndex) {
        if (channelIndex >= 0 && channelIndex < surfaceViews.size()) {
            SurfaceView surfaceView = surfaceViews.get(channelIndex);
            if (surfaceView != null) {
                SurfaceHolder holder = surfaceView.getHolder();
                holder.removeCallback(this);
                holderToChannelMap.remove(holder);
                channelSurfaces.remove(channelIndex);
                surfaceViews.set(channelIndex, null);
            }
        }
    }
    
    public Surface getSurface(int channelIndex) {
        return channelSurfaces.get(channelIndex);
    }
    
    public SurfaceView getSurfaceView(int channelIndex) {
        if (channelIndex >= 0 && channelIndex < surfaceViews.size()) {
            return surfaceViews.get(channelIndex);
        }
        return null;
    }
    
    public List<Surface> getAllActiveSurfaces() {
        return new ArrayList<>(channelSurfaces.values());
    }
    
    public int getChannelCount() {
        return channelSurfaces.size();
    }
    
    public boolean isChannelActive(int channelIndex) {
        return channelSurfaces.containsKey(channelIndex);
    }
    
    public void setListener(SurfaceEventListener listener) {
        this.listener = listener;
    }
    
    // SurfaceHolder.Callback implementation
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        long timestamp = System.currentTimeMillis();
        Integer channelIndex = holderToChannelMap.get(holder);

        if (channelIndex != null) {
            Surface surface = holder.getSurface();
            Log.d(TAG, "Surface created for channel " + channelIndex + " at timestamp: " + timestamp +
                  ", surface: " + surface + ", valid: " + (surface != null && surface.isValid()));

            if (surface != null && surface.isValid()) {
                // Log previous surface if exists
                Surface previousSurface = channelSurfaces.get(channelIndex);
                if (previousSurface != null) {
                    Log.d(TAG, "Channel " + channelIndex + ": Replacing previous surface " + previousSurface +
                          " with new surface " + surface + " at timestamp: " + timestamp);
                }

                channelSurfaces.put(channelIndex, surface);

                if (listener != null) {
                    Log.d(TAG, "Notifying listener about surface creation for channel " + channelIndex +
                          " at timestamp: " + timestamp);
                    listener.onSurfaceCreated(channelIndex, surface);
                } else {
                    Log.w(TAG, "No listener set for surface creation notification for channel " + channelIndex);
                }
            } else {
                Log.e(TAG, "Invalid surface created for channel " + channelIndex + " at timestamp: " + timestamp +
                      ", surface null: " + (surface == null) + ", surface valid: " + (surface != null && surface.isValid()));
            }
        } else {
            Log.w(TAG, "No channel index found for surface holder: " + holder + " at timestamp: " + timestamp);
        }
    }
    
    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Integer channelIndex = holderToChannelMap.get(holder);
        if (channelIndex != null) {
            Surface surface = holder.getSurface();
            
            if (listener != null) {
                listener.onSurfaceChanged(channelIndex, surface, format, width, height);
            }
        }
    }
    
    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        long timestamp = System.currentTimeMillis();
        Integer channelIndex = holderToChannelMap.get(holder);

        if (channelIndex != null) {
            Surface surface = channelSurfaces.remove(channelIndex);

            Log.w(TAG, "Surface destroyed for channel " + channelIndex + " at timestamp: " + timestamp +
                  ", surface: " + surface + ", was valid: " + (surface != null && surface.isValid()));

            if (listener != null && surface != null) {
                Log.d(TAG, "Notifying listener about surface destruction for channel " + channelIndex +
                      " at timestamp: " + timestamp);
                listener.onSurfaceDestroyed(channelIndex, surface);
            } else {
                Log.w(TAG, "No listener or surface for destruction notification - channel: " + channelIndex +
                      ", listener: " + (listener != null) + ", surface: " + (surface != null));
            }
        } else {
            Log.w(TAG, "No channel index found for destroyed surface holder: " + holder +
                  " at timestamp: " + timestamp);
        }
    }
    
    public void cleanup() {
        // Remove all callbacks and clear maps
        for (SurfaceView surfaceView : surfaceViews) {
            if (surfaceView != null) {
                surfaceView.getHolder().removeCallback(this);
            }
        }
        
        surfaceViews.clear();
        holderToChannelMap.clear();
        channelSurfaces.clear();
    }
    
    // Utility methods for surface management
    public void enableSurface(int channelIndex, boolean enabled) {
        SurfaceView surfaceView = getSurfaceView(channelIndex);
        if (surfaceView != null) {
            surfaceView.setVisibility(enabled ? SurfaceView.VISIBLE : SurfaceView.GONE);
        }
    }
    
    public void setSurfaceSize(int channelIndex, int width, int height) {
        SurfaceView surfaceView = getSurfaceView(channelIndex);
        if (surfaceView != null) {
            SurfaceHolder holder = surfaceView.getHolder();
            holder.setFixedSize(width, height);
        }
    }
    
    public void setSurfaceFormat(int channelIndex, int format) {
        SurfaceView surfaceView = getSurfaceView(channelIndex);
        if (surfaceView != null) {
            SurfaceHolder holder = surfaceView.getHolder();
            holder.setFormat(format);
        }
    }
    
    // Debug methods
    public String getDebugInfo() {
        StringBuilder sb = new StringBuilder();
        sb.append("MultiSurfaceManager Debug Info:\n");
        sb.append("Total surfaces: ").append(surfaceViews.size()).append("\n");
        sb.append("Active channels: ").append(channelSurfaces.size()).append("\n");
        
        for (Map.Entry<Integer, Surface> entry : channelSurfaces.entrySet()) {
            sb.append("Channel ").append(entry.getKey())
              .append(": ").append(entry.getValue().isValid() ? "Valid" : "Invalid")
              .append("\n");
        }
        
        return sb.toString();
    }
}
