/**
 * StreamLumo Frame Writer - Implementation
 * 
 * Captures video frames from OBS at 60 FPS and writes to shared memory.
 * 
 * @license GPL-2.0
 */

#include "frame_writer.h"
#include "../include/shared_buffer.h"

#ifdef _WIN32
#include "shm_win32.h"
using ShmImpl = StreamLumo::ShmWin32;
#else
#include "shm_posix.h"
using ShmImpl = StreamLumo::ShmPosix;
#endif

#include <obs.h>
#include <util/platform.h>
#include <graphics/graphics.h>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <map>

namespace {

// Filter data structure
struct PreviewCaptureData {
    obs_source_t *source;
    StreamLumo::FrameWriter *writer;
    gs_texrender_t *texrender;
    gs_stagesurf_t *stagesurf;
    uint32_t width;
    uint32_t height;
};

// Global map declaration
std::mutex g_filter_map_mutex;
std::map<obs_source_t*, PreviewCaptureData*> g_filter_map;

// Filter callbacks
const char *PreviewCaptureGetName(void *type_data)
{
    UNUSED_PARAMETER(type_data);
    return "StreamLumo Preview Capture";
}

void *PreviewCaptureCreate(obs_data_t *settings, obs_source_t *source)
{
    UNUSED_PARAMETER(settings);
    PreviewCaptureData *data = new PreviewCaptureData();
    data->source = source;
    data->writer = nullptr;
    data->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    data->stagesurf = nullptr;
    data->width = 0;
    data->height = 0;
    
    // Register in global map
    {
        std::lock_guard<std::mutex> lock(g_filter_map_mutex);
        g_filter_map[source] = data;
    }
    
    return data;
}

void PreviewCaptureDestroy(void *data)
{
    PreviewCaptureData *capture = static_cast<PreviewCaptureData*>(data);
    if (capture) {
        // Unregister from global map
        {
            std::lock_guard<std::mutex> lock(g_filter_map_mutex);
            g_filter_map.erase(capture->source);
        }
        
        obs_enter_graphics();
        if (capture->texrender) gs_texrender_destroy(capture->texrender);
        if (capture->stagesurf) gs_stagesurface_destroy(capture->stagesurf);
        obs_leave_graphics();
        delete capture;
    }
}

void PreviewCaptureVideoRender(void *data, gs_effect_t *effect)
{
    PreviewCaptureData *capture = static_cast<PreviewCaptureData*>(data);
    if (!capture || !capture->writer) {
        obs_source_skip_video_filter(capture->source);
        return;
    }

    // Get the target source (the source or filter that feeds into this filter)
    obs_source_t *target = obs_filter_get_target(capture->source);
    if (!target) {
        obs_source_skip_video_filter(capture->source);
        return;
    }

    // 1. Render the target normally to the screen (pass-through)
    // We use skip_video_filter to render the underlying source without this filter
    // This avoids infinite recursion since we are a filter
    obs_source_skip_video_filter(capture->source);

    // 2. Capture the frame
    // Render the target again into our texture
    
    uint32_t width = obs_source_get_base_width(target);
    uint32_t height = obs_source_get_base_height(target);
    
    if (width == 0 || height == 0) return;

    if (capture->width != width || capture->height != height || !capture->stagesurf) {
        capture->width = width;
        capture->height = height;
        if (capture->stagesurf) gs_stagesurface_destroy(capture->stagesurf);
        capture->stagesurf = gs_stagesurface_create(width, height, GS_RGBA);
    }

    // Render to texture
    if (gs_texrender_begin(capture->texrender, width, height)) {
        struct vec4 clear_color;
        vec4_zero(&clear_color);
        gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
        
        // Render the target source into our texture
        // Again, use skip_video_filter to avoid recursion
        obs_source_skip_video_filter(capture->source);
        
        gs_texrender_end(capture->texrender);
    }

    // Copy to staging surface for CPU readback
    gs_texture_t *tex = gs_texrender_get_texture(capture->texrender);
    if (tex) {
        gs_stage_texture(capture->stagesurf, tex);
        
        // Map and read (Blocking! In production use async staging chain)
        uint8_t *ptr;
        uint32_t linesize;
        if (gs_stagesurface_map(capture->stagesurf, &ptr, &linesize)) {
            // Pass to writer
            // We construct a fake video_data/obs_source_frame or just call processFrame directly
            // Since we have RGBA data, we can pass it directly
            const uint8_t *data_arr[1] = { ptr };
            const uint32_t linesize_arr[1] = { linesize };
            
            capture->writer->processFrame(
                data_arr,
                linesize_arr,
                width,
                height,
                VIDEO_FORMAT_RGBA
            );
            
            gs_stagesurface_unmap(capture->stagesurf);
        }
    }
}

struct obs_source_info preview_capture_info = {};

// Global map to store filter data pointers (since we can't access private data from outside)
// This is a hack, but necessary without public API access to private data

void SetPreviewFilterWriter(obs_source_t *filter, StreamLumo::FrameWriter *writer)
{
    std::lock_guard<std::mutex> lock(g_filter_map_mutex);
    auto it = g_filter_map.find(filter);
    if (it != g_filter_map.end()) {
        it->second->writer = writer;
    }
}

} // namespace

// Register the filter
void RegisterPreviewFilter()
{
    preview_capture_info.id = "streamlumo_preview_capture";
    preview_capture_info.type = OBS_SOURCE_TYPE_FILTER;
    preview_capture_info.output_flags = OBS_SOURCE_VIDEO;
    preview_capture_info.get_name = PreviewCaptureGetName;
    preview_capture_info.create = PreviewCaptureCreate;
    preview_capture_info.destroy = PreviewCaptureDestroy;
    preview_capture_info.video_render = PreviewCaptureVideoRender;
    
    obs_register_source(&preview_capture_info);
}

inline uint8_t clampToByte(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

inline void writeRgbaFromYuv(int y, int u, int v, uint8_t *dst)
{
    // Standard BT.601 conversion works well for OBS preview feeds.
    int r = static_cast<int>(y + 1.402f * v);
    int g = static_cast<int>(y - 0.344136f * u - 0.714136f * v);
    int b = static_cast<int>(y + 1.772f * u);

    dst[0] = clampToByte(r);
    dst[1] = clampToByte(g);
    dst[2] = clampToByte(b);
    dst[3] = 255;
}

namespace StreamLumo {

FrameWriter::FrameWriter(const std::string& channelName, Mode mode)
    : m_running(false)
    , m_totalFrames(0)
    , m_droppedFrames(0)
    , m_writtenFrames(0)
    , m_startTime(0)
    , m_lastStatsTime(0)
    , m_channelName(channelName)
    , m_shm(nullptr)
    , m_mode(mode)
    , m_currentSource(nullptr)
    , m_texrender(nullptr)
    , m_stagesurf(nullptr)
    , m_captureWidth(0)
    , m_captureHeight(0)
    , m_tickAccumulator(0.0f)
{
    m_shm = new ShmImpl(channelName);
    // Allocate frame buffer for RGBA conversion
    m_frameBuffer.resize(FRAME_SIZE);
    
    blog(LOG_INFO, "[FrameWriter] Initialized for channel: %s (Mode: %s)", 
         channelName.c_str(), mode == MODE_GLOBAL_OUTPUT ? "Global Output" : "Source Capture");
}

FrameWriter::~FrameWriter()
{
    stop();
    
    // Clean up graphics resources
    obs_enter_graphics();
    if (m_texrender) {
        gs_texrender_destroy(m_texrender);
        m_texrender = nullptr;
    }
    if (m_stagesurf) {
        gs_stagesurface_destroy(m_stagesurf);
        m_stagesurf = nullptr;
    }
    obs_leave_graphics();
    
    if (m_shm) {
        delete m_shm;
        m_shm = nullptr;
    }
    blog(LOG_INFO, "[FrameWriter] Destroyed");
}

bool FrameWriter::connect() {
    if (!m_shm) return false;
    return m_shm->connect();
}

bool FrameWriter::checkPauseRequested() const {
    if (!m_shm || !m_shm->isConnected()) return false;
    
    // Read the pause_requested flag from shared memory
    SharedFrameBuffer* buffer = m_shm->getBuffer();
    if (!buffer) return false;
    
    return buffer->pause_requested.load(std::memory_order_acquire) != 0;
}

void FrameWriter::confirmPaused() {
    if (!m_shm || !m_shm->isConnected()) return;
    
    SharedFrameBuffer* buffer = m_shm->getBuffer();
    if (!buffer) return;
    
    // Set producer_paused to confirm we have stopped
    buffer->producer_paused.store(1, std::memory_order_release);
    blog(LOG_INFO, "[FrameWriter] Confirmed pause to consumer");
}

void FrameWriter::clearPauseState() {
    if (!m_shm || !m_shm->isConnected()) return;
    
    SharedFrameBuffer* buffer = m_shm->getBuffer();
    if (!buffer) return;
    
    // Clear both pause flags
    buffer->pause_requested.store(0, std::memory_order_release);
    buffer->producer_paused.store(0, std::memory_order_release);
}

bool FrameWriter::start()
{
    if (m_running.load()) {
        blog(LOG_WARNING, "[FrameWriter] Already running");
        return true;
    }
    
    blog(LOG_INFO, "[FrameWriter] Starting frame capture...");
    
    // Get OBS video settings
    struct obs_video_info ovi;
    obs_get_video_info(&ovi);
    blog(LOG_INFO, "[FrameWriter] OBS Video Settings:");
    blog(LOG_INFO, "  Resolution: %dx%d", ovi.base_width, ovi.base_height);
    blog(LOG_INFO, "  FPS: %u/%u (%.2f Hz)", ovi.fps_num, ovi.fps_den, (double)ovi.fps_num / ovi.fps_den);
    blog(LOG_INFO, "  Format: %d (0=RGBA, 1=BGRA, 2=I420, 3=NV12, 4=Y800)", ovi.output_format);
    
    // Reset statistics
    m_totalFrames.store(0);
    m_droppedFrames.store(0);
    m_writtenFrames.store(0);
    m_startTime = os_gettime_ns();
    m_lastStatsTime = m_startTime;
    
    if (m_mode == MODE_GLOBAL_OUTPUT) {
        // Register raw video callback on the main video output
        video_t *video = obs_get_video();
        if (!video) {
            blog(LOG_ERROR, "[FrameWriter] Failed to get OBS video context");
            return false;
        }
        
        obs_add_raw_video_callback(nullptr, rawVideoCallback, this);
        blog(LOG_INFO, "[FrameWriter] Capturing from main video output (program)");
    } else {
        // Source capture mode - use tick callback to render source
        obs_add_tick_callback(tickCallback, this);
        blog(LOG_INFO, "[FrameWriter] Source capture mode - using tick callback");
    }
    
    m_running.store(true);
    
    blog(LOG_INFO, "[FrameWriter] Frame capture started successfully");
    
    return true;
}

void FrameWriter::setSource(obs_source_t* source)
{
    if (m_mode != MODE_SOURCE_CAPTURE) return;
    
    std::lock_guard<std::mutex> lock(m_sourceMutex);
    
    if (m_currentSource) {
        // Release reference
        obs_source_release(m_currentSource);
        m_currentSource = nullptr;
    }
    
    m_currentSource = source;
    
    if (m_currentSource) {
        // Add reference to keep source alive
        obs_source_get_ref(m_currentSource);
        blog(LOG_INFO, "[FrameWriter] Switched to source: %s", obs_source_get_name(m_currentSource));
    } else {
        blog(LOG_INFO, "[FrameWriter] Source cleared");
    }
}

void FrameWriter::stop()
{
    if (!m_running.load()) {
        return;
    }
    
    blog(LOG_INFO, "[FrameWriter] Stopping frame capture...");
    
    m_running.store(false);
    
    if (m_mode == MODE_GLOBAL_OUTPUT) {
        obs_remove_raw_video_callback(rawVideoCallback, this);
    } else {
        // Remove tick callback
        obs_remove_tick_callback(tickCallback, this);
        
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        if (m_currentSource) {
            obs_source_release(m_currentSource);
            m_currentSource = nullptr;
        }
    }
    
    // Log final statistics
    auto stats = getStatistics();
    blog(LOG_INFO, "[FrameWriter] Final statistics:");
    blog(LOG_INFO, "[FrameWriter]   Total frames: %llu", stats.totalFrames);
    blog(LOG_INFO, "[FrameWriter]   Written frames: %llu", stats.writtenFrames);
    blog(LOG_INFO, "[FrameWriter]   Dropped frames: %llu", stats.droppedFrames);
    blog(LOG_INFO, "[FrameWriter]   Average FPS: %.2f", stats.averageFps);
    
    blog(LOG_INFO, "[FrameWriter] Frame capture stopped");
}

bool FrameWriter::isRunning() const
{
    return m_running.load();
}

FrameStatistics FrameWriter::getStatistics() const
{
    FrameStatistics stats;
    stats.totalFrames = m_totalFrames.load();
    stats.droppedFrames = m_droppedFrames.load();
    stats.writtenFrames = m_writtenFrames.load();
    
    // Calculate average FPS
    uint64_t elapsed_ns = os_gettime_ns() - m_startTime;
    if (elapsed_ns > 0) {
        double elapsed_s = elapsed_ns / 1000000000.0;
        stats.averageFps = stats.totalFrames / elapsed_s;
    } else {
        stats.averageFps = 0.0;
    }
    
    stats.averageLatencyMs = 0.0; // TODO: Implement latency tracking
    
    return stats;
}

void FrameWriter::rawVideoCallback(void *param, struct video_data *frame)
{
    FrameWriter *writer = static_cast<FrameWriter*>(param);
    if (writer && writer->isRunning()) {
        // For global output, we need to get format info from OBS
        struct obs_video_info ovi;
        obs_get_video_info(&ovi);
        
        writer->processFrame(
            (const uint8_t *const *)frame->data, 
            frame->linesize, 
            ovi.output_width, 
            ovi.output_height, 
            ovi.output_format
        );
    }
}

void FrameWriter::sourceVideoCallback(void *param, obs_source_t *source, const struct obs_source_frame *frame)
{
    UNUSED_PARAMETER(source);
    FrameWriter *writer = static_cast<FrameWriter*>(param);
    if (writer && writer->isRunning()) {
        writer->processFrame(
            (const uint8_t *const *)frame->data, 
            frame->linesize, 
            frame->width, 
            frame->height, 
            frame->format
        );
    }
}

void FrameWriter::tickCallback(void *param, float seconds)
{
    FrameWriter *writer = static_cast<FrameWriter*>(param);
    if (!writer || !writer->isRunning()) return;
    
    // Accumulate time and capture at ~30 FPS
    writer->m_tickAccumulator += seconds;
    const float targetInterval = 1.0f / 30.0f; // 30 FPS
    
    if (writer->m_tickAccumulator >= targetInterval) {
        writer->m_tickAccumulator -= targetInterval;
        writer->captureSourceFrame();
    }
}

void FrameWriter::captureSourceFrame()
{
    obs_source_t *source = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_sourceMutex);
        source = m_currentSource;
        if (source) {
            obs_source_get_ref(source);
        }
    }
    
    if (!source) return;
    
    // Get source dimensions
    uint32_t width = obs_source_get_width(source);
    uint32_t height = obs_source_get_height(source);
    
    if (width == 0 || height == 0) {
        obs_source_release(source);
        return;
    }
    
    obs_enter_graphics();
    
    // Create or resize texrender and stagesurf if needed
    if (!m_texrender) {
        m_texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        if (!m_texrender) {
            blog(LOG_ERROR, "[FrameWriter] Failed to create texrender");
            obs_leave_graphics();
            obs_source_release(source);
            return;
        }
    }
    
    if (m_captureWidth != width || m_captureHeight != height || !m_stagesurf) {
        if (m_stagesurf) {
            gs_stagesurface_destroy(m_stagesurf);
            m_stagesurf = nullptr;
        }
        m_stagesurf = gs_stagesurface_create(width, height, GS_RGBA);
        if (!m_stagesurf) {
            blog(LOG_ERROR, "[FrameWriter] Failed to create stagesurface %dx%d", width, height);
            obs_leave_graphics();
            obs_source_release(source);
            return;
        }
        m_captureWidth = width;
        m_captureHeight = height;
        blog(LOG_INFO, "[FrameWriter] Preview capture resized to %dx%d", width, height);
    }
    
    // Reset the texrender before each use
    gs_texrender_reset(m_texrender);
    
    // Render source to texture
    if (gs_texrender_begin(m_texrender, width, height)) {
        struct vec4 clear_color;
        vec4_zero(&clear_color);
        gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
        
        gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
        gs_blend_state_push();
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
        
        obs_source_video_render(source);
        
        gs_blend_state_pop();
        gs_texrender_end(m_texrender);
        
        // Copy to staging surface
        gs_texture_t *tex = gs_texrender_get_texture(m_texrender);
        if (tex && m_stagesurf) {
            gs_stage_texture(m_stagesurf, tex);
            
            // Map and read pixels
            uint8_t *ptr;
            uint32_t linesize;
            if (gs_stagesurface_map(m_stagesurf, &ptr, &linesize)) {
                // Pass to processFrame
                const uint8_t *data_arr[1] = { ptr };
                const uint32_t linesize_arr[1] = { linesize };
                
                processFrame(data_arr, linesize_arr, width, height, VIDEO_FORMAT_RGBA);
                
                gs_stagesurface_unmap(m_stagesurf);
            } else {
                blog(LOG_WARNING, "[FrameWriter] Failed to map stagesurface");
            }
        } else {
            blog(LOG_WARNING, "[FrameWriter] tex=%p stagesurf=%p", (void*)tex, (void*)m_stagesurf);
        }
    } else {
        blog(LOG_WARNING, "[FrameWriter] Failed to begin texrender (width=%u height=%u)", width, height);
    }
    
    obs_leave_graphics();
    obs_source_release(source);
}

void FrameWriter::processFrame(const uint8_t *const data[], const uint32_t linesize[], uint32_t width, uint32_t height, enum video_format format)
{
    m_totalFrames.fetch_add(1);
    
    // Log statistics every 5 seconds
    uint64_t now = os_gettime_ns();
    if (now - m_lastStatsTime >= 5000000000ULL) { // 5 seconds
        auto stats = getStatistics();
        blog(LOG_INFO, "[FrameWriter:%s] Stats: %llu frames, %.2f FPS, %llu dropped",
             m_channelName.c_str(), stats.totalFrames, stats.averageFps, stats.droppedFrames);
        m_lastStatsTime = now;
    }
    
    // Thread-safe frame conversion and write
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        
        // Convert frame to RGBA with proper stride handling
        convertToRGBA(data, linesize, width, height, format, m_frameBuffer.data());
        
        // Write to shared memory
        if (!m_shm->writeFrame(m_frameBuffer.data(), m_frameBuffer.size())) {
            m_droppedFrames.fetch_add(1);
            // Don't log every dropped frame - only in stats
        } else {
            m_writtenFrames.fetch_add(1);
        }
    }
}

/**
 * Convert video frame to RGBA format
 * 
 * OBS typically provides frames in NV12 or I420 format.
 * We need to convert to RGBA for WebGL rendering.
 */
void FrameWriter::convertToRGBA(const uint8_t *const data[], const uint32_t linesize[], uint32_t width, uint32_t height, enum video_format format, uint8_t *rgbaBuffer)
{
    // Safety check for invalid resolution
    if (width == 0 || height == 0) {
        return;
    }
    
    // Validate linesize (detect stride/alignment issues)
    const uint32_t expected_linesize_rgba = width * 4;
    const uint32_t expected_linesize_yuv = width;
    const uint32_t actual_linesize = linesize[0];
    
    // Debug logging for first few frames or if format changes
    static uint32_t last_log_time = 0;
    static int log_counter = 0;
    uint32_t now = (uint32_t)(os_gettime_ns() / 1000000000ULL);
    
    // Force log for the first 100 calls to ensure we catch it
    if (log_counter < 100 || now != last_log_time) {
        blog(LOG_INFO, "[FrameWriter] Frame Info: %dx%d, Format: %d, Linesize: %u, %u, %u", 
             width, height, format, linesize[0], linesize[1], linesize[2]);
        
        // Warn about stride mismatches
        if (format == VIDEO_FORMAT_RGBA || format == VIDEO_FORMAT_BGRA) {
            if (actual_linesize != expected_linesize_rgba) {
                blog(LOG_WARNING, "[FrameWriter] STRIDE MISMATCH: Expected %u, got %u (padding: %u bytes)",
                     expected_linesize_rgba, actual_linesize, actual_linesize - expected_linesize_rgba);
            }
        } else if (format == VIDEO_FORMAT_NV12 || format == VIDEO_FORMAT_I420) {
             if (actual_linesize != expected_linesize_yuv) {
                blog(LOG_WARNING, "[FrameWriter] YUV STRIDE MISMATCH: Expected %u, got %u (padding: %u bytes)",
                     expected_linesize_yuv, actual_linesize, actual_linesize - expected_linesize_yuv);
            }
        }
        
        last_log_time = now;
        log_counter++;
    }

    // Calculate scaling factors
    // We always write to the full FRAME_WIDTH x FRAME_HEIGHT buffer
    float scale_x = (float)width / FRAME_WIDTH;
    float scale_y = (float)height / FRAME_HEIGHT;

    // Handle different video formats
    switch (format) {
        case VIDEO_FORMAT_I420:
        case VIDEO_FORMAT_NV12:
            {
                const bool isNV12 = (format == VIDEO_FORMAT_NV12);
                const uint8_t *y_plane = data[0];
                const uint8_t *u_plane = data[1];
                const uint8_t *v_plane = isNV12 ? nullptr : data[2];
                const uint32_t y_linesize = linesize[0];
                const uint32_t u_linesize = linesize[1];
                const uint32_t v_linesize = isNV12 ? linesize[1] : linesize[2];

                if (!y_plane || !u_plane || (!isNV12 && !v_plane)) {
                    blog(LOG_WARNING, "[FrameWriter] Missing YUV planes for format %d", format);
                    break;
                }
                
                // Safety check for zero linesize
                if (y_linesize == 0 || u_linesize == 0 || (!isNV12 && v_linesize == 0)) {
                    blog(LOG_ERROR, "[FrameWriter] Invalid linesize for YUV format");
                    break;
                }
                
                for (uint32_t y = 0; y < FRAME_HEIGHT; y++) {
                    uint32_t src_y = (uint32_t)(y * scale_y);
                    if (src_y >= height) src_y = height - 1;

                    const uint8_t *y_row = y_plane + (src_y * y_linesize);
                    const uint8_t *u_row = u_plane + ((src_y / 2) * u_linesize);
                    const uint8_t *v_row = isNV12 ? nullptr : (v_plane + ((src_y / 2) * v_linesize));
                    uint8_t *dst_row = rgbaBuffer + (y * FRAME_WIDTH * 4);

                    for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
                        uint32_t src_x = (uint32_t)(x * scale_x);
                        if (src_x >= width) src_x = width - 1;

                        const uint8_t y_val = y_row[src_x];
                        int u_val = 0;
                        int v_val = 0;

                        if (isNV12) {
                            const uint8_t *uv_row = u_row;
                            uint32_t chroma_offset = (src_x / 2) * 2;
                            // Ensure we don't read past the end of the row
                            if (chroma_offset + 1 >= u_linesize) {
                                chroma_offset = (u_linesize >= 2) ? u_linesize - 2 : 0;
                            }
                            u_val = static_cast<int>(uv_row[chroma_offset]) - 128;
                            v_val = static_cast<int>(uv_row[chroma_offset + 1]) - 128;
                        } else {
                            uint32_t chroma_x = src_x / 2;
                            if (chroma_x >= u_linesize) {
                                chroma_x = (u_linesize > 0) ? u_linesize - 1 : 0;
                            }
                            u_val = static_cast<int>(u_row[chroma_x]) - 128;

                            if (chroma_x >= v_linesize) {
                                chroma_x = (v_linesize > 0) ? v_linesize - 1 : 0;
                            }
                            v_val = static_cast<int>(v_row[chroma_x]) - 128;
                        }

                        writeRgbaFromYuv(y_val, u_val, v_val, dst_row + (x * 4));
                    }
                }
            }
            break;
            
        case VIDEO_FORMAT_UYVY:
            // UYVY is 4:2:2 packed. 4 bytes = 2 pixels.
            // Byte order: U0 Y0 V0 Y1
            {
                const uint8_t *src_data = data[0];
                uint32_t src_linesize = linesize[0];
                
                for (uint32_t y = 0; y < FRAME_HEIGHT; y++) {
                    // Top-Down (No Flip)
                    uint32_t src_y = (uint32_t)(y * scale_y);
                    if (src_y >= height) src_y = height - 1;
                    
                    uint8_t *dst_row = rgbaBuffer + (y * FRAME_WIDTH * 4);
                    const uint8_t *src_row = src_data + (src_y * src_linesize);
                    
                    for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
                        uint32_t src_x = (uint32_t)(x * scale_x);
                        if (src_x >= width) src_x = width - 1;
                        
                        // Calculate block index (2 pixels per block)
                        uint32_t block_idx = (src_x / 2) * 4;
                        bool is_second_pixel = (src_x % 2) == 1;
                        
                        int u = src_row[block_idx + 0] - 128;
                        int y_val = src_row[block_idx + 1 + (is_second_pixel ? 2 : 0)];
                        int v = src_row[block_idx + 2] - 128;
                        
                        // Convert
                        int r = y_val + 1.402 * v;
                        int g = y_val - 0.344136 * u - 0.714136 * v;
                        int b = y_val + 1.772 * u;
                        
                        dst_row[x * 4 + 0] = (uint8_t)std::clamp(r, 0, 255);
                        dst_row[x * 4 + 1] = (uint8_t)std::clamp(g, 0, 255);
                        dst_row[x * 4 + 2] = (uint8_t)std::clamp(b, 0, 255);
                        dst_row[x * 4 + 3] = 255;
                    }
                }
            }
            break;

        case VIDEO_FORMAT_YUY2:
            // YUY2 is 4:2:2 packed. 4 bytes = 2 pixels.
            // Byte order: Y0 U0 Y1 V0
            {
                const uint8_t *src_data = data[0];
                uint32_t src_linesize = linesize[0];
                
                for (uint32_t y = 0; y < FRAME_HEIGHT; y++) {
                    // Top-Down (No Flip)
                    uint32_t src_y = (uint32_t)(y * scale_y);
                    if (src_y >= height) src_y = height - 1;
                    
                    uint8_t *dst_row = rgbaBuffer + (y * FRAME_WIDTH * 4);
                    const uint8_t *src_row = src_data + (src_y * src_linesize);
                    
                    for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
                        uint32_t src_x = (uint32_t)(x * scale_x);
                        if (src_x >= width) src_x = width - 1;
                        
                        // Calculate block index (2 pixels per block)
                        uint32_t block_idx = (src_x / 2) * 4;
                        bool is_second_pixel = (src_x % 2) == 1;
                        
                        int y_val = src_row[block_idx + (is_second_pixel ? 2 : 0)];
                        int u = src_row[block_idx + 1] - 128;
                        int v = src_row[block_idx + 3] - 128;
                        
                        // Convert
                        int r = y_val + 1.402 * v;
                        int g = y_val - 0.344136 * u - 0.714136 * v;
                        int b = y_val + 1.772 * u;
                        
                        dst_row[x * 4 + 0] = (uint8_t)std::clamp(r, 0, 255);
                        dst_row[x * 4 + 1] = (uint8_t)std::clamp(g, 0, 255);
                        dst_row[x * 4 + 2] = (uint8_t)std::clamp(b, 0, 255);
                        dst_row[x * 4 + 3] = 255;
                    }
                }
            }
            break;

        case VIDEO_FORMAT_Y800:
            // Grayscale 8-bit
            {
                const uint8_t *src_data = data[0];
                uint32_t src_linesize = linesize[0];
                
                for (uint32_t y = 0; y < FRAME_HEIGHT; y++) {
                    // Top-Down (No Flip)
                    uint32_t src_y = (uint32_t)(y * scale_y);
                    if (src_y >= height) src_y = height - 1;
                    
                    uint8_t *dst_row = rgbaBuffer + (y * FRAME_WIDTH * 4);
                    const uint8_t *src_row = src_data + (src_y * src_linesize);
                    
                    for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
                        uint32_t src_x = (uint32_t)(x * scale_x);
                        if (src_x >= width) src_x = width - 1;
                        
                        uint8_t val = src_row[src_x];
                        dst_row[x * 4 + 0] = val;
                        dst_row[x * 4 + 1] = val;
                        dst_row[x * 4 + 2] = val;
                        dst_row[x * 4 + 3] = 255;
                    }
                }
            }
            break;

        case VIDEO_FORMAT_RGBA:
            // Already RGBA - use proper stride handling
            {
                const uint8_t *src_data = data[0];
                const uint32_t src_linesize = linesize[0];
                const uint32_t src_stride = src_linesize; // OBS stride (may include padding)
                const uint32_t dst_stride = FRAME_WIDTH * 4; // Our buffer stride (no padding)
                
                // If resolutions match and no padding, use fast path
                if (width == FRAME_WIDTH && height == FRAME_HEIGHT && src_linesize == dst_stride) {
                    // Fast memcpy - no scaling, no stride conversion needed
                    std::memcpy(rgbaBuffer, src_data, FRAME_SIZE);
                } else {
                    // Slow path with proper stride handling
                    for (uint32_t y = 0; y < FRAME_HEIGHT; y++) {
                        uint32_t src_y = (uint32_t)(y * scale_y);
                        if (src_y >= height) src_y = height - 1;
                        
                        uint8_t *dst_row = rgbaBuffer + (y * dst_stride);
                        const uint8_t *src_row = src_data + (src_y * src_stride); // Use stride, not width
                        
                        for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
                            uint32_t src_x = (uint32_t)(x * scale_x);
                            if (src_x >= width) src_x = width - 1;
                            
                            const uint8_t *src_pixel = src_row + (src_x * 4);
                            dst_row[x * 4 + 0] = src_pixel[0];
                            dst_row[x * 4 + 1] = src_pixel[1];
                            dst_row[x * 4 + 2] = src_pixel[2];
                            dst_row[x * 4 + 3] = src_pixel[3];
                        }
                    }
                }
            }
            break;

        case VIDEO_FORMAT_BGRA:
            // BGRA to RGBA - use proper stride handling
            {
                const uint8_t *src_data = data[0];
                const uint32_t src_linesize = linesize[0];
                const uint32_t src_stride = src_linesize; // OBS stride (may include padding)
                const uint32_t dst_stride = FRAME_WIDTH * 4; // Our buffer stride (no padding)
                
                // Optimized path for matching resolutions
                if (width == FRAME_WIDTH && height == FRAME_HEIGHT) {
                    // Row-by-row with stride handling and BGRA->RGBA swap
                    for (uint32_t y = 0; y < height; y++) {
                        uint8_t *dst_row = rgbaBuffer + (y * dst_stride);
                        const uint8_t *src_row = src_data + (y * src_stride); // Use stride
                        
                        for (uint32_t x = 0; x < width; x++) {
                            dst_row[x * 4 + 0] = src_row[x * 4 + 2]; // R = B
                            dst_row[x * 4 + 1] = src_row[x * 4 + 1]; // G = G
                            dst_row[x * 4 + 2] = src_row[x * 4 + 0]; // B = R
                            dst_row[x * 4 + 3] = src_row[x * 4 + 3]; // A = A
                        }
                    }
                } else {
                    // Scaling path with stride handling
                    for (uint32_t y = 0; y < FRAME_HEIGHT; y++) {
                        uint32_t src_y = (uint32_t)(y * scale_y);
                        if (src_y >= height) src_y = height - 1;
                        
                        uint8_t *dst_row = rgbaBuffer + (y * dst_stride);
                        const uint8_t *src_row = src_data + (src_y * src_stride); // Use stride
                        
                        for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
                            uint32_t src_x = (uint32_t)(x * scale_x);
                            if (src_x >= width) src_x = width - 1;
                            
                            const uint8_t *src_pixel = src_row + (src_x * 4);
                            dst_row[x * 4 + 0] = src_pixel[2]; // R = B
                            dst_row[x * 4 + 1] = src_pixel[1]; // G = G
                            dst_row[x * 4 + 2] = src_pixel[0]; // B = R
                            dst_row[x * 4 + 3] = src_pixel[3]; // A = A
                        }
                    }
                }
            }
            break;
            
        default:
            // Unknown format - fill with Red to indicate error
            static bool logged_error = false;
            if (!logged_error) {
                blog(LOG_ERROR, "[FrameWriter] Unsupported video format: %d", format);
                logged_error = true;
            }
            
            // Fill with Red
            for (uint32_t y = 0; y < FRAME_HEIGHT; y++) {
                uint8_t *dst_row = rgbaBuffer + (y * FRAME_WIDTH * 4);
                for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
                    dst_row[x * 4 + 0] = 255; // R
                    dst_row[x * 4 + 1] = 0;   // G
                    dst_row[x * 4 + 2] = 0;   // B
                    dst_row[x * 4 + 3] = 255; // A
                }
            }
            break;
    }

}

} // namespace StreamLumo
