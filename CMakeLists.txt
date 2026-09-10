cmake_minimum_required(VERSION 3.24)

project(dlss5_mgpu LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

find_package(CUDAToolkit REQUIRED)

set(VULKAN_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan-sdk/root"
    CACHE PATH "Local Vulkan SDK root used when system headers are unavailable")
find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h
    PATHS "${VULKAN_SDK_ROOT}/usr/include" /usr/include
    REQUIRED)
find_file(VULKAN_LIBRARY libvulkan.so.1
    PATHS "${VULKAN_SDK_ROOT}/usr/lib/x86_64-linux-gnu"
          /usr/lib/x86_64-linux-gnu
          /lib/x86_64-linux-gnu
    REQUIRED)

add_library(mgpu_p2p STATIC
    src/p2p_transport.cu
)

target_include_directories(mgpu_p2p
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(mgpu_p2p PUBLIC CUDA::cudart)
set_target_properties(mgpu_p2p PROPERTIES
    CUDA_ARCHITECTURES 86
    POSITION_INDEPENDENT_CODE ON
)

add_executable(mgpu-p2p-probe
    src/p2p_probe.cu
)

target_link_libraries(mgpu-p2p-probe PRIVATE mgpu_p2p CUDA::cudart)
set_target_properties(mgpu-p2p-probe PROPERTIES CUDA_ARCHITECTURES 86)

add_executable(mgpu-p2p-ring-probe
    src/p2p_ring_probe.cu
)
target_link_libraries(mgpu-p2p-ring-probe PRIVATE mgpu_p2p CUDA::cudart)
set_target_properties(mgpu-p2p-ring-probe PROPERTIES CUDA_ARCHITECTURES 86)

add_executable(mgpu-cpu-sync-p2p-probe
    src/cpu_sync_p2p_probe.cu
)
target_link_libraries(mgpu-cpu-sync-p2p-probe PRIVATE mgpu_p2p CUDA::cudart)
set_target_properties(mgpu-cpu-sync-p2p-probe PROPERTIES CUDA_ARCHITECTURES 86)

add_executable(mgpu-cpu-sync-frame-probe
    src/cpu_sync_frame_probe.cu
)
target_link_libraries(mgpu-cpu-sync-frame-probe PRIVATE mgpu_p2p CUDA::cudart)
set_target_properties(mgpu-cpu-sync-frame-probe PROPERTIES CUDA_ARCHITECTURES 86)

add_executable(mgpu-vulkan-cuda-probe
    src/vulkan_cuda_probe.cpp
)

target_include_directories(mgpu-vulkan-cuda-probe PRIVATE
    ${VULKAN_INCLUDE_DIR}
)
target_link_libraries(mgpu-vulkan-cuda-probe PRIVATE
    CUDA::cudart
    ${VULKAN_LIBRARY}
)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mgpu_p2p PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mgpu-p2p-probe PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mgpu-p2p-ring-probe PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mgpu-cpu-sync-p2p-probe PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mgpu-cpu-sync-frame-probe PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mgpu-vulkan-cuda-probe PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS mgpu-p2p-probe mgpu-p2p-ring-probe mgpu-cpu-sync-p2p-probe
                mgpu-cpu-sync-frame-probe mgpu-vulkan-cuda-probe
        RUNTIME DESTINATION bin)
