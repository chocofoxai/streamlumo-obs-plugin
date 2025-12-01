/**
 * StreamLumo Win32 Shared Memory Header
 * 
 * @license MIT
 */

#ifndef STREAMLUMO_SHM_WIN32_H
#define STREAMLUMO_SHM_WIN32_H

#ifdef _WIN32

#include <cstdint>
#include <cstddef>

// Forward declaration
struct SharedFrameBuffer;

namespace StreamLumo {

/**
 * Frame metadata structure
 */
struct FrameMetadata {
    uint32_t width;
    uint32_t height;
    uint32_t frameSize;
    uint32_t format;
    uint64_t frameCounter;
    uint64_t droppedFrames;
    uint64_t lastWriteTimestampNs;
};

/**
 * Win32 Shared Memory API
 */
class ShmWin32 {
public:
    // Create shared memory (producer side - OBS plugin)
    static bool create();
    
    // Connect to existing shared memory (consumer side - Electron)
    static bool connect();
    
    // Disconnect from shared memory
    static void disconnect();
    
    // Destroy shared memory (cleanup)
    static void destroy();
    
    // Write frame to shared memory (producer)
    static bool writeFrame(const unsigned char* frameData, size_t dataSize);
    
    // Read latest frame from shared memory (consumer)
    static bool readFrame(unsigned char* buffer, size_t bufferSize);
    
    // Wait for new frame (optional, uses semaphore)
    static bool waitForFrame(int timeoutMs = -1);
    
    // Get frame metadata
    static bool getMetadata(FrameMetadata& metadata);
    
    // Check if connected
    static bool isConnected();
    
    // Get direct access to shared buffer (for control flags)
    static SharedFrameBuffer* getBuffer();
};

} // namespace StreamLumo

#endif // _WIN32
#endif // STREAMLUMO_SHM_WIN32_H
