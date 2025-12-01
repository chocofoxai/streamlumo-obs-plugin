/**
 * StreamLumo Win32 Shared Memory Implementation
 * 
 * Implements shared memory operations for Windows using Win32 API.
 * This file is part of the proprietary Electron application (not GPL).
 * 
 * @license MIT
 */

#ifdef _WIN32

#include "shm_win32.h"
#include "../include/shared_buffer.h"
#include <windows.h>
#include <iostream>
#include <cstring>
#include <chrono>

namespace StreamLumo {

static SharedFrameBuffer* g_shm_ptr = nullptr;
static HANDLE g_hMapFile = NULL;
static HANDLE g_hSemaphore = NULL;

/**
 * Create or open shared memory region
 */
bool ShmWin32::create() {
    // Create file mapping object
    g_hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE,    // Use paging file
        NULL,                     // Default security
        PAGE_READWRITE,           // Read/write access
        0,                        // High-order DWORD of size
        SHARED_BUFFER_SIZE,       // Low-order DWORD of size
        SHM_NAME_WIN32            // Name of mapping object
    );
    
    if (g_hMapFile == NULL) {
        std::cerr << "[ShmWin32] Failed to create file mapping: " << GetLastError() << std::endl;
        return false;
    }
    
    bool isFirstCreate = (GetLastError() != ERROR_ALREADY_EXISTS);
    
    // Map view of file
    void* ptr = MapViewOfFile(
        g_hMapFile,               // Handle to mapping object
        FILE_MAP_ALL_ACCESS,      // Read/write access
        0,                        // High-order DWORD of offset
        0,                        // Low-order DWORD of offset
        SHARED_BUFFER_SIZE        // Number of bytes to map
    );
    
    if (ptr == NULL) {
        std::cerr << "[ShmWin32] Failed to map view of file: " << GetLastError() << std::endl;
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
        return false;
    }
    
    g_shm_ptr = static_cast<SharedFrameBuffer*>(ptr);
    
    // Initialize metadata if we're the first
    if (isFirstCreate) {
        g_shm_ptr->write_index.store(0, std::memory_order_release);
        g_shm_ptr->read_index.store(0, std::memory_order_release);
        g_shm_ptr->width = FRAME_WIDTH;
        g_shm_ptr->height = FRAME_HEIGHT;
        g_shm_ptr->frame_size = FRAME_SIZE;
        g_shm_ptr->format = FORMAT_RGBA;
        g_shm_ptr->frame_counter.store(0, std::memory_order_release);
        g_shm_ptr->dropped_frames.store(0, std::memory_order_release);
        g_shm_ptr->last_write_timestamp_ns = 0;
        std::memset(g_shm_ptr->reserved, 0, sizeof(g_shm_ptr->reserved));
        
        std::cout << "[ShmWin32] Initialized shared memory structure" << std::endl;
    }
    
    // Create semaphore for signaling
    g_hSemaphore = CreateSemaphoreA(
        NULL,           // Default security
        0,              // Initial count
        LONG_MAX,       // Maximum count
        "StreamLumoFrameSemaphore"
    );
    
    if (g_hSemaphore == NULL) {
        std::cerr << "[ShmWin32] Failed to create semaphore: " << GetLastError() << std::endl;
        // Continue anyway - semaphore is optional
    }
    
    std::cout << "[ShmWin32] Shared memory created successfully (" 
              << (SHARED_BUFFER_SIZE / 1024 / 1024) << " MB)" << std::endl;
    
    return true;
}

/**
 * Connect to existing shared memory region
 */
bool ShmWin32::connect() {
    // Open existing file mapping object
    g_hMapFile = OpenFileMappingA(
        FILE_MAP_ALL_ACCESS,   // Read/write access
        FALSE,                 // Don't inherit handle
        SHM_NAME_WIN32         // Name of mapping object
    );
    
    if (g_hMapFile == NULL) {
        std::cerr << "[ShmWin32] Failed to open file mapping: " << GetLastError() << std::endl;
        std::cerr << "[ShmWin32] Is OBS running with the StreamLumo plugin?" << std::endl;
        return false;
    }
    
    // Map view of file
    void* ptr = MapViewOfFile(
        g_hMapFile,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        SHARED_BUFFER_SIZE
    );
    
    if (ptr == NULL) {
        std::cerr << "[ShmWin32] Failed to map view of file: " << GetLastError() << std::endl;
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
        return false;
    }
    
    g_shm_ptr = static_cast<SharedFrameBuffer*>(ptr);
    
    // Open existing semaphore
    g_hSemaphore = OpenSemaphoreA(
        SEMAPHORE_ALL_ACCESS,
        FALSE,
        "StreamLumoFrameSemaphore"
    );
    
    if (g_hSemaphore == NULL) {
        // Semaphore not required - can use polling
    }
    
    std::cout << "[ShmWin32] Connected to shared memory successfully" << std::endl;
    std::cout << "[ShmWin32] Resolution: " << g_shm_ptr->width << "x" << g_shm_ptr->height << std::endl;
    std::cout << "[ShmWin32] Frame size: " << g_shm_ptr->frame_size << " bytes" << std::endl;
    
    return true;
}

/**
 * Disconnect from shared memory
 */
void ShmWin32::disconnect() {
    if (g_shm_ptr != nullptr) {
        UnmapViewOfFile(g_shm_ptr);
        g_shm_ptr = nullptr;
    }
    
    if (g_hMapFile != NULL) {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
    }
    
    if (g_hSemaphore != NULL) {
        CloseHandle(g_hSemaphore);
        g_hSemaphore = NULL;
    }
    
    std::cout << "[ShmWin32] Disconnected from shared memory" << std::endl;
}

/**
 * Destroy shared memory (automatic on Windows when all handles closed)
 */
void ShmWin32::destroy() {
    disconnect();
    std::cout << "[ShmWin32] Shared memory destroyed" << std::endl;
}

/**
 * Write frame data to shared memory (producer)
 */
bool ShmWin32::writeFrame(const unsigned char* frameData, size_t dataSize) {
    if (g_shm_ptr == nullptr) {
        std::cerr << "[ShmWin32] Not connected to shared memory" << std::endl;
        return false;
    }
    
    if (dataSize != FRAME_SIZE) {
        std::cerr << "[ShmWin32] Invalid frame size: " << dataSize << " (expected " << FRAME_SIZE << ")" << std::endl;
        return false;
    }
    
    // Get current write index
    uint64_t currentWrite = g_shm_ptr->write_index.load(std::memory_order_acquire);
    uint64_t currentRead = g_shm_ptr->read_index.load(std::memory_order_acquire);
    
    // Check if we're overwriting unread frames
    if (shouldDropFrames(currentWrite, currentRead)) {
        g_shm_ptr->dropped_frames.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[ShmWin32] Warning: Dropping frames (consumer too slow)" << std::endl;
    }
    
    // Copy frame data to current write buffer
    std::memcpy(g_shm_ptr->frames[currentWrite], frameData, dataSize);
    
    // Update timestamp
    auto now = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch());
    g_shm_ptr->last_write_timestamp_ns = ns.count();
    
    // Advance write index
    uint64_t nextWrite = nextBufferIndex(currentWrite);
    g_shm_ptr->write_index.store(nextWrite, std::memory_order_release);
    
    // Increment frame counter
    g_shm_ptr->frame_counter.fetch_add(1, std::memory_order_relaxed);
    
    // Signal semaphore if available
    if (g_hSemaphore != NULL) {
        ReleaseSemaphore(g_hSemaphore, 1, NULL);
    }
    
    return true;
}

/**
 * Read latest frame from shared memory (consumer)
 */
bool ShmWin32::readFrame(unsigned char* buffer, size_t bufferSize) {
    if (g_shm_ptr == nullptr) {
        std::cerr << "[ShmWin32] Not connected to shared memory" << std::endl;
        return false;
    }
    
    if (bufferSize < FRAME_SIZE) {
        std::cerr << "[ShmWin32] Buffer too small: " << bufferSize << " (need " << FRAME_SIZE << ")" << std::endl;
        return false;
    }
    
    // Get current indices
    uint64_t currentWrite = g_shm_ptr->write_index.load(std::memory_order_acquire);
    uint64_t currentRead = g_shm_ptr->read_index.load(std::memory_order_acquire);
    
    // Check if there's a new frame available
    if (currentWrite == currentRead) {
        return false;
    }
    
    // Read latest frame
    uint64_t readIdx = getLatestFrameIndex(currentWrite);
    
    // Copy frame data
    std::memcpy(buffer, g_shm_ptr->frames[readIdx], FRAME_SIZE);
    
    // Update read index
    g_shm_ptr->read_index.store(currentWrite, std::memory_order_release);
    
    return true;
}

/**
 * Wait for new frame with timeout
 */
bool ShmWin32::waitForFrame(int timeoutMs) {
    if (g_hSemaphore == NULL) {
        return false;
    }
    
    DWORD timeout = (timeoutMs < 0) ? INFINITE : timeoutMs;
    DWORD result = WaitForSingleObject(g_hSemaphore, timeout);
    
    return (result == WAIT_OBJECT_0);
}

/**
 * Get frame metadata
 */
bool ShmWin32::getMetadata(FrameMetadata& metadata) {
    if (g_shm_ptr == nullptr) {
        return false;
    }
    
    metadata.width = g_shm_ptr->width;
    metadata.height = g_shm_ptr->height;
    metadata.frameSize = g_shm_ptr->frame_size;
    metadata.format = g_shm_ptr->format;
    metadata.frameCounter = g_shm_ptr->frame_counter.load(std::memory_order_relaxed);
    metadata.droppedFrames = g_shm_ptr->dropped_frames.load(std::memory_order_relaxed);
    metadata.lastWriteTimestampNs = g_shm_ptr->last_write_timestamp_ns;
    
    return true;
}

/**
 * Check if shared memory is connected
 */
bool ShmWin32::isConnected() {
    return g_shm_ptr != nullptr;
}

/**
 * Get direct access to shared buffer (for control flags)
 */
SharedFrameBuffer* ShmWin32::getBuffer() {
    return g_shm_ptr;
}

} // namespace StreamLumo

#endif // _WIN32
