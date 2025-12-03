/**
 * StreamLumo Shared Frame Buffer
 * 
 * Cross-process shared memory structure for 60 FPS video rendering.
 * Uses triple buffering with atomic operations for lock-free synchronization.
 * 
 * GPL-Compliant Architecture:
 * - This header is used by both OBS plugin (GPL) and Electron app (proprietary)
 * - Shared memory acts as IPC boundary between processes
 * - No direct linking between GPL and proprietary code
 * 
 * @license MIT (for this header file)
 */

#ifndef STREAMLUMO_SHARED_BUFFER_H
#define STREAMLUMO_SHARED_BUFFER_H

#include <stdint.h>
#include <atomic>

// Cross-platform alignment macro
#ifdef _MSC_VER
  #define SL_ALIGNED(x) __declspec(align(x))
#else
  #define SL_ALIGNED(x) __attribute__((aligned(x)))
#endif

// Shared memory name for POSIX/Win32
#define SHM_NAME "/streamlumo_frames"
#define SHM_NAME_WIN32 "Local\\StreamLumoFrames"
#define SEM_NAME "/streamlumo_sem"

// Video format constants
#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080
#define FRAME_CHANNELS 4  // RGBA
#define FRAME_SIZE (FRAME_WIDTH * FRAME_HEIGHT * FRAME_CHANNELS)  // 8,294,400 bytes (~7.9 MB)
#define NUM_BUFFERS 3     // Triple buffering

// Pixel format
enum PixelFormat {
    FORMAT_RGBA = 0,
    FORMAT_BGRA = 1,
    FORMAT_RGB = 2,
    FORMAT_BGR = 3
};

/**
 * Shared Frame Buffer Structure
 * 
 * Layout:
 * - Control metadata (64 bytes, cache-aligned)
 * - Triple-buffered frame data (3 × 7.9 MB = ~23.7 MB)
 * 
 * Total size: ~23.7 MB
 * 
 * Synchronization:
 * - write_index: Atomic index of buffer being written (0-2)
 * - read_index: Atomic index of buffer last read (0-2)
 * - Use memory_order_acquire/release for proper memory barriers
 */
struct SharedFrameBuffer {
    // === Control Metadata (64 bytes, cache-aligned) ===
    
    // Atomic indices for lock-free triple buffering
    std::atomic<uint64_t> write_index;      // Current write position (0-2)
    std::atomic<uint64_t> read_index;       // Current read position (0-2)
    
    // Frame metadata
    uint32_t width;                         // Frame width (default: 1920)
    uint32_t height;                        // Frame height (default: 1080)
    uint32_t frame_size;                    // Bytes per frame (8,294,400)
    uint32_t format;                        // Pixel format (PixelFormat enum)
    
    // Statistics (atomic for thread-safe access)
    std::atomic<uint64_t> frame_counter;    // Total frames written since startup
    std::atomic<uint64_t> dropped_frames;   // Frames dropped by producer
    
    // Timing info (for latency measurement)
    std::atomic<uint64_t> last_write_timestamp_ns;  // Nanosecond timestamp of last write
    
    // Control flags (for bidirectional signaling)
    std::atomic<uint8_t> pause_requested;   // Consumer requests producer to pause (for settings changes)
    std::atomic<uint8_t> producer_paused;   // Producer confirms it has paused
    
    // Reserved for future use (padding to 64 bytes)
    uint8_t reserved[6];
    
    // === Frame Data (23.7 MB) ===
    
    // Triple-buffered frame data
    // Buffer 0: Producer writing OR ready for consumer
    // Buffer 1: Consumer reading OR ready for producer
    // Buffer 2: Ready for next operation
    unsigned char frames[NUM_BUFFERS][FRAME_SIZE];
    
};

// Apply alignment to the struct (MSVC requires it before the struct)
#ifdef _MSC_VER
// For MSVC, we use #pragma pack or ensure proper alignment in usage
// The struct is naturally cache-aligned due to atomics
#else
// For GCC/Clang, use the attribute syntax
typedef struct SharedFrameBuffer SharedFrameBufferAligned __attribute__((aligned(64)));
#endif

// Calculate total shared memory size
static constexpr size_t SHARED_BUFFER_SIZE = sizeof(SharedFrameBuffer);

/**
 * Helper functions for buffer management
 */
namespace StreamLumo {
    
    /**
     * Get next buffer index in circular fashion
     */
    inline uint64_t nextBufferIndex(uint64_t current) {
        return (current + 1) % NUM_BUFFERS;
    }
    
    /**
     * Calculate distance between write and read indices
     * Returns number of buffers between them
     */
    inline int bufferDistance(uint64_t writeIdx, uint64_t readIdx) {
        return (writeIdx - readIdx + NUM_BUFFERS) % NUM_BUFFERS;
    }
    
    /**
     * Check if consumer is falling behind
     * Returns true if more than 1 frame behind (should drop frames)
     */
    inline bool shouldDropFrames(uint64_t writeIdx, uint64_t readIdx) {
        return bufferDistance(writeIdx, readIdx) > 1;
    }
    
    /**
     * Get latest available frame index (for low-latency mode)
     */
    inline uint64_t getLatestFrameIndex(uint64_t writeIdx) {
        return (writeIdx - 1 + NUM_BUFFERS) % NUM_BUFFERS;
    }
}

#endif // STREAMLUMO_SHARED_BUFFER_H
