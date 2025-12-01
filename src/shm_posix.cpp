/**
 * StreamLumo POSIX Shared Memory Implementation
 * 
 * Implements shared memory operations for macOS and Linux using POSIX API.
 * This file is part of the proprietary Electron application (not GPL).
 * 
 * @license MIT
 */

#include "shm_posix.h"
#include "../../native-shm/include/shared_buffer.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <chrono>

namespace StreamLumo {

ShmPosix::ShmPosix(const std::string& channelName) 
    : m_channelName(channelName), m_shm_fd(-1), m_shm_ptr(nullptr), m_sem(nullptr) {
    
    // Construct names based on channel
    // e.g. "/streamlumo_frames_program"
    m_shmName = std::string(SHM_NAME) + "_" + channelName;
    m_semName = std::string(SEM_NAME) + "_" + channelName;
}

ShmPosix::~ShmPosix() {
    disconnect();
}

/**
 * Create or open shared memory region
 */
bool ShmPosix::create() {
    // Open/create shared memory object
    m_shm_fd = shm_open(m_shmName.c_str(), O_CREAT | O_RDWR, 0666);
    if (m_shm_fd == -1) {
        std::cerr << "[ShmPosix] Failed to create shared memory (" << m_shmName << "): " << strerror(errno) << std::endl;
        return false;
    }
    
    // Set size of shared memory
    if (ftruncate(m_shm_fd, SHARED_BUFFER_SIZE) == -1) {
        std::cerr << "[ShmPosix] Failed to set shared memory size: " << strerror(errno) << std::endl;
        close(m_shm_fd);
        m_shm_fd = -1;
        return false;
    }
    
    // Map shared memory to process address space
    void* ptr = mmap(nullptr, SHARED_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m_shm_fd, 0);
    if (ptr == MAP_FAILED) {
        std::cerr << "[ShmPosix] Failed to map shared memory: " << strerror(errno) << std::endl;
        close(m_shm_fd);
        m_shm_fd = -1;
        return false;
    }
    
    m_shm_ptr = static_cast<SharedFrameBuffer*>(ptr);
    
    // Initialize metadata (only if we're the first to create it)
    // Use atomic flag to check if already initialized
    uint64_t expected = 0;
    if (m_shm_ptr->frame_counter.compare_exchange_strong(expected, 0)) {
        // We're the first - initialize the structure
        m_shm_ptr->write_index.store(0, std::memory_order_release);
        m_shm_ptr->read_index.store(0, std::memory_order_release);
        m_shm_ptr->width = FRAME_WIDTH;
        m_shm_ptr->height = FRAME_HEIGHT;
        m_shm_ptr->frame_size = FRAME_SIZE;
        m_shm_ptr->format = FORMAT_RGBA;
        m_shm_ptr->frame_counter.store(0, std::memory_order_release);
        m_shm_ptr->dropped_frames.store(0, std::memory_order_release);
        m_shm_ptr->last_write_timestamp_ns = 0;
        std::memset(m_shm_ptr->reserved, 0, sizeof(m_shm_ptr->reserved));
        
        std::cout << "[ShmPosix] Initialized shared memory structure for " << m_channelName << std::endl;
    }
    
    // Open/create semaphore for signaling
    m_sem = sem_open(m_semName.c_str(), O_CREAT, 0666, 0);
    if (m_sem == SEM_FAILED) {
        std::cerr << "[ShmPosix] Failed to create semaphore: " << strerror(errno) << std::endl;
        // Continue anyway - semaphore is optional (can use polling)
        m_sem = nullptr;
    }
    
    std::cout << "[ShmPosix] Shared memory created successfully (" 
              << (SHARED_BUFFER_SIZE / 1024 / 1024) << " MB) for " << m_channelName << std::endl;
    
    return true;
}

/**
 * Connect to existing shared memory region
 */
bool ShmPosix::connect() {
    // Open existing shared memory object (don't create)
    m_shm_fd = shm_open(m_shmName.c_str(), O_RDWR, 0666);
    if (m_shm_fd == -1) {
        // Silent fail is okay for connect - it might not exist yet
        return false;
    }
    
    // Map shared memory to process address space
    void* ptr = mmap(nullptr, SHARED_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m_shm_fd, 0);
    if (ptr == MAP_FAILED) {
        std::cerr << "[ShmPosix] Failed to map shared memory: " << strerror(errno) << std::endl;
        close(m_shm_fd);
        m_shm_fd = -1;
        return false;
    }
    
    m_shm_ptr = static_cast<SharedFrameBuffer*>(ptr);
    
    // Open existing semaphore
    m_sem = sem_open(m_semName.c_str(), 0);
    if (m_sem == SEM_FAILED) {
        // Semaphore might not exist yet or permission denied
        m_sem = nullptr;
    }
    
    return true;
}

/**
 * Disconnect from shared memory
 */
void ShmPosix::disconnect() {
    if (m_shm_ptr) {
        munmap(m_shm_ptr, SHARED_BUFFER_SIZE);
        m_shm_ptr = nullptr;
    }
    
    if (m_shm_fd != -1) {
        close(m_shm_fd);
        m_shm_fd = -1;
    }
    
    if (m_sem) {
        sem_close(m_sem);
        m_sem = nullptr;
    }
}

/**
 * Destroy shared memory (cleanup)
 */
void ShmPosix::destroy() {
    disconnect();
    shm_unlink(m_shmName.c_str());
    sem_unlink(m_semName.c_str());
}

/**
 * Check if connected
 */
bool ShmPosix::isConnected() const {
    return m_shm_ptr != nullptr;
}

/**
 * Write frame to shared memory (producer)
 */
bool ShmPosix::writeFrame(const unsigned char* frameData, size_t dataSize) {
    if (!m_shm_ptr) return false;
    if (dataSize > FRAME_SIZE) return false;
    
    // Get next write index (triple buffering)
    int currentWriteIndex = m_shm_ptr->write_index.load(std::memory_order_acquire);
    int nextWriteIndex = (currentWriteIndex + 1) % NUM_BUFFERS;
    
    // If next write index is being read, we drop the frame (or wait, but dropping is better for latency)
    int currentReadIndex = m_shm_ptr->read_index.load(std::memory_order_acquire);
    if (nextWriteIndex == currentReadIndex) {
        m_shm_ptr->dropped_frames.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    
    // Copy data to shared buffer
    unsigned char* dest = m_shm_ptr->frames[nextWriteIndex];
    std::memcpy(dest, frameData, dataSize);
    
    // Update write index
    m_shm_ptr->write_index.store(nextWriteIndex, std::memory_order_release);
    
    // Update counters
    m_shm_ptr->frame_counter.fetch_add(1, std::memory_order_relaxed);
    
    // Signal semaphore
    if (m_sem) {
        sem_post(m_sem);
    }
    
    return true;
}

/**
 * Read latest frame from shared memory (consumer)
 */
bool ShmPosix::readFrame(unsigned char* buffer, size_t bufferSize) {
    if (!m_shm_ptr) return false;
    if (bufferSize < FRAME_SIZE) return false;
    
    // Get latest write index
    int currentWriteIndex = m_shm_ptr->write_index.load(std::memory_order_acquire);
    int currentReadIndex = m_shm_ptr->read_index.load(std::memory_order_relaxed);
    
    // If nothing new to read
    if (currentWriteIndex == currentReadIndex) {
        return false;
    }
    
    // Copy data from shared buffer
    const unsigned char* src = m_shm_ptr->frames[currentWriteIndex];
    std::memcpy(buffer, src, FRAME_SIZE);
    
    // Update read index
    m_shm_ptr->read_index.store(currentWriteIndex, std::memory_order_release);
    
    return true;
}

/**
 * Wait for new frame
 */
bool ShmPosix::waitForFrame(int timeoutMs) {
    if (!m_sem) return false;
    
    if (timeoutMs < 0) {
        return sem_wait(m_sem) == 0;
    } else {
        // sem_timedwait not available on macOS, use polling or trywait
        // For now, just trywait
        return sem_trywait(m_sem) == 0;
    }
}

/**
 * Get frame metadata
 */
bool ShmPosix::getMetadata(FrameMetadata& metadata) {
    if (!m_shm_ptr) return false;
    
    metadata.width = m_shm_ptr->width;
    metadata.height = m_shm_ptr->height;
    metadata.frameSize = m_shm_ptr->frame_size;
    metadata.format = m_shm_ptr->format;
    metadata.frameCounter = m_shm_ptr->frame_counter.load(std::memory_order_relaxed);
    metadata.droppedFrames = m_shm_ptr->dropped_frames.load(std::memory_order_relaxed);
    metadata.lastWriteTimestampNs = m_shm_ptr->last_write_timestamp_ns;
    
    return true;
}

} // namespace StreamLumo
