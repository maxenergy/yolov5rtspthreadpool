package com.wulala.myyolov5rtspthreadpool;

import androidx.appcompat.app.AppCompatActivity;

import android.content.res.AssetManager;
import android.os.Bundle;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.TextView;

import com.wulala.myyolov5rtspthreadpool.databinding.ActivityMainBinding;

public class MainActivity extends AppCompatActivity implements SurfaceHolder.Callback {

    static {
        System.loadLibrary("myyolov5rtspthreadpool");
    }

    private ActivityMainBinding binding;
    AssetManager assetManager;
    private long nativePlayerObj = 0;
    private SurfaceView surfaceView;
    private SurfaceHolder surfaceHolder;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        // Initialize SurfaceView
        surfaceView = binding.surfaceView;
        surfaceHolder = surfaceView.getHolder();
        surfaceHolder.addCallback(this);

        assetManager = getAssets();
        setNativeAssetManager(assetManager);
        nativePlayerObj = prepareNative();
    }

    // SurfaceHolder.Callback methods
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        // Surface is created, set it to native code
        setNativeSurface(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // Surface dimensions changed
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        // Surface is destroyed, clear native surface
        setNativeSurface(null);
    }

    // jni native methods
    private native long prepareNative();
    private native void setNativeAssetManager(AssetManager assetManager);
    private native void setNativeSurface(Object surface);

}