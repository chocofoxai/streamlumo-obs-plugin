cmake_minimum_required(VERSION 3.16)
project(streamlumo-obs-plugin VERSION 1.0.0)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set OBS headers path (from cloned repo)
set(OBS_INCLUDE_DIR "/tmp/obs-studio-headers/libobs")
set(OBS_FRONTEND_API_DIR "/tmp/obs-studio-headers/frontend/api")

# Verify OBS headers exist
if(NOT EXISTS "${OBS_INCLUDE_DIR}/obs.h")
  message(FATAL_ERROR "OBS headers not found at ${OBS_INCLUDE_DIR}")
endif()

# Find libobs
find_package(libobs QUIET)
if(NOT libobs_FOUND)
  message(STATUS "libobs package config not found, using manual configuration")
  # Manual configuration for OBS
  set(LIBOBS_INCLUDE_DIRS "${OBS_INCLUDE_DIR}")
  set(LIBOBS_LIBRARIES "obs")
endif()

# Add SIMDE headers include path
set(SIMDE_INCLUDE_DIR "/opt/homebrew/include")

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

# Include directories
target_include_directories(streamlumo-plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LIBOBS_INCLUDE_DIRS}
    ${OBS_FRONTEND_API_DIR}
    ${SIMDE_INCLUDE_DIR}
    /tmp/obs-studio-headers
)

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
