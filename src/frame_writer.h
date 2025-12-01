/**
 * StreamLumo Frame Writer - Header
 * 
 * Captures video frames from OBS at 60 FPS and writes to shared memory.
 * 
 * @license GPL-2.0
 */

#ifndef STREAMLUMO_FRAME_WRITER_H
#define STREAMLUMO_FRAME_WRITER_H

#include <obs.h>
#include <cstdint>
#include <atomic>
#include <vector>
#include <mutex>
#include <string>

// Register the preview capture filter
void RegisterPreviewFilter();

// Forward declaration
namespace StreamLumo {
    class ShmPosix;
    class ShmWin32;
}

#ifdef _WIN32
using ShmImpl = StreamLumo::ShmWin32;
#else
using ShmImpl = StreamLumo::ShmPosix;
#endif

namespace StreamLumo {

/**
 * Frame statistics
 */
struct FrameStatistics {
    uint64_t totalFrames;
    uint64_t droppedFrames;
    uint64_t writtenFrames;
    double averageFps;
    double averageLatencyMs;
};

/**
 * Frame Writer Class
 * 
 * Captures raw video frames from OBS and writes them to shared memory.
 * Uses obs_add_raw_video_callback for direct frame access.
 */
class FrameWriter {
public:
    enum Mode {
        MODE_GLOBAL_OUTPUT,
        MODE_SOURCE_CAPTURE
    };

    FrameWriter(const std::string& channelName, Mode mode = MODE_GLOBAL_OUTPUT);
    ~FrameWriter();
    
    /**
     * Start capturing frames
     */
    bool start();
    
    /**
     * Stop capturing frames
     */
    void stop();

    /**
     * Set the source to capture from (only for MODE_SOURCE_CAPTURE)
     */
    void setSource(obs_source_t* source);
    
    /**
     * Check if currently capturing
     */
    bool isRunning() const;
    
    /**
     * Get statistics
     */
    FrameStatistics getStatistics() const;
    
    /**
     * Connect to shared memory
     */
    bool connect();

    /**
     * Process a single frame
     */
    void processFrame(const uint8_t *const data[], const uint32_t linesize[], uint32_t width, uint32_t height, enum video_format format);

    /**
     * Check if consumer has requested a pause (for settings changes)
     */
    bool checkPauseRequested() const;
    
    /**
     * Confirm to consumer that we have paused
     */
    void confirmPaused();
    
    /**
     * Clear the pause state (called after settings change complete)
     */
    void clearPauseState();

private:
    /**
     * Raw video callback (called by OBS at 60 FPS)
     */
    static void rawVideoCallback(void *param, struct video_data *frame);

    /**
     * Source video callback (called by OBS for specific source)
     */
    static void sourceVideoCallback(void *param, obs_source_t *source, const struct obs_source_frame *frame);
    
    /**
     * Convert NV12/I420 to RGBA
     */
    void convertToRGBA(const uint8_t *const data[], const uint32_t linesize[], uint32_t width, uint32_t height, enum video_format format, uint8_t *dst);
    
    /**
     * Tick callback for source capture mode
     */
    static void tickCallback(void *param, float seconds);
    
    /**
     * Render and capture the current source
     */
    void captureSourceFrame();
    
    // State
    std::atomic<bool> m_running;
    Mode m_mode;
    obs_source_t* m_currentSource;
    std::vector<uint8_t> m_frameBuffer;
    std::mutex m_frameMutex;
    std::mutex m_sourceMutex;
    
    // Graphics resources for source capture
    gs_texrender_t* m_texrender;
    gs_stagesurf_t* m_stagesurf;
    uint32_t m_captureWidth;
    uint32_t m_captureHeight;
    float m_tickAccumulator;
    
    // Statistics
    std::atomic<uint64_t> m_totalFrames;
    std::atomic<uint64_t> m_droppedFrames;
    std::atomic<uint64_t> m_writtenFrames;
    uint64_t m_startTime;
    uint64_t m_lastStatsTime;
    
    // Shared Memory
    ShmImpl* m_shm;
    std::string m_channelName;
};

} // namespace StreamLumo

#endif // STREAMLUMO_FRAME_WRITER_H
