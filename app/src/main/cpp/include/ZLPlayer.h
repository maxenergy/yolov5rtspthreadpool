#ifndef AIBOX_ZLPLAYER_H
#define AIBOX_ZLPLAYER_H

#include "safe_queue.h"
#include "util.h"
#include "rknn_api.h"
#include <unistd.h>
#include "rk_mpi.h"
#include "im2d.h"
#include "rga.h"
#include "RgaUtils.h"
#include "im2d.hpp"
#include "rga_utils.h"
#include "mpp_decoder.h"
#include "yolov5_thread_pool.h"
#include "display_queue.h"
#include "EnhancedDetectionRenderer.h"
#include <android/native_window.h>

typedef struct g_rknn_app_context_t {
    FILE *out_fp;
    MppDecoder *decoder;
    Yolov5ThreadPool *yolov5ThreadPool;
    RenderFrameQueue *renderFrameQueue;
    // MppEncoder *encoder;
    // mk_media media;
    // mk_pusher pusher;
    const char *push_url;
    uint64_t pts;
    uint64_t dts;

    int job_cnt;
    int result_cnt;
    int frame_cnt;

} rknn_app_context_t;

class ZLPlayer {

private:
    char *data_source = 0; // 指针 请赋初始值
    pthread_t pid_rtsp = 0;
    pthread_t pid_render = 0;
    char *modelFileContent = 0;
    int modelFileSize = 0;

    std::chrono::steady_clock::time_point nextRendTime;

    // Enhanced detection rendering
    std::shared_ptr<EnhancedDetectionRenderer> enhancedDetectionRenderer;
    std::shared_ptr<DetectionRenderingMonitor> renderingMonitor;
    bool isActiveChannel = false;
    float currentSystemLoad = 0.0f;

    // Channel-specific surface management
    ANativeWindow* channelSurface = nullptr;
    pthread_mutex_t surfaceMutex = PTHREAD_MUTEX_INITIALIZER;

    // Surface health monitoring
    int surfaceInvalidCount = 0;
    int surfaceLockFailCount = 0;
    bool surfaceRecoveryRequested = false;
    long surfaceRecoveryRequestTime = 0;
    int surfaceRecoveryAttempts = 0;
    static const int MAX_SURFACE_INVALID_COUNT = 5;
    static const int MAX_SURFACE_LOCK_FAIL_COUNT = 10;
    static const long SURFACE_RECOVERY_TIMEOUT_MS = 10000; // 10 seconds
    static const int MAX_SURFACE_RECOVERY_ATTEMPTS = 3;

public:
    // static RenderCallback renderCallback;
    rknn_app_context_t app_ctx;
    char rtsp_url[512]; // RTSP URL storage
    bool isStreaming = 0; // 是否播放
    int channelIndex = 0;

    // ZLPlayer(const char *data_source, JNICallbackHelper *helper);
    ZLPlayer(char *modelFileData, int modelDataLen);

    ~ZLPlayer();

    static void mpp_decoder_frame_callback(void *userdata, int width_stride, int height_stride, int width, int height, int format, int fd, void *data);

    int process_video_rtsp();

    // int process_video_rtsp(rknn_app_context_t *ctx, const char *url);
    void setModelFile(char *data, int dataLen);

    // void setRenderCallback(RenderCallback renderCallback_);

    void display();

    void get_detect_result();

    // Enhanced detection rendering methods
    void setEnhancedDetectionRenderer(std::shared_ptr<EnhancedDetectionRenderer> renderer);
    void setRenderingMonitor(std::shared_ptr<DetectionRenderingMonitor> monitor);
    void setChannelIndex(int index);
    void setActiveChannel(bool active);
    void updateSystemLoad(float load);
    float getCurrentSystemLoad() const;

    // Channel-specific surface management
    void setChannelSurface(ANativeWindow* surface);
    ANativeWindow* getChannelSurface() const;
    void renderFrame(uint8_t *src_data, int width, int height, int src_line_size);

    // Surface recovery and health monitoring
    bool isSurfaceRecoveryRequested() const;
    void clearSurfaceRecoveryRequest();
    void requestSurfaceRecovery();
    bool validateSurfaceHealth();
    void forceSurfaceReset();
};

#endif //AIBOX_ZLPLAYER_H
