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
#include <iostream>
#include <cstring>
#include <chrono>

namespace StreamLumo {

ShmWin32::ShmWin32(const std::string& channelName)
    : m_channelName(channelName)
    , m_hMapFile(NULL)
    , m_shm_ptr(nullptr)
    , m_hSemaphore(NULL)
{
    // Generate unique names based on channel
    m_shmName = std::string("Local\\StreamLumo_") + channelName;
    m_semName = std::string("StreamLumoSem_") + channelName;
}

ShmWin32::~ShmWin32()
{
    disconnect();
}

/**
 * Create or open shared memory region
 */
bool ShmWin32::create() {
    // Create file mapping object
    m_hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE,    // Use paging file
        NULL,                     // Default security
        PAGE_READWRITE,           // Read/write access
        0,                        // High-order DWORD of size
        static_cast<DWORD>(SHARED_BUFFER_SIZE),  // Low-order DWORD of size
        m_shmName.c_str()         // Name of mapping object
    );
    
    if (m_hMapFile == NULL) {
        std::cerr << "[ShmWin32] Failed to create file mapping: " << GetLastError() << std::endl;
        return false;
    }
    
    bool isFirstCreate = (GetLastError() != ERROR_ALREADY_EXISTS);
    
    // Map view of file
    void* ptr = MapViewOfFile(
        m_hMapFile,               // Handle to mapping object
        FILE_MAP_ALL_ACCESS,      // Read/write access
        0,                        // High-order DWORD of offset
        0,                        // Low-order DWORD of offset
        SHARED_BUFFER_SIZE        // Number of bytes to map
    );
    
    if (ptr == NULL) {
        std::cerr << "[ShmWin32] Failed to map view of file: " << GetLastError() << std::endl;
        CloseHandle(m_hMapFile);
        m_hMapFile = NULL;
        return false;
    }
    
    m_shm_ptr = static_cast<SharedFrameBuffer*>(ptr);
    
    // Initialize metadata if we're the first
    if (isFirstCreate) {
        m_shm_ptr->write_index.store(0, std::memory_order_release);
        m_shm_ptr->read_index.store(0, std::memory_order_release);
        m_shm_ptr->width = FRAME_WIDTH;
        m_shm_ptr->height = FRAME_HEIGHT;
        m_shm_ptr->frame_size = FRAME_SIZE;
        m_shm_ptr->format = FORMAT_RGBA;
        m_shm_ptr->frame_counter.store(0, std::memory_order_release);
        m_shm_ptr->dropped_frames.store(0, std::memory_order_release);
        m_shm_ptr->last_write_timestamp_ns.store(0, std::memory_order_release);
        m_shm_ptr->pause_requested.store(0, std::memory_order_release);
        m_shm_ptr->producer_paused.store(0, std::memory_order_release);
        std::memset(m_shm_ptr->reserved, 0, sizeof(m_shm_ptr->reserved));
        
        std::cout << "[ShmWin32] Initialized shared memory structure" << std::endl;
    }
    
    // Create semaphore for signaling
    m_hSemaphore = CreateSemaphoreA(
        NULL,           // Default security
        0,              // Initial count
        LONG_MAX,       // Maximum count
        m_semName.c_str()
    );
    
    if (m_hSemaphore == NULL) {
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
    m_hMapFile = OpenFileMappingA(
        FILE_MAP_ALL_ACCESS,   // Read/write access
        FALSE,                 // Don't inherit handle
        m_shmName.c_str()      // Name of mapping object
    );
    
    if (m_hMapFile == NULL) {
        // Try to create if it doesn't exist (producer mode)
        return create();
    }
    
    // Map view of file
    void* ptr = MapViewOfFile(
        m_hMapFile,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        SHARED_BUFFER_SIZE
    );
    
    if (ptr == NULL) {
        std::cerr << "[ShmWin32] Failed to map view of file: " << GetLastError() << std::endl;
        CloseHandle(m_hMapFile);
        m_hMapFile = NULL;
        return false;
    }
    
    m_shm_ptr = static_cast<SharedFrameBuffer*>(ptr);
    
    // Open existing semaphore
    m_hSemaphore = OpenSemaphoreA(
        SEMAPHORE_ALL_ACCESS,
        FALSE,
        m_semName.c_str()
    );
    
    if (m_hSemaphore == NULL) {
        // Semaphore not required - can use polling
    }
    
    std::cout << "[ShmWin32] Connected to shared memory successfully" << std::endl;
    std::cout << "[ShmWin32] Resolution: " << m_shm_ptr->width << "x" << m_shm_ptr->height << std::endl;
    std::cout << "[ShmWin32] Frame size: " << m_shm_ptr->frame_size << " bytes" << std::endl;
    
    return true;
}

/**
 * Disconnect from shared memory
 */
void ShmWin32::disconnect() {
    if (m_shm_ptr != nullptr) {
        UnmapViewOfFile(m_shm_ptr);
        m_shm_ptr = nullptr;
    }
    
    if (m_hMapFile != NULL) {
        CloseHandle(m_hMapFile);
        m_hMapFile = NULL;
    }
    
    if (m_hSemaphore != NULL) {
        CloseHandle(m_hSemaphore);
        m_hSemaphore = NULL;
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
    if (m_shm_ptr == nullptr) {
        std::cerr << "[ShmWin32] Not connected to shared memory" << std::endl;
        return false;
    }
    
    if (dataSize != FRAME_SIZE) {
        std::cerr << "[ShmWin32] Invalid frame size: " << dataSize << " (expected " << FRAME_SIZE << ")" << std::endl;
        return false;
    }
    
    // Get current write index
    uint64_t currentWrite = m_shm_ptr->write_index.load(std::memory_order_acquire);
    uint64_t currentRead = m_shm_ptr->read_index.load(std::memory_order_acquire);
    
    // Check if we're overwriting unread frames
    if (shouldDropFrames(currentWrite, currentRead)) {
        m_shm_ptr->dropped_frames.fetch_add(1, std::memory_order_relaxed);
    }
    
    // Copy frame data to current write buffer
    std::memcpy(m_shm_ptr->frames[currentWrite], frameData, dataSize);
    
    // Update timestamp
    auto now = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch());
    m_shm_ptr->last_write_timestamp_ns.store(static_cast<uint64_t>(ns.count()), std::memory_order_release);
    
    // Advance write index
    uint64_t nextWrite = nextBufferIndex(currentWrite);
    m_shm_ptr->write_index.store(nextWrite, std::memory_order_release);
    
    // Increment frame counter
    m_shm_ptr->frame_counter.fetch_add(1, std::memory_order_relaxed);
    
    // Signal semaphore if available
    if (m_hSemaphore != NULL) {
        ReleaseSemaphore(m_hSemaphore, 1, NULL);
    }
    
    return true;
}

/**
 * Read latest frame from shared memory (consumer)
 */
bool ShmWin32::readFrame(unsigned char* buffer, size_t bufferSize) {
    if (m_shm_ptr == nullptr) {
        std::cerr << "[ShmWin32] Not connected to shared memory" << std::endl;
        return false;
    }
    
    if (bufferSize < FRAME_SIZE) {
        std::cerr << "[ShmWin32] Buffer too small: " << bufferSize << " (need " << FRAME_SIZE << ")" << std::endl;
        return false;
    }
    
    // Get current indices
    uint64_t currentWrite = m_shm_ptr->write_index.load(std::memory_order_acquire);
    uint64_t currentRead = m_shm_ptr->read_index.load(std::memory_order_acquire);
    
    // Check if there's a new frame available
    if (currentWrite == currentRead) {
        return false;
    }
    
    // Read latest frame
    uint64_t readIdx = getLatestFrameIndex(currentWrite);
    
    // Copy frame data
    std::memcpy(buffer, m_shm_ptr->frames[readIdx], FRAME_SIZE);
    
    // Update read index
    m_shm_ptr->read_index.store(currentWrite, std::memory_order_release);
    
    return true;
}

/**
 * Wait for new frame with timeout
 */
bool ShmWin32::waitForFrame(int timeoutMs) {
    if (m_hSemaphore == NULL) {
        return false;
    }
    
    DWORD timeout = (timeoutMs < 0) ? INFINITE : static_cast<DWORD>(timeoutMs);
    DWORD result = WaitForSingleObject(m_hSemaphore, timeout);
    
    return (result == WAIT_OBJECT_0);
}

/**
 * Get frame metadata
 */
bool ShmWin32::getMetadata(FrameMetadata& metadata) {
    if (m_shm_ptr == nullptr) {
        return false;
    }
    
    metadata.width = m_shm_ptr->width;
    metadata.height = m_shm_ptr->height;
    metadata.frameSize = m_shm_ptr->frame_size;
    metadata.format = m_shm_ptr->format;
    metadata.frameCounter = m_shm_ptr->frame_counter.load(std::memory_order_relaxed);
    metadata.droppedFrames = m_shm_ptr->dropped_frames.load(std::memory_order_relaxed);
    metadata.lastWriteTimestampNs = m_shm_ptr->last_write_timestamp_ns.load(std::memory_order_relaxed);
    
    return true;
}

/**
 * Check if shared memory is connected
 */
bool ShmWin32::isConnected() const {
    return m_shm_ptr != nullptr;
}

} // namespace StreamLumo

#endif // _WIN32
