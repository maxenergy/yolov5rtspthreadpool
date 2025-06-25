#ifndef GPU_ACCELERATED_RENDERER_H
#define GPU_ACCELERATED_RENDERER_H

#include <memory>
#include <mutex>
#include <vector>
#include <atomic>
#include <unordered_map>

// OpenCV includes for GPU acceleration
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc/imgproc.hpp>

// CUDA support disabled for Android build
#define DISABLE_CUDA_SUPPORT 1

#if !DISABLE_CUDA_SUPPORT
#include <opencv2/core/cuda.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudawarping.hpp>
#endif

// Android GPU includes
#include <android/native_window.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "yolo_datatype.h"

/**
 * GPU Accelerated Renderer for Multi-Channel Video Processing
 * Utilizes OpenCV CUDA and Android GPU APIs for high-performance rendering
 */
class GPUAcceleratedRenderer {
public:
    enum AccelerationType {
        NONE = 0,           // CPU-only processing
        OPENCV_CUDA = 1,    // OpenCV CUDA acceleration
        ANDROID_GPU = 2,    // Android GPU/OpenGL ES acceleration
        HYBRID = 3          // Combination of both
    };

    enum OperationType {
        SCALING = 0,
        ROTATION = 1,
        COLOR_CONVERSION = 2,
        BLENDING = 3,
        COMPOSITION = 4
    };

    struct GPUCapabilities {
        bool cudaAvailable;
        bool openglAvailable;
        int cudaDeviceCount;
        size_t cudaMemoryTotal;
        size_t cudaMemoryFree;
        std::string gpuVendor;
        std::string gpuRenderer;
        int maxTextureSize;
        bool supportsNPOT; // Non-Power-Of-Two textures
        
        GPUCapabilities() : cudaAvailable(false), openglAvailable(false),
                           cudaDeviceCount(0), cudaMemoryTotal(0), cudaMemoryFree(0),
                           maxTextureSize(0), supportsNPOT(false) {}
    };

    struct RenderingConfig {
        AccelerationType preferredAcceleration;
        bool enableMemoryPooling;
        bool enableAsyncProcessing;
        int maxConcurrentOperations;
        size_t maxGpuMemoryUsage;
        bool fallbackToCPU;
        
        RenderingConfig() : preferredAcceleration(HYBRID), enableMemoryPooling(true),
                           enableAsyncProcessing(true), maxConcurrentOperations(4),
                           maxGpuMemoryUsage(256 * 1024 * 1024), fallbackToCPU(true) {}
    };

private:
    GPUCapabilities capabilities;
    RenderingConfig config;
    mutable std::mutex rendererMutex;
    
    // OpenCV CUDA resources
    std::vector<cv::cuda::GpuMat> gpuMatPool;
    std::vector<cv::cuda::Stream> cudaStreams;
    mutable std::mutex cudaResourcesMutex;
    
    // OpenGL ES resources
    EGLDisplay eglDisplay;
    EGLContext eglContext;
    EGLSurface eglSurface;
    std::unordered_map<int, GLuint> textureCache;
    std::unordered_map<int, GLuint> framebufferCache;
    mutable std::mutex openglResourcesMutex;
    
    // Performance tracking
    std::atomic<bool> gpuAccelerationEnabled{true};
    std::atomic<size_t> currentGpuMemoryUsage{0};
    std::atomic<int> activeOperations{0};

public:
    GPUAcceleratedRenderer();
    ~GPUAcceleratedRenderer();

    // Initialization and cleanup
    bool initialize();
    void cleanup();
    bool isInitialized() const;
    
    // Capability detection
    GPUCapabilities detectCapabilities();
    bool isCudaAvailable() const;
    bool isOpenGLAvailable() const;
    
    // Configuration
    void setRenderingConfig(const RenderingConfig& config);
    RenderingConfig getRenderingConfig() const;
    void setAccelerationType(AccelerationType type);
    
    // GPU-accelerated operations
    bool scaleFrame(const uint8_t* srcData, int srcWidth, int srcHeight, int srcStride,
                   uint8_t* dstData, int dstWidth, int dstHeight, int dstStride,
                   AccelerationType acceleration = HYBRID);
    
    bool rotateFrame(const uint8_t* srcData, int srcWidth, int srcHeight, int srcStride,
                    uint8_t* dstData, int dstWidth, int dstHeight, int dstStride,
                    float angle, AccelerationType acceleration = HYBRID);
    
    bool convertColorSpace(const uint8_t* srcData, int srcWidth, int srcHeight, int srcStride,
                          uint8_t* dstData, int dstStride, int srcFormat, int dstFormat,
                          AccelerationType acceleration = HYBRID);
    
    bool blendFrames(const uint8_t* src1Data, const uint8_t* src2Data,
                    int width, int height, int stride, uint8_t* dstData,
                    float alpha, AccelerationType acceleration = HYBRID);
    
    bool composeMultiChannelFrame(const std::vector<const uint8_t*>& srcFrames,
                                 const std::vector<cv::Rect>& srcRects,
                                 int srcWidth, int srcHeight, int srcStride,
                                 uint8_t* dstData, int dstWidth, int dstHeight, int dstStride,
                                 AccelerationType acceleration = HYBRID);

    // Memory management
    bool allocateGpuMemory(size_t size);
    void releaseGpuMemory(size_t size);
    size_t getAvailableGpuMemory() const;
    void optimizeMemoryUsage();

    // Performance monitoring
    float getGpuUtilization() const;
    size_t getCurrentMemoryUsage() const;
    int getActiveOperations() const;
    std::vector<std::string> getPerformanceReport() const;

private:
    // OpenCV CUDA implementation
    bool scaleFrameCuda(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst);
    bool rotateFrameCuda(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst, float angle);
    bool convertColorSpaceCuda(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst, int code);
    bool blendFramesCuda(const cv::cuda::GpuMat& src1, const cv::cuda::GpuMat& src2,
                        cv::cuda::GpuMat& dst, float alpha);
    
    // OpenGL ES implementation
    bool initializeOpenGL();
    void cleanupOpenGL();
    bool scaleFrameOpenGL(const uint8_t* srcData, int srcWidth, int srcHeight,
                         uint8_t* dstData, int dstWidth, int dstHeight);
    bool rotateFrameOpenGL(const uint8_t* srcData, int srcWidth, int srcHeight,
                          uint8_t* dstData, int dstWidth, int dstHeight, float angle);
    bool blendFramesOpenGL(const uint8_t* src1Data, const uint8_t* src2Data,
                          int width, int height, uint8_t* dstData, float alpha);
    
    // Resource management
    cv::cuda::GpuMat getGpuMat(int width, int height, int type);
    void returnGpuMat(cv::cuda::GpuMat& mat);
    cv::cuda::Stream getCudaStream();
    void returnCudaStream(cv::cuda::Stream& stream);
    
    GLuint getTexture(int width, int height);
    void returnTexture(GLuint texture);
    GLuint getFramebuffer();
    void returnFramebuffer(GLuint framebuffer);
    
    // Utility methods
    bool uploadToGpu(const uint8_t* cpuData, int width, int height, int stride,
                    cv::cuda::GpuMat& gpuMat);
    bool downloadFromGpu(const cv::cuda::GpuMat& gpuMat, uint8_t* cpuData, int stride);
    AccelerationType selectOptimalAcceleration(OperationType operation, int dataSize) const;
    bool fallbackToCPU(OperationType operation, const std::string& reason);
};

/**
 * GPU Memory Pool Manager
 * Manages GPU memory allocation and reuse for optimal performance
 */
class GPUMemoryPool {
public:
    struct MemoryBlock {
        void* ptr;
        size_t size;
        bool inUse;
        std::chrono::steady_clock::time_point lastUsed;
        
        MemoryBlock(void* p, size_t s) : ptr(p), size(s), inUse(false),
                                        lastUsed(std::chrono::steady_clock::now()) {}
    };

private:
    std::vector<std::unique_ptr<MemoryBlock>> memoryBlocks;
    mutable std::mutex poolMutex;
    std::atomic<size_t> totalAllocated{0};
    std::atomic<size_t> totalUsed{0};
    size_t maxPoolSize;

public:
    GPUMemoryPool(size_t maxSize = 256 * 1024 * 1024);
    ~GPUMemoryPool();

    void* allocate(size_t size);
    void deallocate(void* ptr);
    void cleanup();
    size_t getTotalAllocated() const;
    size_t getTotalUsed() const;
    float getUtilization() const;
};

/**
 * GPU Performance Monitor
 * Monitors GPU performance and provides optimization recommendations
 */
class GPUPerformanceMonitor {
public:
    struct PerformanceMetrics {
        float gpuUtilization;
        size_t memoryUsage;
        float averageOperationTime;
        int operationsPerSecond;
        int failedOperations;
        std::chrono::steady_clock::time_point lastUpdate;
        
        PerformanceMetrics() : gpuUtilization(0.0f), memoryUsage(0),
                              averageOperationTime(0.0f), operationsPerSecond(0),
                              failedOperations(0), lastUpdate(std::chrono::steady_clock::now()) {}
    };

private:
    PerformanceMetrics metrics;
    mutable std::mutex metricsMutex;
    std::atomic<bool> monitoringEnabled{true};

public:
    GPUPerformanceMonitor();
    ~GPUPerformanceMonitor();

    void recordOperation(float operationTime, bool success);
    void updateGpuUtilization(float utilization);
    void updateMemoryUsage(size_t usage);
    
    PerformanceMetrics getMetrics() const;
    std::vector<std::string> generateOptimizationRecommendations() const;
    bool shouldFallbackToCPU() const;
    
    void startMonitoring();
    void stopMonitoring();
    void resetMetrics();
};

#endif // GPU_ACCELERATED_RENDERER_H
