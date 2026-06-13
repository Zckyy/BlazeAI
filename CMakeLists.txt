cmake_minimum_required(VERSION 3.18)
project(ScreenAnalysisTool LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force MSVC static or dynamic runtime (default is dynamic/MD)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# CUDA setup
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -arch=sm_75") # Support Turing and newer GPUs

# Find dependencies
# OpenCV
find_package(OpenCV REQUIRED)

# CUDA Toolkit
find_package(CUDAToolkit REQUIRED)

# ONNX Runtime (expects user to point CMAKE_PREFIX_PATH or onnxruntime_DIR to the ORT installation folder)
find_package(onnxruntime QUIET)
if(NOT onnxruntime_FOUND)
    # Fallback/Manual configuration if find_package fails
    message(STATUS "ONNX Runtime package not found. Expecting headers in include directory or manually configured path.")
    set(ONNXRUNTIME_INCLUDE_DIRS "" CACHE PATH "Path to ONNX Runtime include folder")
    set(ONNXRUNTIME_LIBRARIES "" CACHE FILEPATH "Path to onnxruntime.lib")
endif()

# ViGEmClient (virtual Xbox 360 controller backend). Not in vcpkg; vendored under
# third_party/vigem the same way as the optional TensorRT DLLs (see that folder's
# README for how to fetch the nefarius.ViGEm.Client NuGet package contents).
set(VIGEM_DIR "${CMAKE_SOURCE_DIR}/third_party/vigem")
if(EXISTS "${VIGEM_DIR}/include/ViGEm/Client.h")
    set(VIGEM_FOUND TRUE)
else()
    set(VIGEM_FOUND FALSE)
    message(STATUS "ViGEmClient SDK not found in third_party/vigem - building without virtual-controller support.")
endif()

# Fetch Dear ImGui
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
)
FetchContent_MakeAvailable(imgui)

# Gather source files
file(GLOB_RECURSE SRC_FILES
    "src/*.h"
    "src/*.cpp"
    "src/*.cu"
)

# ImGui backend files
set(IMGUI_SRC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

add_executable(ScreenAnalysisTool ${SRC_FILES} ${IMGUI_SRC})

# Includes
target_include_directories(ScreenAnalysisTool PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${OpenCV_INCLUDE_DIRS}
)

if(onnxruntime_FOUND)
    target_link_libraries(ScreenAnalysisTool PRIVATE onnxruntime)
else()
    if(ONNXRUNTIME_INCLUDE_DIRS)
        target_include_directories(ScreenAnalysisTool PRIVATE ${ONNXRUNTIME_INCLUDE_DIRS})
    endif()
    if(ONNXRUNTIME_LIBRARIES)
        target_link_libraries(ScreenAnalysisTool PRIVATE ${ONNXRUNTIME_LIBRARIES})
    endif()
endif()

# Automatically copy ONNX Runtime DLLs to the target binary directory on Windows
if(WIN32 AND ONNXRUNTIME_LIBRARIES)
    get_filename_component(ONNXRUNTIME_LIB_DIR "${ONNXRUNTIME_LIBRARIES}" DIRECTORY)
    file(GLOB ONNXRUNTIME_DLLS "${ONNXRUNTIME_LIB_DIR}/*.dll")
    if(ONNXRUNTIME_DLLS)
        add_custom_command(TARGET ScreenAnalysisTool POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ONNXRUNTIME_DLLS}
            $<TARGET_FILE_DIR:ScreenAnalysisTool>
            COMMENT "Copying ONNX Runtime DLLs to output directory"
        )
    endif()
endif()

# Copy TensorRT runtime DLLs (optional backend) from the tracked third_party folder so the
# "Use TensorRT" toggle survives a clean build. These are too large for git (see the README
# in that folder for the pip command to re-fetch them); the build just skips them if absent.
if(WIN32)
    file(GLOB TENSORRT_DLLS "${CMAKE_SOURCE_DIR}/third_party/tensorrt/*.dll")
    if(TENSORRT_DLLS)
        add_custom_command(TARGET ScreenAnalysisTool POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${TENSORRT_DLLS}
            $<TARGET_FILE_DIR:ScreenAnalysisTool>
            COMMENT "Copying TensorRT DLLs to output directory"
        )
    endif()
endif()

# Link Libraries
target_link_libraries(ScreenAnalysisTool PRIVATE
    ${OpenCV_LIBRARIES}
    CUDA::cudart
    CUDA::cuda_driver
    d3d11
    dxgi
    d3dcompiler
    dwmapi
)

if(VIGEM_FOUND)
    target_include_directories(ScreenAnalysisTool PRIVATE "${VIGEM_DIR}/include")
    target_link_libraries(ScreenAnalysisTool PRIVATE "${VIGEM_DIR}/lib/x64/ViGEmClient.lib" setupapi xinput9_1_0)
    target_compile_definitions(ScreenAnalysisTool PRIVATE BLAZE_HAVE_VIGEM=1 UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
endif()

# System libs for Windows
target_link_libraries(ScreenAnalysisTool PRIVATE
    kernel32
    user32
    gdi32
    winspool
    shell32
    ole32
    oleaut32
    uuid
    comdlg32
    advapi32
)

# Configure CUDA architectures explicitly
set_target_properties(ScreenAnalysisTool PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
