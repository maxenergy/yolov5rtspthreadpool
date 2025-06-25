#define DISABLE_CUDA_SUPPORT 1
#include "GPUAcceleratedRenderer.h"
#include "logging.h"
#include <algorithm>

GPUAcceleratedRenderer::GPUAcceleratedRenderer() 
    : eglDisplay(EGL_NO_DISPLAY), eglContext(EGL_NO_CONTEXT), eglSurface(EGL_NO_SURFACE) {
    LOGD("GPUAcceleratedRenderer created");
}

GPUAcceleratedRenderer::~GPUAcceleratedRenderer() {
    cleanup();
    LOGD("GPUAcceleratedRenderer destroyed");
}

bool GPUAcceleratedRenderer::initialize() {
    std::lock_guard<std::mutex> lock(rendererMutex);
    
    LOGD("Initializing GPU accelerated renderer");
    
    // Detect GPU capabilities
    capabilities = detectCapabilities();
    
    bool success = true;
    
#if !DISABLE_CUDA_SUPPORT
    // Initialize CUDA if available
    if (capabilities.cudaAvailable &&
        (config.preferredAcceleration == OPENCV_CUDA || config.preferredAcceleration == HYBRID)) {
        try {
            // Initialize CUDA streams
            int streamCount = std::min(config.maxConcurrentOperations, 4);
            cudaStreams.resize(streamCount);
            for (int i = 0; i < streamCount; i++) {
                cudaStreams[i] = cv::cuda::Stream();
            }

            // Pre-allocate GPU memory pool
            if (config.enableMemoryPooling) {
                int poolSize = std::min(8, config.maxConcurrentOperations * 2);
                gpuMatPool.reserve(poolSize);
            }

            LOGD("CUDA acceleration initialized successfully");
        } catch (const cv::Exception& e) {
            LOGE("Failed to initialize CUDA: %s", e.what());
            capabilities.cudaAvailable = false;
            success = false;
        }
    }
#else
    // CUDA disabled for Android build
    capabilities.cudaAvailable = false;
    LOGD("CUDA support disabled for Android build");
#endif
    
    // Initialize OpenGL if available
    if (capabilities.openglAvailable && 
        (config.preferredAcceleration == ANDROID_GPU || config.preferredAcceleration == HYBRID)) {
        if (!initializeOpenGL()) {
            LOGE("Failed to initialize OpenGL");
            capabilities.openglAvailable = false;
            success = false;
        } else {
            LOGD("OpenGL acceleration initialized successfully");
        }
    }
    
    if (!capabilities.cudaAvailable && !capabilities.openglAvailable) {
        LOGW("No GPU acceleration available, falling back to CPU");
        if (!config.fallbackToCPU) {
            return false;
        }
    }
    
    LOGD("GPU accelerated renderer initialization %s", success ? "successful" : "partial");
    return true;
}

void GPUAcceleratedRenderer::cleanup() {
    std::lock_guard<std::mutex> lock(rendererMutex);
    
    LOGD("Cleaning up GPU accelerated renderer");
    
    // Cleanup CUDA resources
    {
        std::lock_guard<std::mutex> cudaLock(cudaResourcesMutex);
        gpuMatPool.clear();
        cudaStreams.clear();
    }
    
    // Cleanup OpenGL resources
    cleanupOpenGL();
    
    currentGpuMemoryUsage.store(0);
    activeOperations.store(0);
}

bool GPUAcceleratedRenderer::isInitialized() const {
    return capabilities.cudaAvailable || capabilities.openglAvailable;
}

GPUAcceleratedRenderer::GPUCapabilities GPUAcceleratedRenderer::detectCapabilities() {
    GPUCapabilities caps;
    
    // Detect CUDA capabilities - disabled for now due to OpenCV CUDA unavailability
    caps.cudaAvailable = false;
    caps.cudaDeviceCount = 0;
    caps.cudaMemoryTotal = 0;
    caps.cudaMemoryFree = 0;
    LOGD("CUDA acceleration disabled - OpenCV CUDA not available");
    
    // Detect OpenGL capabilities
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display != EGL_NO_DISPLAY) {
        if (eglInitialize(display, nullptr, nullptr)) {
            caps.openglAvailable = true;
            
            // Get GPU vendor and renderer info
            const char* vendor = (const char*)glGetString(GL_VENDOR);
            const char* renderer = (const char*)glGetString(GL_RENDERER);
            if (vendor) caps.gpuVendor = vendor;
            if (renderer) caps.gpuRenderer = renderer;
            
            // Get max texture size
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &caps.maxTextureSize);
            
            // Check for NPOT support
            const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
            if (extensions && strstr(extensions, "GL_OES_texture_npot")) {
                caps.supportsNPOT = true;
            }
            
            LOGD("OpenGL detected: %s %s, max texture: %d", 
                 caps.gpuVendor.c_str(), caps.gpuRenderer.c_str(), caps.maxTextureSize);
            
            eglTerminate(display);
        }
    }
    
    return caps;
}

bool GPUAcceleratedRenderer::isCudaAvailable() const {
    return capabilities.cudaAvailable;
}

bool GPUAcceleratedRenderer::isOpenGLAvailable() const {
    return capabilities.openglAvailable;
}

void GPUAcceleratedRenderer::setRenderingConfig(const RenderingConfig& newConfig) {
    std::lock_guard<std::mutex> lock(rendererMutex);
    config = newConfig;
    LOGD("Rendering configuration updated");
}

GPUAcceleratedRenderer::RenderingConfig GPUAcceleratedRenderer::getRenderingConfig() const {
    std::lock_guard<std::mutex> lock(rendererMutex);
    return config;
}

void GPUAcceleratedRenderer::setAccelerationType(AccelerationType type) {
    std::lock_guard<std::mutex> lock(rendererMutex);
    config.preferredAcceleration = type;
    LOGD("Acceleration type set to %d", type);
}

bool GPUAcceleratedRenderer::scaleFrame(const uint8_t* srcData, int srcWidth, int srcHeight, int srcStride,
                                       uint8_t* dstData, int dstWidth, int dstHeight, int dstStride,
                                       AccelerationType acceleration) {
    if (!srcData || !dstData) {
        return false;
    }
    
    activeOperations.fetch_add(1);
    
    AccelerationType actualAcceleration = (acceleration == HYBRID) ? 
        selectOptimalAcceleration(SCALING, srcWidth * srcHeight * 4) : acceleration;
    
    bool success = false;
    
    try {
        if (actualAcceleration == OPENCV_CUDA && capabilities.cudaAvailable) {
            // Use CUDA acceleration
            cv::cuda::GpuMat srcGpu = getGpuMat(srcWidth, srcHeight, CV_8UC4);
            cv::cuda::GpuMat dstGpu = getGpuMat(dstWidth, dstHeight, CV_8UC4);
            
            if (uploadToGpu(srcData, srcWidth, srcHeight, srcStride, srcGpu)) {
                if (scaleFrameCuda(srcGpu, dstGpu)) {
                    success = downloadFromGpu(dstGpu, dstData, dstStride);
                }
            }
            
            returnGpuMat(srcGpu);
            returnGpuMat(dstGpu);
            
        } else if (actualAcceleration == ANDROID_GPU && capabilities.openglAvailable) {
            // Use OpenGL acceleration
            success = scaleFrameOpenGL(srcData, srcWidth, srcHeight, dstData, dstWidth, dstHeight);
            
        } else {
            // CPU fallback
            success = fallbackToCPU(SCALING, "GPU acceleration not available or not optimal");
        }
        
    } catch (const std::exception& e) {
        LOGE("GPU scaling failed: %s", e.what());
        success = fallbackToCPU(SCALING, e.what());
    }
    
    activeOperations.fetch_sub(1);
    return success;
}

bool GPUAcceleratedRenderer::rotateFrame(const uint8_t* srcData, int srcWidth, int srcHeight, int srcStride,
                                        uint8_t* dstData, int dstWidth, int dstHeight, int dstStride,
                                        float angle, AccelerationType acceleration) {
    if (!srcData || !dstData) {
        return false;
    }
    
    activeOperations.fetch_add(1);
    
    AccelerationType actualAcceleration = (acceleration == HYBRID) ? 
        selectOptimalAcceleration(ROTATION, srcWidth * srcHeight * 4) : acceleration;
    
    bool success = false;
    
    try {
        if (actualAcceleration == OPENCV_CUDA && capabilities.cudaAvailable) {
            // Use CUDA acceleration
            cv::cuda::GpuMat srcGpu = getGpuMat(srcWidth, srcHeight, CV_8UC4);
            cv::cuda::GpuMat dstGpu = getGpuMat(dstWidth, dstHeight, CV_8UC4);
            
            if (uploadToGpu(srcData, srcWidth, srcHeight, srcStride, srcGpu)) {
                if (rotateFrameCuda(srcGpu, dstGpu, angle)) {
                    success = downloadFromGpu(dstGpu, dstData, dstStride);
                }
            }
            
            returnGpuMat(srcGpu);
            returnGpuMat(dstGpu);
            
        } else if (actualAcceleration == ANDROID_GPU && capabilities.openglAvailable) {
            // Use OpenGL acceleration
            success = rotateFrameOpenGL(srcData, srcWidth, srcHeight, dstData, dstWidth, dstHeight, angle);
            
        } else {
            // CPU fallback
            success = fallbackToCPU(ROTATION, "GPU acceleration not available or not optimal");
        }
        
    } catch (const std::exception& e) {
        LOGE("GPU rotation failed: %s", e.what());
        success = fallbackToCPU(ROTATION, e.what());
    }
    
    activeOperations.fetch_sub(1);
    return success;
}

bool GPUAcceleratedRenderer::convertColorSpace(const uint8_t* srcData, int srcWidth, int srcHeight, int srcStride,
                                              uint8_t* dstData, int dstStride, int srcFormat, int dstFormat,
                                              AccelerationType acceleration) {
    if (!srcData || !dstData) {
        return false;
    }
    
    activeOperations.fetch_add(1);
    
    AccelerationType actualAcceleration = (acceleration == HYBRID) ? 
        selectOptimalAcceleration(COLOR_CONVERSION, srcWidth * srcHeight * 4) : acceleration;
    
    bool success = false;
    
    try {
        if (actualAcceleration == OPENCV_CUDA && capabilities.cudaAvailable) {
            // Use CUDA acceleration
            cv::cuda::GpuMat srcGpu = getGpuMat(srcWidth, srcHeight, CV_8UC4);
            cv::cuda::GpuMat dstGpu = getGpuMat(srcWidth, srcHeight, CV_8UC4);
            
            if (uploadToGpu(srcData, srcWidth, srcHeight, srcStride, srcGpu)) {
                int conversionCode = (srcFormat == CV_8UC3 && dstFormat == CV_8UC4) ? 
                    cv::COLOR_RGB2RGBA : cv::COLOR_RGBA2RGB;
                if (convertColorSpaceCuda(srcGpu, dstGpu, conversionCode)) {
                    success = downloadFromGpu(dstGpu, dstData, dstStride);
                }
            }
            
            returnGpuMat(srcGpu);
            returnGpuMat(dstGpu);
            
        } else {
            // CPU fallback
            success = fallbackToCPU(COLOR_CONVERSION, "GPU acceleration not available or not optimal");
        }
        
    } catch (const std::exception& e) {
        LOGE("GPU color conversion failed: %s", e.what());
        success = fallbackToCPU(COLOR_CONVERSION, e.what());
    }
    
    activeOperations.fetch_sub(1);
    return success;
}

bool GPUAcceleratedRenderer::blendFrames(const uint8_t* src1Data, const uint8_t* src2Data,
                                        int width, int height, int stride, uint8_t* dstData,
                                        float alpha, AccelerationType acceleration) {
    if (!src1Data || !src2Data || !dstData) {
        return false;
    }
    
    activeOperations.fetch_add(1);
    
    AccelerationType actualAcceleration = (acceleration == HYBRID) ? 
        selectOptimalAcceleration(BLENDING, width * height * 4) : acceleration;
    
    bool success = false;
    
    try {
        if (actualAcceleration == OPENCV_CUDA && capabilities.cudaAvailable) {
            // Use CUDA acceleration
            cv::cuda::GpuMat src1Gpu = getGpuMat(width, height, CV_8UC4);
            cv::cuda::GpuMat src2Gpu = getGpuMat(width, height, CV_8UC4);
            cv::cuda::GpuMat dstGpu = getGpuMat(width, height, CV_8UC4);
            
            if (uploadToGpu(src1Data, width, height, stride, src1Gpu) &&
                uploadToGpu(src2Data, width, height, stride, src2Gpu)) {
                if (blendFramesCuda(src1Gpu, src2Gpu, dstGpu, alpha)) {
                    success = downloadFromGpu(dstGpu, dstData, stride);
                }
            }
            
            returnGpuMat(src1Gpu);
            returnGpuMat(src2Gpu);
            returnGpuMat(dstGpu);
            
        } else if (actualAcceleration == ANDROID_GPU && capabilities.openglAvailable) {
            // Use OpenGL acceleration
            success = blendFramesOpenGL(src1Data, src2Data, width, height, dstData, alpha);
            
        } else {
            // CPU fallback
            success = fallbackToCPU(BLENDING, "GPU acceleration not available or not optimal");
        }
        
    } catch (const std::exception& e) {
        LOGE("GPU blending failed: %s", e.what());
        success = fallbackToCPU(BLENDING, e.what());
    }
    
    activeOperations.fetch_sub(1);
    return success;
}

// Resource management and utility methods
cv::cuda::GpuMat GPUAcceleratedRenderer::getGpuMat(int width, int height, int type) {
    std::lock_guard<std::mutex> lock(cudaResourcesMutex);

    // Try to reuse from pool
    for (auto it = gpuMatPool.begin(); it != gpuMatPool.end(); ++it) {
        if (it->cols == width && it->rows == height && it->type() == type) {
            cv::cuda::GpuMat mat = *it;
            gpuMatPool.erase(it);
            return mat;
        }
    }

    // Create new if not found in pool
    return cv::cuda::GpuMat(height, width, type);
}

void GPUAcceleratedRenderer::returnGpuMat(cv::cuda::GpuMat& mat) {
    if (mat.empty()) return;

    std::lock_guard<std::mutex> lock(cudaResourcesMutex);

    // Return to pool if not too many
    if (gpuMatPool.size() < 16) {
        gpuMatPool.push_back(mat);
    }

    mat.release();
}

cv::cuda::Stream GPUAcceleratedRenderer::getCudaStream() {
    std::lock_guard<std::mutex> lock(cudaResourcesMutex);

    if (!cudaStreams.empty()) {
        cv::cuda::Stream stream = cudaStreams.back();
        cudaStreams.pop_back();
        return stream;
    }

    return cv::cuda::Stream();
}

void GPUAcceleratedRenderer::returnCudaStream(cv::cuda::Stream& stream) {
    std::lock_guard<std::mutex> lock(cudaResourcesMutex);

    if (cudaStreams.size() < 8) {
        cudaStreams.push_back(stream);
    }
}

bool GPUAcceleratedRenderer::uploadToGpu(const uint8_t* cpuData, int width, int height, int stride,
                                        cv::cuda::GpuMat& gpuMat) {
    try {
        cv::Mat cpuMat(height, width, CV_8UC4, const_cast<uint8_t*>(cpuData), stride);
        gpuMat.upload(cpuMat);
        return true;
    } catch (const cv::Exception& e) {
        LOGE("Failed to upload to GPU: %s", e.what());
        return false;
    }
}

bool GPUAcceleratedRenderer::downloadFromGpu(const cv::cuda::GpuMat& gpuMat, uint8_t* cpuData, int stride) {
    try {
        cv::Mat cpuMat(gpuMat.rows, gpuMat.cols, gpuMat.type(), cpuData, stride);
        gpuMat.download(cpuMat);
        return true;
    } catch (const cv::Exception& e) {
        LOGE("Failed to download from GPU: %s", e.what());
        return false;
    }
}

GPUAcceleratedRenderer::AccelerationType GPUAcceleratedRenderer::selectOptimalAcceleration(
    OperationType operation, int dataSize) const {

    // Select based on data size and operation type
    if (dataSize < 1024 * 1024) { // < 1MB
        return NONE; // CPU is faster for small data
    }

    if (capabilities.cudaAvailable && dataSize > 4 * 1024 * 1024) { // > 4MB
        return OPENCV_CUDA; // CUDA is better for large data
    }

    if (capabilities.openglAvailable) {
        return ANDROID_GPU; // OpenGL for medium data
    }

    return NONE; // Fallback to CPU
}

bool GPUAcceleratedRenderer::fallbackToCPU(OperationType operation, const std::string& reason) {
    LOGW("Falling back to CPU for operation %d: %s", operation, reason.c_str());

    // Implement CPU fallback for each operation type
    switch (operation) {
        case SCALING:
            // TODO: Implement CPU scaling
            return false;
        case ROTATION:
            // TODO: Implement CPU rotation
            return false;
        case COLOR_CONVERSION:
            // TODO: Implement CPU color conversion
            return false;
        case BLENDING:
            // TODO: Implement CPU blending
            return false;
        case COMPOSITION:
            // TODO: Implement CPU composition
            return false;
        default:
            return false;
    }
}

// Performance monitoring methods
float GPUAcceleratedRenderer::getGpuUtilization() const {
    // Simplified GPU utilization calculation
    int active = activeOperations.load();
    int maxOps = config.maxConcurrentOperations;
    return (maxOps > 0) ? static_cast<float>(active) / maxOps : 0.0f;
}

size_t GPUAcceleratedRenderer::getCurrentMemoryUsage() const {
    return currentGpuMemoryUsage.load();
}

int GPUAcceleratedRenderer::getActiveOperations() const {
    return activeOperations.load();
}

std::vector<std::string> GPUAcceleratedRenderer::getPerformanceReport() const {
    std::vector<std::string> report;

    report.push_back("GPU Accelerated Renderer Performance Report:");
    report.push_back("CUDA Available: " + std::string(capabilities.cudaAvailable ? "Yes" : "No"));
    report.push_back("OpenGL Available: " + std::string(capabilities.openglAvailable ? "Yes" : "No"));
    report.push_back("Active Operations: " + std::to_string(activeOperations.load()));
    report.push_back("GPU Utilization: " + std::to_string(getGpuUtilization() * 100.0f) + "%");
    report.push_back("Memory Usage: " + std::to_string(currentGpuMemoryUsage.load() / (1024 * 1024)) + " MB");

    return report;
}

// CUDA implementation methods
bool GPUAcceleratedRenderer::scaleFrameCuda(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst) {
#if !DISABLE_CUDA_SUPPORT
    try {
        cv::cuda::Stream stream = getCudaStream();
        cv::cuda::resize(src, dst, dst.size(), 0, 0, cv::INTER_LINEAR, stream);
        stream.waitForCompletion();
        returnCudaStream(stream);
        return true;
    } catch (const cv::Exception& e) {
        LOGE("CUDA scaling failed: %s", e.what());
        return false;
    }
#else
    // Fallback to CPU implementation
    cv::Mat srcCpu, dstCpu;
    src.download(srcCpu);
    cv::resize(srcCpu, dstCpu, dst.size(), 0, 0, cv::INTER_LINEAR);
    dst.upload(dstCpu);
    return true;
#endif
}

bool GPUAcceleratedRenderer::rotateFrameCuda(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst, float angle) {
#if !DISABLE_CUDA_SUPPORT
    try {
        cv::Point2f center(src.cols / 2.0f, src.rows / 2.0f);
        cv::Mat rotationMatrix = cv::getRotationMatrix2D(center, angle, 1.0);

        cv::cuda::Stream stream = getCudaStream();
        cv::cuda::warpAffine(src, dst, rotationMatrix, dst.size(), cv::INTER_LINEAR,
                            cv::BORDER_CONSTANT, cv::Scalar(), stream);
        stream.waitForCompletion();
        returnCudaStream(stream);
        return true;
    } catch (const cv::Exception& e) {
        LOGE("CUDA rotation failed: %s", e.what());
        return false;
    }
#else
    // Fallback to CPU implementation
    cv::Mat srcCpu, dstCpu;
    src.download(srcCpu);
    cv::Point2f center(srcCpu.cols / 2.0f, srcCpu.rows / 2.0f);
    cv::Mat rotationMatrix = cv::getRotationMatrix2D(center, angle, 1.0);
    cv::warpAffine(srcCpu, dstCpu, rotationMatrix, dst.size(), cv::INTER_LINEAR,
                   cv::BORDER_CONSTANT, cv::Scalar());
    dst.upload(dstCpu);
    return true;
#endif
}

bool GPUAcceleratedRenderer::convertColorSpaceCuda(const cv::cuda::GpuMat& src, cv::cuda::GpuMat& dst, int code) {
#if !DISABLE_CUDA_SUPPORT
    try {
        cv::cuda::Stream stream = getCudaStream();
        cv::cuda::cvtColor(src, dst, code, 0, stream);
        stream.waitForCompletion();
        returnCudaStream(stream);
        return true;
    } catch (const cv::Exception& e) {
        LOGE("CUDA color conversion failed: %s", e.what());
        return false;
    }
#else
    // Fallback to CPU implementation
    cv::Mat srcCpu, dstCpu;
    src.download(srcCpu);
    cv::cvtColor(srcCpu, dstCpu, code);
    dst.upload(dstCpu);
    return true;
#endif
}

bool GPUAcceleratedRenderer::blendFramesCuda(const cv::cuda::GpuMat& src1, const cv::cuda::GpuMat& src2,
                                            cv::cuda::GpuMat& dst, float alpha) {
#if !DISABLE_CUDA_SUPPORT
    try {
        cv::cuda::Stream stream = getCudaStream();
        cv::cuda::addWeighted(src1, alpha, src2, 1.0f - alpha, 0.0, dst, -1, stream);
        stream.waitForCompletion();
        returnCudaStream(stream);
        return true;
    } catch (const cv::Exception& e) {
        LOGE("CUDA blending failed: %s", e.what());
        return false;
    }
#else
    // Fallback to CPU implementation
    cv::Mat src1Cpu, src2Cpu, dstCpu;
    src1.download(src1Cpu);
    src2.download(src2Cpu);
    cv::addWeighted(src1Cpu, alpha, src2Cpu, 1.0f - alpha, 0.0, dstCpu);
    dst.upload(dstCpu);
    return true;
#endif
}

// OpenGL implementation methods
bool GPUAcceleratedRenderer::initializeOpenGL() {
    // Initialize EGL context for off-screen rendering
    eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay == EGL_NO_DISPLAY) {
        LOGE("Failed to get EGL display");
        return false;
    }

    if (!eglInitialize(eglDisplay, nullptr, nullptr)) {
        LOGE("Failed to initialize EGL");
        return false;
    }

    // Configure EGL
    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint numConfigs;
    if (!eglChooseConfig(eglDisplay, configAttribs, &config, 1, &numConfigs)) {
        LOGE("Failed to choose EGL config");
        return false;
    }

    // Create context
    EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    eglContext = eglCreateContext(eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    if (eglContext == EGL_NO_CONTEXT) {
        LOGE("Failed to create EGL context");
        return false;
    }

    // Create surface
    EGLint surfaceAttribs[] = {
        EGL_WIDTH, 1,
        EGL_HEIGHT, 1,
        EGL_NONE
    };

    eglSurface = eglCreatePbufferSurface(eglDisplay, config, surfaceAttribs);
    if (eglSurface == EGL_NO_SURFACE) {
        LOGE("Failed to create EGL surface");
        return false;
    }

    if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
        LOGE("Failed to make EGL context current");
        return false;
    }

    LOGD("OpenGL ES context initialized successfully");
    return true;
}

void GPUAcceleratedRenderer::cleanupOpenGL() {
    std::lock_guard<std::mutex> lock(openglResourcesMutex);

    // Clean up textures and framebuffers
    for (auto& pair : textureCache) {
        glDeleteTextures(1, &pair.second);
    }
    textureCache.clear();

    for (auto& pair : framebufferCache) {
        glDeleteFramebuffers(1, &pair.second);
    }
    framebufferCache.clear();

    // Clean up EGL context
    if (eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (eglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(eglDisplay, eglSurface);
            eglSurface = EGL_NO_SURFACE;
        }

        if (eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(eglDisplay, eglContext);
            eglContext = EGL_NO_CONTEXT;
        }

        eglTerminate(eglDisplay);
        eglDisplay = EGL_NO_DISPLAY;
    }
}

bool GPUAcceleratedRenderer::scaleFrameOpenGL(const uint8_t* srcData, int srcWidth, int srcHeight,
                                             uint8_t* dstData, int dstWidth, int dstHeight) {
    // Simplified OpenGL scaling implementation
    LOGD("OpenGL scaling: %dx%d -> %dx%d", srcWidth, srcHeight, dstWidth, dstHeight);
    return false; // TODO: Implement actual OpenGL scaling
}

bool GPUAcceleratedRenderer::rotateFrameOpenGL(const uint8_t* srcData, int srcWidth, int srcHeight,
                                              uint8_t* dstData, int dstWidth, int dstHeight, float angle) {
    // Simplified OpenGL rotation implementation
    LOGD("OpenGL rotation: %dx%d, angle: %.2f", srcWidth, srcHeight, angle);
    return false; // TODO: Implement actual OpenGL rotation
}

bool GPUAcceleratedRenderer::blendFramesOpenGL(const uint8_t* src1Data, const uint8_t* src2Data,
                                              int width, int height, uint8_t* dstData, float alpha) {
    // Simplified OpenGL blending implementation
    LOGD("OpenGL blending: %dx%d, alpha: %.2f", width, height, alpha);
    return false; // TODO: Implement actual OpenGL blending
}

// Memory management methods
bool GPUAcceleratedRenderer::allocateGpuMemory(size_t size) {
    size_t current = currentGpuMemoryUsage.load();
    if (current + size > config.maxGpuMemoryUsage) {
        LOGW("GPU memory allocation would exceed limit: %zu + %zu > %zu",
             current, size, config.maxGpuMemoryUsage);
        return false;
    }

    currentGpuMemoryUsage.fetch_add(size);
    return true;
}

void GPUAcceleratedRenderer::releaseGpuMemory(size_t size) {
    currentGpuMemoryUsage.fetch_sub(size);
}

size_t GPUAcceleratedRenderer::getAvailableGpuMemory() const {
    size_t current = currentGpuMemoryUsage.load();
    return (current < config.maxGpuMemoryUsage) ?
           config.maxGpuMemoryUsage - current : 0;
}

void GPUAcceleratedRenderer::optimizeMemoryUsage() {
    std::lock_guard<std::mutex> lock(cudaResourcesMutex);

    // Clear GPU mat pool if memory usage is high
    if (currentGpuMemoryUsage.load() > config.maxGpuMemoryUsage * 0.8f) {
        gpuMatPool.clear();
        LOGD("Cleared GPU memory pool due to high usage");
    }
}

// Texture and framebuffer management
GLuint GPUAcceleratedRenderer::getTexture(int width, int height) {
    std::lock_guard<std::mutex> lock(openglResourcesMutex);

    int key = width * 10000 + height; // Simple key generation
    auto it = textureCache.find(key);
    if (it != textureCache.end()) {
        GLuint texture = it->second;
        textureCache.erase(it);
        return texture;
    }

    // Create new texture
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    return texture;
}

void GPUAcceleratedRenderer::returnTexture(GLuint texture) {
    // For simplicity, just delete the texture
    glDeleteTextures(1, &texture);
}

GLuint GPUAcceleratedRenderer::getFramebuffer() {
    GLuint framebuffer;
    glGenFramebuffers(1, &framebuffer);
    return framebuffer;
}

void GPUAcceleratedRenderer::returnFramebuffer(GLuint framebuffer) {
    glDeleteFramebuffers(1, &framebuffer);
}
