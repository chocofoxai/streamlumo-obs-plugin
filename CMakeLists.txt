cmake_minimum_required(VERSION 3.16)
project(streamlumo-obs-plugin VERSION 1.0.0)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================================================================
# OBS Headers Configuration
# Priority: CMake variable > Environment variable > Default path
# =============================================================================
if(NOT DEFINED OBS_INCLUDE_DIR)
  if(DEFINED ENV{OBS_HEADERS_DIR})
    set(OBS_INCLUDE_DIR "$ENV{OBS_HEADERS_DIR}/libobs")
    set(OBS_FRONTEND_API_DIR "$ENV{OBS_HEADERS_DIR}/frontend/api")
    set(OBS_ROOT_DIR "$ENV{OBS_HEADERS_DIR}")
  elseif(EXISTS "/tmp/obs-studio-headers/libobs")
    set(OBS_INCLUDE_DIR "/tmp/obs-studio-headers/libobs")
    set(OBS_FRONTEND_API_DIR "/tmp/obs-studio-headers/frontend/api")
    set(OBS_ROOT_DIR "/tmp/obs-studio-headers")
  else()
    # Default fallback
    set(OBS_INCLUDE_DIR "/tmp/obs-studio-headers/libobs")
    set(OBS_FRONTEND_API_DIR "/tmp/obs-studio-headers/frontend/api")
    set(OBS_ROOT_DIR "/tmp/obs-studio-headers")
  endif()
else()
  # OBS_INCLUDE_DIR was passed via -D, derive other paths
  get_filename_component(OBS_ROOT_DIR "${OBS_INCLUDE_DIR}" DIRECTORY)
  set(OBS_FRONTEND_API_DIR "${OBS_ROOT_DIR}/frontend/api")
endif()

# Verify OBS headers exist (warning only for CI flexibility)
if(EXISTS "${OBS_INCLUDE_DIR}/obs.h")
  message(STATUS "Found OBS headers at: ${OBS_INCLUDE_DIR}")
else()
  message(WARNING "OBS headers not found at ${OBS_INCLUDE_DIR}/obs.h - build may fail")
endif()

# Find libobs
find_package(libobs QUIET)
if(NOT libobs_FOUND)
  message(STATUS "libobs package config not found, using manual configuration")
  set(LIBOBS_INCLUDE_DIRS "${OBS_INCLUDE_DIR}")
  set(LIBOBS_LIBRARIES "obs")
endif()

# =============================================================================
# SIMDE Headers Configuration (optional - for SIMD optimizations)
# Priority: CMake variable > Environment variable > Auto-detect
# =============================================================================
if(NOT DEFINED SIMDE_INCLUDE_DIR)
  if(DEFINED ENV{SIMDE_INCLUDE_DIR})
    set(SIMDE_INCLUDE_DIR "$ENV{SIMDE_INCLUDE_DIR}")
  elseif(EXISTS "/opt/homebrew/include/simde")
    # macOS ARM (Apple Silicon)
    set(SIMDE_INCLUDE_DIR "/opt/homebrew/include")
  elseif(EXISTS "/usr/local/include/simde")
    # macOS Intel / Homebrew on Intel
    set(SIMDE_INCLUDE_DIR "/usr/local/include")
  elseif(EXISTS "/usr/include/simde")
    # Linux
    set(SIMDE_INCLUDE_DIR "/usr/include")
  else()
    # SIMDE not found - still allow build without SIMD
    set(SIMDE_INCLUDE_DIR "")
    message(STATUS "SIMDE not found - building without SIMD optimizations")
  endif()
endif()

# Source files
set(PLUGIN_SOURCES
    src/plugin_main.cpp
    src/frame_writer.cpp
)

# Platform-specific source files and libraries
if(WIN32)
    list(APPEND PLUGIN_SOURCES src/shm_win32.cpp)
    set(PLATFORM_LIBS "")
elseif(APPLE)
    list(APPEND PLUGIN_SOURCES src/shm_posix.cpp)
    set(PLATFORM_LIBS pthread)
else()
    # Linux
    list(APPEND PLUGIN_SOURCES src/shm_posix.cpp)
    set(PLATFORM_LIBS pthread rt)
endif()

# Create plugin library
add_library(streamlumo-plugin MODULE ${PLUGIN_SOURCES})

# RPATH settings for macOS
if(APPLE)
    set(CMAKE_MACOSX_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH "@loader_path/../../../../Frameworks")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    set_target_properties(streamlumo-plugin PROPERTIES
        BUILD_RPATH "/Applications/OBS.app/Contents/Frameworks"
    )
endif()

# Include directories - build list dynamically
set(PLUGIN_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LIBOBS_INCLUDE_DIRS}
    ${OBS_FRONTEND_API_DIR}
    ${OBS_ROOT_DIR}
)

# Add SIMDE if found
if(NOT "${SIMDE_INCLUDE_DIR}" STREQUAL "")
    list(APPEND PLUGIN_INCLUDE_DIRS ${SIMDE_INCLUDE_DIR})
endif()

target_include_directories(streamlumo-plugin PRIVATE ${PLUGIN_INCLUDE_DIRS})

# Link libraries - Framework for macOS
if(APPLE)
    target_link_libraries(streamlumo-plugin
        ${PLATFORM_LIBS}
    )
else()
    target_link_libraries(streamlumo-plugin
        ${LIBOBS_LIBRARIES}
        ${PLATFORM_LIBS}
    )
endif()

# Set output properties
set_target_properties(streamlumo-plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "streamlumo-plugin"
    SUFFIX ".so"
)

if(APPLE)
    set_target_properties(streamlumo-plugin PROPERTIES
        LINK_FLAGS "-undefined dynamic_lookup"
    )
endif()

# Installation
if(APPLE)
    set(OBS_PLUGIN_DESTINATION "$ENV{HOME}/Library/Application Support/obs-studio/plugins/streamlumo-plugin/bin")
elseif(WIN32)
    set(OBS_PLUGIN_DESTINATION "C:/Program Files/obs-studio/obs-plugins/64bit")
else()
    set(OBS_PLUGIN_DESTINATION "$ENV{HOME}/.config/obs-studio/plugins/streamlumo-plugin/bin/64bit")
endif()

install(TARGETS streamlumo-plugin
    LIBRARY DESTINATION ${OBS_PLUGIN_DESTINATION}
)

# Debug output
message(STATUS "StreamLumo OBS Plugin Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Install destination: ${OBS_PLUGIN_DESTINATION}")
message(STATUS "  Platform libraries: ${PLATFORM_LIBS}")
message(STATUS "  SIMDE include: ${SIMDE_INCLUDE_DIR}")
message(STATUS "  OBS Frontend API: ${OBS_FRONTEND_API_DIR}")
