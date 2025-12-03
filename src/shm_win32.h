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
#include <string>
#include <windows.h>

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
    ShmWin32(const std::string& channelName);
    ~ShmWin32();

    // Create shared memory (producer side - OBS plugin)
    bool create();
    
    // Connect to existing shared memory (consumer side - Electron)
    bool connect();
    
    // Disconnect from shared memory
    void disconnect();
    
    // Destroy shared memory (cleanup)
    void destroy();
    
    // Write frame to shared memory (producer)
    bool writeFrame(const unsigned char* frameData, size_t dataSize);
    
    // Read latest frame from shared memory (consumer)
    bool readFrame(unsigned char* buffer, size_t bufferSize);
    
    // Wait for new frame (optional, uses semaphore)
    bool waitForFrame(int timeoutMs = -1);
    
    // Get frame metadata
    bool getMetadata(FrameMetadata& metadata);
    
    // Check if connected
    bool isConnected() const;
    
    // Get direct access to shared buffer (for control flags)
    SharedFrameBuffer* getBuffer() const { return m_shm_ptr; }

private:
    std::string m_channelName;
    std::string m_shmName;
    std::string m_semName;
    
    HANDLE m_hMapFile;
    SharedFrameBuffer* m_shm_ptr;
    HANDLE m_hSemaphore;
};

} // namespace StreamLumo

#endif // _WIN32
#endif // STREAMLUMO_SHM_WIN32_H
