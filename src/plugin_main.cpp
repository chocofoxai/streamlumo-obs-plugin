/**
 * StreamLumo OBS Plugin - Main Entry Point
 * 
 * This plugin captures video frames from OBS at 60 FPS and writes them to
 * shared memory for consumption by the StreamLumo Electron application.
 * 
 * GPL Compliance:
 * - This plugin is GPL-2.0 licensed (links with libobs)
 * - Communicates with proprietary StreamLumo app via shared memory (IPC boundary)
 * - No direct linking between GPL and proprietary code
 * 
 * @license GPL-2.0
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/threading.h>
#include "frame_writer.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("streamlumo-plugin", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "StreamLumo Video Frame Capture Plugin - Shared Memory Output";
}

static StreamLumo::FrameWriter *g_program_writer = nullptr;
static StreamLumo::FrameWriter *g_preview_writer = nullptr;
static bool g_program_active = false;
static bool g_preview_active = false;

/**
 * Update the source for the preview writer
 */
static void update_preview_source()
{
    if (!g_preview_writer) return;

    obs_source_t *source = nullptr;
    if (obs_frontend_preview_program_mode_active()) {
        source = obs_frontend_get_current_preview_scene();
    } else {
        // If not in studio mode, preview is same as program (current scene)
        source = obs_frontend_get_current_scene();
    }

    if (source) {
        g_preview_writer->setSource(source);
        obs_source_release(source); // setSource adds its own ref
    }
}

/**
 * Frontend event callback
 */
static void frontend_event_callback(enum obs_frontend_event event, void *param)
{
    UNUSED_PARAMETER(param);
    
    switch (event) {
        case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
        case OBS_FRONTEND_EVENT_SCENE_CHANGED:
        case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
        case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
            update_preview_source();
            break;
        default:
            break;
    }
}

/**
 * Tick callback to check for shared memory connection and pause requests
 */
static void check_connection_tick(void *param, float seconds)
{
    UNUSED_PARAMETER(param);
    
    static float timer = 0.0f;
    timer += seconds;
    
    // Check for pause requests more frequently (every tick)
    // This allows the consumer to signal us to stop for settings changes
    if (g_program_active && g_program_writer) {
        if (g_program_writer->checkPauseRequested()) {
            blog(LOG_INFO, "[StreamLumo] Pause requested - stopping frame capture for settings change");
            g_program_writer->stop();
            g_program_writer->confirmPaused();
            g_program_active = false;
        }
    }
    
    if (g_preview_active && g_preview_writer) {
        if (g_preview_writer->checkPauseRequested()) {
            blog(LOG_INFO, "[StreamLumo] Pause requested for preview - stopping");
            g_preview_writer->stop();
            g_preview_writer->confirmPaused();
            g_preview_active = false;
        }
    }
    
    // Check every 2 seconds if not active (for auto-reconnect)
    if (timer < 2.0f) return;
    timer = 0.0f;
    
    // Check Program - only restart if not paused by consumer
    if (!g_program_active) {
        if (!g_program_writer) {
            g_program_writer = new StreamLumo::FrameWriter("program", StreamLumo::FrameWriter::MODE_GLOBAL_OUTPUT);
        }
        // Only reconnect if pause is not requested
        if (!g_program_writer->checkPauseRequested()) {
            if (g_program_writer->connect()) {
                if (g_program_writer->start()) {
                    g_program_active = true;
                    blog(LOG_INFO, "[StreamLumo] Program writer started (retry)");
                }
            }
        }
    }

    // Check Preview
    if (!g_preview_active) {
        if (!g_preview_writer) {
            g_preview_writer = new StreamLumo::FrameWriter("preview", StreamLumo::FrameWriter::MODE_SOURCE_CAPTURE);
        }
        if (!g_preview_writer->checkPauseRequested()) {
            if (g_preview_writer->connect()) {
                if (g_preview_writer->start()) {
                    g_preview_active = true;
                    update_preview_source();
                    blog(LOG_INFO, "[StreamLumo] Preview writer started (retry)");
                }
            }
        }
    }
}

/**
 * Plugin load callback
 */
bool obs_module_load(void)
{
    blog(LOG_INFO, "[StreamLumo] Plugin loading...");
    blog(LOG_INFO, "[StreamLumo] Version: 1.0.0");
    blog(LOG_INFO, "[StreamLumo] License: GPL-2.0");
    blog(LOG_INFO, "[StreamLumo] IPC Method: Shared Memory");
    
    // Register tick callback to handle connection retries
    obs_add_tick_callback(check_connection_tick, nullptr);
    
    // Register frontend event callback
    obs_frontend_add_event_callback(frontend_event_callback, nullptr);
    
    // Create frame writers
    g_program_writer = new StreamLumo::FrameWriter("program", StreamLumo::FrameWriter::MODE_GLOBAL_OUTPUT);
    g_preview_writer = new StreamLumo::FrameWriter("preview", StreamLumo::FrameWriter::MODE_SOURCE_CAPTURE);
    
    // Connect Program
    if (g_program_writer->connect()) {
        if (g_program_writer->start()) {
            g_program_active = true;
            blog(LOG_INFO, "[StreamLumo] Program writer started");
        }
    } else {
        blog(LOG_WARNING, "[StreamLumo] Failed to connect Program - waiting for Electron...");
    }

    // Connect Preview
    if (g_preview_writer->connect()) {
        if (g_preview_writer->start()) {
            g_preview_active = true;
            update_preview_source();
            blog(LOG_INFO, "[StreamLumo] Preview writer started");
        }
    } else {
        blog(LOG_WARNING, "[StreamLumo] Failed to connect Preview - waiting for Electron...");
    }
    
    // Register the preview capture filter
    RegisterPreviewFilter();
    
    blog(LOG_INFO, "[StreamLumo] Plugin loaded successfully - 60 FPS capture active");
    blog(LOG_INFO, "[StreamLumo] Target resolution: 1920x1080 RGBA");
    blog(LOG_INFO, "[StreamLumo] Throughput: ~474 MB/s");
    
    return true;
}

/**
 * Plugin unload callback
 */
void obs_module_unload(void)
{
    blog(LOG_INFO, "[StreamLumo] Plugin unloading...");
    
    obs_remove_tick_callback(check_connection_tick, nullptr);
    obs_frontend_remove_event_callback(frontend_event_callback, nullptr);
    
    g_program_active = false;
    g_preview_active = false;
    
    // Stop Program
    if (g_program_writer) {
        g_program_writer->stop();
        delete g_program_writer;
        g_program_writer = nullptr;
    }

    // Stop Preview
    if (g_preview_writer) {
        g_preview_writer->stop();
        delete g_preview_writer;
        g_preview_writer = nullptr;
    }
    
    blog(LOG_INFO, "[StreamLumo] Frame writers stopped");
    
    // Disconnect from shared memory (don't destroy, Electron owns it)
    // ShmImpl::disconnect(); // Handled by FrameWriter destructor
    blog(LOG_INFO, "[StreamLumo] Disconnected from shared memory");
    
    blog(LOG_INFO, "[StreamLumo] Plugin unloaded successfully");
}

/**
 * Module post-load callback (optional)
 */
void obs_module_post_load(void)
{
    blog(LOG_INFO, "[StreamLumo] Post-load callback");
    
    // Log statistics if available
    if (g_program_writer && g_program_active) {
        auto stats = g_program_writer->getStatistics();
        blog(LOG_INFO, "[StreamLumo] Program stats: %llu frames, %.2f FPS", stats.totalFrames, stats.averageFps);
    }
}

/**
 * Get module name
 */
MODULE_EXPORT const char *obs_module_name(void)
{
    return "StreamLumo Frame Capture Plugin";
}
