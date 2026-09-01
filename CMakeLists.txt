cmake_minimum_required(VERSION 3.28)

project(Halcyon VERSION 0.1.0 LANGUAGES C CXX)

option(HALCYON_BUILD_TESTS "Build Halcyon CPU-side tests" ON)
option(HALCYON_ENABLE_VALIDATION "Enable Vulkan validation by default" ON)
option(HALCYON_BUILD_EXPERIMENTAL_M2
    "Build unfinished M2+ RenderGraph, bindless and frame-budget prototypes" OFF)

option(HALCYON_ENABLE_GOOGLETEST "Build the GoogleTest dependency" OFF)
option(HALCYON_ENABLE_IMGUI "Build Dear ImGui" OFF)
option(HALCYON_ENABLE_SPIRV_TOOLS "Build SPIR-V validation and optimization tools" OFF)
option(HALCYON_ENABLE_BASISU "Build Basis Universal texture tooling" OFF)
option(HALCYON_ENABLE_MESHOPTIMIZER "Build meshoptimizer" OFF)
option(HALCYON_ENABLE_DRACO "Build Draco geometry compression" OFF)
option(HALCYON_ENABLE_BENCHMARK "Build Google Benchmark" OFF)
option(HALCYON_ENABLE_ZSTD "Build Zstandard" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

find_package(Vulkan 1.3 REQUIRED)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(ThirdParty/glfw)

set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(ThirdParty/glm)
add_subdirectory(ThirdParty)

function(halcyon_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(HalcyonCore INTERFACE)
target_include_directories(HalcyonCore INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")
target_compile_features(HalcyonCore INTERFACE cxx_std_20)
target_compile_definitions(HalcyonCore INTERFACE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
)

add_library(HalcyonRenderer STATIC
    Source/Renderer/Resources/DeferredDeletionQueue.h
    Source/Renderer/Scene/Camera.cpp
    Source/Renderer/Scene/Camera.h
    Source/Renderer/Scene/FramePacket.h
    Source/Renderer/Vulkan/HalcyonVulkanRenderer.cpp
    Source/Renderer/Vulkan/HalcyonVulkanRenderer.h
    Source/Renderer/Resources/ResourceTypes.h
    Source/Renderer/Resources/UploadRing.h
)
target_include_directories(HalcyonRenderer PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")
target_link_libraries(HalcyonRenderer
    PUBLIC HalcyonCore glm::glm Vulkan::Vulkan
    PRIVATE Halcyon::Vma Halcyon::Stb Halcyon::RobinMap
    PRIVATE glfw
)
target_compile_definitions(HalcyonRenderer PRIVATE GLFW_INCLUDE_NONE)
halcyon_enable_warnings(HalcyonRenderer)

# These CPU-only prototypes belong to M2 and later milestones.  Keeping them
# in a separate opt-in target prevents unfinished advanced work from becoming
# part of the M0/M1 renderer's build or public dependency closure.
if(HALCYON_BUILD_EXPERIMENTAL_M2)
    add_library(HalcyonRendererExperimental STATIC
        Source/Renderer/Graph/RenderGraph.cpp
        Source/Renderer/Graph/BarrierPlanner.cpp
        Source/Renderer/Graph/RenderGraph.h
        Source/Renderer/Graph/BarrierPlanner.h
        Source/Renderer/Resources/BindlessTable.cpp
        Source/Renderer/Resources/BindlessTable.h
        Source/Renderer/Quality/FrameBudgetController.cpp
        Source/Renderer/Quality/FrameBudgetController.h
    )
    target_include_directories(HalcyonRendererExperimental
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_link_libraries(HalcyonRendererExperimental PUBLIC HalcyonCore)
    halcyon_enable_warnings(HalcyonRendererExperimental)
endif()

find_program(HALCYON_DXC_EXECUTABLE
    NAMES dxc
    HINTS "$ENV{VULKAN_SDK}/Bin"
    DOC "DirectX Shader Compiler used to validate HLSL and produce SPIR-V"
)
if(HALCYON_DXC_EXECUTABLE)
    set(HALCYON_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/Shaders")
    set(HALCYON_VERTEX_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/triangle.vert.spv")
    set(HALCYON_FRAGMENT_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/triangle.frag.spv")

    add_custom_command(
        OUTPUT "${HALCYON_VERTEX_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3
                -Zpc -T vs_6_6 -E main
                -Fo "${HALCYON_VERTEX_SPIRV}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/triangle.vert.hlsl"
        DEPENDS Shaders/triangle.vert.hlsl
        VERBATIM
        COMMENT "Compiling triangle vertex shader"
    )
    add_custom_command(
        OUTPUT "${HALCYON_FRAGMENT_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3
                -Zpc -T ps_6_6 -E main
                -Fo "${HALCYON_FRAGMENT_SPIRV}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/triangle.frag.hlsl"
        DEPENDS Shaders/triangle.frag.hlsl
        VERBATIM
        COMMENT "Compiling triangle fragment shader"
    )
    add_custom_target(HalcyonShaders ALL
        DEPENDS "${HALCYON_VERTEX_SPIRV}" "${HALCYON_FRAGMENT_SPIRV}"
    )
    add_dependencies(HalcyonRenderer HalcyonShaders)
else()
    message(WARNING "DXC was not found. Embedded M1 shaders remain usable, but HLSL validation is disabled.")
endif()

add_executable(HalcyonSandbox Source/Sandbox/Main.cpp)
set_target_properties(HalcyonSandbox PROPERTIES OUTPUT_NAME Halcyon)
target_link_libraries(HalcyonSandbox PRIVATE HalcyonRenderer glfw)
if(HALCYON_ENABLE_IMGUI)
    target_link_libraries(HalcyonSandbox PRIVATE Halcyon::ImGui)
endif()
target_compile_definitions(HalcyonSandbox PRIVATE
    GLFW_INCLUDE_NONE
    HALCYON_ENABLE_VALIDATION=$<BOOL:${HALCYON_ENABLE_VALIDATION}>
)
halcyon_enable_warnings(HalcyonSandbox)

# The cooker deliberately has no renderer/Vulkan dependency.  Keeping it as a
# separate executable makes asset-pipeline experiments buildable on machines
# without a GPU and gives the runtime a deterministic manifest contract.
add_executable(HalcyonCooker Source/Cooker/Main.cpp)
target_link_libraries(HalcyonCooker PRIVATE HalcyonCore)
halcyon_enable_warnings(HalcyonCooker)

if(HALCYON_BUILD_TESTS)
    include(CTest)

    add_executable(HalcyonCoreTests Tests/CoreTests.cpp)
    target_link_libraries(HalcyonCoreTests PRIVATE HalcyonCore)
    halcyon_enable_warnings(HalcyonCoreTests)
    add_test(NAME Halcyon.Core COMMAND HalcyonCoreTests)

    if(HALCYON_BUILD_EXPERIMENTAL_M2)
        add_executable(HalcyonRendererCoreTests Tests/RendererCoreTests.cpp)
        target_link_libraries(HalcyonRendererCoreTests
            PRIVATE HalcyonRendererExperimental)
        halcyon_enable_warnings(HalcyonRendererCoreTests)
        add_test(NAME Halcyon.RendererCore COMMAND HalcyonRendererCoreTests)
    endif()

    add_executable(HalcyonCameraTests
        Tests/CameraTests.cpp
        Source/Renderer/Scene/Camera.cpp
    )
    target_link_libraries(HalcyonCameraTests PRIVATE HalcyonCore glm::glm)
    halcyon_enable_warnings(HalcyonCameraTests)
    add_test(NAME Halcyon.Camera COMMAND HalcyonCameraTests)

    add_executable(HalcyonInfrastructureTests Tests/RendererInfrastructureTests.cpp)
    target_link_libraries(HalcyonInfrastructureTests PRIVATE HalcyonCore)
    target_include_directories(HalcyonInfrastructureTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_compile_definitions(HalcyonInfrastructureTests PRIVATE
        HALCYON_BUILD_EXPERIMENTAL_M2=$<BOOL:${HALCYON_BUILD_EXPERIMENTAL_M2}>)
    if(HALCYON_BUILD_EXPERIMENTAL_M2)
        target_link_libraries(HalcyonInfrastructureTests
            PRIVATE HalcyonRendererExperimental)
    endif()
    halcyon_enable_warnings(HalcyonInfrastructureTests)
    add_test(NAME Halcyon.Infrastructure COMMAND HalcyonInfrastructureTests)

    add_executable(HalcyonDeferredDeletionTests Tests/DeferredDeletionQueueTests.cpp)
    target_link_libraries(HalcyonDeferredDeletionTests PRIVATE HalcyonCore)
    target_include_directories(HalcyonDeferredDeletionTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    halcyon_enable_warnings(HalcyonDeferredDeletionTests)
    add_test(NAME Halcyon.DeferredDeletion COMMAND HalcyonDeferredDeletionTests)

    # Convenience aggregate target used by IDEs and CI jobs that only need to
    # build the complete CPU test suite.  CTest remains the runner of record.
    add_custom_target(HalcyonTests)
    add_dependencies(HalcyonTests
        HalcyonCoreTests
        HalcyonCameraTests
        HalcyonInfrastructureTests
        HalcyonDeferredDeletionTests)
    if(HALCYON_BUILD_EXPERIMENTAL_M2)
        add_dependencies(HalcyonTests HalcyonRendererCoreTests)
    endif()
endif()
