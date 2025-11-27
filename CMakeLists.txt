cmake_minimum_required(VERSION 3.10)
project(simple-rdma C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build type defaults to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2")

# Find libibverbs
find_library(IBVERBS_LIB ibverbs
    PATHS
        /usr/lib
        /usr/local/lib
        /usr/lib/x86_64-linux-gnu
    REQUIRED
)

# Find infiniband headers
find_path(IBVERBS_INCLUDE_DIR infiniband/verbs.h
    PATHS
        /usr/include
        /usr/local/include
    REQUIRED
)

# Include directories
include_directories(${IBVERBS_INCLUDE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# Common source files
set(COMMON_SOURCES
    src/rdma_common.c
)

set(COMMON_HEADERS
    src/rdma_common.h
    src/devinfo.h
)

# Create a library for common code
add_library(rdma_common STATIC ${COMMON_SOURCES} ${COMMON_HEADERS})

# Sender UC executable (UC - Unreliable Connection)
add_executable(sender_uc
    src/sender_uc.c
)

target_link_libraries(sender_uc
    rdma_common
    ${IBVERBS_LIB}
)

# Receiver UC executable (UC - Unreliable Connection)
add_executable(receiver_uc
    src/receiver_uc.c
)

target_link_libraries(receiver_uc
    rdma_common
    ${IBVERBS_LIB}
)

# Receiver RC executable (RC - Reliable Connection)
add_executable(receiver_rc
    src/reciever_rc.c
)

target_link_libraries(receiver_rc
    rdma_common
    ${IBVERBS_LIB}
)

# Sender RC executable (RC - Reliable Connection)
add_executable(sender_rc
    src/sender_rc.c
)

target_link_libraries(sender_rc
    rdma_common
    ${IBVERBS_LIB}
)

# Installation (optional)
install(TARGETS sender_uc receiver_uc sender_rc receiver_rc
    RUNTIME DESTINATION bin
)

# Print configuration summary
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "IBVERBS library: ${IBVERBS_LIB}")
message(STATUS "IBVERBS include: ${IBVERBS_INCLUDE_DIR}")

