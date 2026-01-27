# PhantomFPGA Userspace Application
#
# Builds the trainee application that communicates with the PhantomFPGA driver.
# This CMakeLists.txt demonstrates proper C project configuration.

cmake_minimum_required(VERSION 3.10)
project(phantomfpga_app
    VERSION 1.0.0
    DESCRIPTION "PhantomFPGA userspace frame receiver application"
    LANGUAGES C
)

# ----------------------------------------------------------------------------
# Build Configuration
# ----------------------------------------------------------------------------

# Require C11 standard (for timespec_get, anonymous structs, etc.)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)  # Enable GNU extensions for poll, mmap, etc.

# Default to Release build if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Enable all warnings in development
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wformat=2 -Wformat-security")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=implicit-function-declaration")

# Debug-specific flags
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -g3 -fsanitize=address,undefined")

# Release-specific flags
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2 -DNDEBUG")

# ----------------------------------------------------------------------------
# PhantomFPGA Application
# ----------------------------------------------------------------------------

add_executable(phantomfpga_app
    phantomfpga_app.c
)

# Include path to driver directory for UAPI header (phantomfpga_uapi.h)
target_include_directories(phantomfpga_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../driver
)

# Link against pthread (for potential future threading)
# Note: Currently not strictly required, but good practice for poll/epoll apps
find_package(Threads REQUIRED)
target_link_libraries(phantomfpga_app PRIVATE Threads::Threads)

# Link against rt for clock_gettime (needed on older glibc)
# On modern Linux, this is in libc, but linking rt doesn't hurt
target_link_libraries(phantomfpga_app PRIVATE rt)

# ----------------------------------------------------------------------------
# Installation
# ----------------------------------------------------------------------------

# Install to standard bin directory when 'make install' is run
install(TARGETS phantomfpga_app
    RUNTIME DESTINATION bin
)

# ----------------------------------------------------------------------------
# Custom Targets
# ----------------------------------------------------------------------------

# Add a 'help' target for trainee guidance
add_custom_target(app-help
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "PhantomFPGA Application Build"
    COMMAND ${CMAKE_COMMAND} -E echo "==============================="
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Build commands:"
    COMMAND ${CMAKE_COMMAND} -E echo "  cmake -B build           # Configure in 'build' directory"
    COMMAND ${CMAKE_COMMAND} -E echo "  cmake --build build      # Build the application"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Debug build:"
    COMMAND ${CMAKE_COMMAND} -E echo "  cmake -B build -DCMAKE_BUILD_TYPE=Debug"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Run the application:"
    COMMAND ${CMAKE_COMMAND} -E echo "  ./build/phantomfpga_app --help"
    COMMAND ${CMAKE_COMMAND} -E echo "  ./build/phantomfpga_app --rate 1000 --size 256"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    VERBATIM
)

# Add a format target if clang-format is available
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i ${CMAKE_CURRENT_SOURCE_DIR}/*.c
        COMMENT "Formatting source files with clang-format"
        VERBATIM
    )
endif()

# ----------------------------------------------------------------------------
# Build Information
# ----------------------------------------------------------------------------

message(STATUS "PhantomFPGA App Configuration:")
message(STATUS "  C Compiler:    ${CMAKE_C_COMPILER}")
message(STATUS "  C Standard:    C${CMAKE_C_STANDARD}")
message(STATUS "  Build Type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  Include Path:  ${CMAKE_CURRENT_SOURCE_DIR}/../driver")
