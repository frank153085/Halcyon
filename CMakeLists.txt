cmake_minimum_required(VERSION 3.28)

project(Halcyon VERSION 0.1.0 LANGUAGES C CXX)

# CMake 4.4 enables Ninja's C++ module dependency scanner for ordinary C++
# translation units.  Halcyon does not use C++ modules yet; disabling the
# scanner keeps MSVC/Ninja builds reproducible (and avoids treating the
# standard library headers as module interfaces).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(HALCYON_BUILD_TESTS "Build Halcyon CPU-side tests" ON)
option(HALCYON_ENABLE_VALIDATION "Enable Vulkan validation by default" ON)
option(HALCYON_BUILD_EXPERIMENTAL_M2
    "Build M2 FrameGraph, bindless, budget and shader validation infrastructure" ON)
option(HALCYON_BUILD_EXAMPLES
    "Build standalone examples with isolated output directories" ON)

option(HALCYON_ENABLE_GOOGLETEST "Build the GoogleTest dependency" OFF)
option(HALCYON_ENABLE_IMGUI "Build Dear ImGui" OFF)
option(HALCYON_ENABLE_SPIRV_TOOLS "Build SPIR-V validation and optimization tools" OFF)
option(HALCYON_ENABLE_TRACY "Enable optional Tracy CPU instrumentation" OFF)
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

# fastgltf is the canonical M3 scene parser.  Keep the source pinned in the
# repository so a clean checkout does not depend on a system installation.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/fastgltf/CMakeLists.txt")
    set(FASTGLTF_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FASTGLTF_ENABLE_DOCS OFF CACHE BOOL "" FORCE)
    set(FASTGLTF_COMPILE_AS_CPP20 ON CACHE BOOL "" FORCE)
    add_subdirectory(ThirdParty/fastgltf EXCLUDE_FROM_ALL)
    add_library(Halcyon::FastGltf ALIAS fastgltf)
else()
    include(FetchContent)
    FetchContent_Declare(fastgltf
        GIT_REPOSITORY https://github.com/spnda/fastgltf.git
        GIT_TAG d4734a7e55d8d25155e755d895e018e423845cd0
        GIT_SHALLOW TRUE)
    set(FASTGLTF_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FASTGLTF_ENABLE_DOCS OFF CACHE BOOL "" FORCE)
    set(FASTGLTF_COMPILE_AS_CPP20 ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(fastgltf)
    add_library(Halcyon::FastGltf ALIAS fastgltf)
endif()

set(HALCYON_TRACY_ACTIVE OFF)
if(HALCYON_ENABLE_TRACY AND TARGET Halcyon::Tracy)
    set(HALCYON_TRACY_ACTIVE ON)
endif()

function(halcyon_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(halcyon_configure_asset_root target)
    target_compile_definitions(${target} PRIVATE
        HALCYON_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets")
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
    Source/Core/Profiler.h
    Source/Renderer/Resources/DeferredDeletionQueue.h
    Source/Renderer/Scene/Camera.cpp
    Source/Renderer/Scene/Camera.h
    Source/Renderer/Scene/FramePacket.h
    Source/Renderer/Scene/GpuScene.h
    Source/Renderer/Scene/StaticSceneLoader.cpp
    Source/Renderer/Scene/StaticSceneLoader.h
    Source/Renderer/Scene/ProceduralStressScene.cpp
    Source/Renderer/Scene/ProceduralStressScene.h
    Source/Renderer/Scene/SceneDatabase.h
    Source/Renderer/Scene/Ecs/Entity.h
    Source/Renderer/Scene/Ecs/Ecs.h
    Source/Renderer/Scene/Ecs/EntityManager.cpp
    Source/Renderer/Scene/Ecs/EntityManager.h
    Source/Renderer/Scene/Ecs/DenseComponentManager.h
    Source/Renderer/Scene/Ecs/TransformManager.cpp
    Source/Renderer/Scene/Ecs/TransformManager.h
    Source/Renderer/Scene/Ecs/RenderableManager.h
    Source/Renderer/Scene/Ecs/LightManager.h
    Source/Renderer/Scene/Ecs/Scene.cpp
    Source/Renderer/Scene/Ecs/Scene.h
    Source/Renderer/Scene/Ecs/RenderExtractor.cpp
    Source/Renderer/Scene/Ecs/RenderExtractor.h
    Source/Renderer/Vulkan/HalcyonVulkanRenderer.cpp
    Source/Renderer/Vulkan/HalcyonVulkanRenderer.h
    Source/Renderer/Vulkan/VulkanCommon.cpp
    Source/Renderer/Vulkan/VulkanCommon.h
    Source/Renderer/Vulkan/VulkanShaderModule.cpp
    Source/Renderer/Vulkan/VulkanShaderModule.h
    Source/Renderer/Shaders/ShaderReflection.cpp
    Source/Renderer/Shaders/ShaderReflection.h
    Source/Renderer/Vulkan/ShaderLibrary.h
    Source/Renderer/Vulkan/ShaderLibrary.cpp
    Source/Renderer/Vulkan/VulkanPipeline.cpp
    Source/Renderer/Vulkan/VulkanPipeline.h
    Source/Renderer/Vulkan/VulkanSwapchain.cpp
    Source/Renderer/Vulkan/VulkanSwapchain.h
    Source/Renderer/Vulkan/VulkanDevice.cpp
    Source/Renderer/Vulkan/VulkanDevice.h
    Source/Renderer/Vulkan/VulkanBindlessTable.cpp
    Source/Renderer/Vulkan/VulkanBindlessTable.h
    Source/Renderer/Vulkan/VulkanFrameContext.cpp
    Source/Renderer/Vulkan/VulkanFrameContext.h
    Source/Renderer/Vulkan/VulkanSceneResources.cpp
    Source/Renderer/Vulkan/VulkanSceneResources.h
    Source/Renderer/Vulkan/VulkanFrameGraphProvider.cpp
    Source/Renderer/Vulkan/VulkanFrameGraphProvider.h
    Source/Renderer/Vulkan/VulkanM3FrameResources.cpp
    Source/Renderer/Vulkan/VulkanM3FrameResources.h
    Source/Renderer/Vulkan/GpuAllocator.cpp
    Source/Renderer/Vulkan/GpuAllocator.h
    Source/Renderer/Vulkan/GpuUploader.cpp
    Source/Renderer/Vulkan/GpuUploader.h
    Source/Renderer/Vulkan/GpuResourceManager.cpp
    Source/Renderer/Vulkan/StbImage.cpp
    Source/Renderer/Vulkan/GpuResourceManager.h
    Source/Renderer/Resources/ResourceTypes.h
    Source/Renderer/Resources/BindlessTable.cpp
    Source/Renderer/Resources/BindlessTable.h
    Source/Renderer/Resources/UploadRing.h
    Source/Renderer/Quality/Pbr.h
    Source/Renderer/Quality/Ibl.h
    Source/Renderer/Quality/CascadedShadowMaps.h
    Source/Renderer/Quality/GBuffer.h
    Source/Renderer/Quality/Hdr.h
    Source/Renderer/Quality/TemporalAA.h
    Source/Renderer/Quality/Transparency.h
    Source/Renderer/Quality/TraditionalRenderPath.h
    Source/Renderer/Quality/Quality.h
)
target_include_directories(HalcyonRenderer PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")
target_link_libraries(HalcyonRenderer
    PUBLIC HalcyonCore glm::glm Vulkan::Vulkan
    PRIVATE Halcyon::Vma Halcyon::Stb
    PUBLIC Halcyon::RobinMap
    PRIVATE glfw
)
if(TARGET Halcyon::FastGltf)
    target_link_libraries(HalcyonRenderer PRIVATE Halcyon::FastGltf)
endif()
target_compile_definitions(HalcyonRenderer PRIVATE
    GLFW_INCLUDE_NONE
    HALCYON_BUILD_EXPERIMENTAL_M2=$<BOOL:${HALCYON_BUILD_EXPERIMENTAL_M2}>
    HALCYON_ENABLE_TRACY=$<BOOL:${HALCYON_TRACY_ACTIVE}>)
halcyon_enable_warnings(HalcyonRenderer)
target_compile_definitions(HalcyonRenderer PRIVATE HALCYON_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/Shaders")

if(HALCYON_ENABLE_SPIRV_TOOLS)
    target_compile_definitions(HalcyonRenderer PRIVATE HALCYON_ENABLE_SPIRV_TOOLS=1)
    target_link_libraries(HalcyonRenderer PRIVATE Halcyon::SPIRVTools)
endif()

if(HALCYON_TRACY_ACTIVE)
    target_link_libraries(HalcyonRenderer PRIVATE Halcyon::Tracy)
elseif(HALCYON_ENABLE_TRACY)
    message(WARNING "HALCYON_ENABLE_TRACY is ON but the Tracy target is unavailable")
endif()

# M2 infrastructure remains a separate target so its graph, barrier, bindless,
# and quality code can evolve independently of the Vulkan frame loop.
if(HALCYON_BUILD_EXPERIMENTAL_M2)
    add_library(HalcyonFrameGraph STATIC
        # Keep this list in the same order and at the same granularity as
        # filament/src/fg.  The graph itself is backend-neutral; the source
        # files below are intentionally one responsibility per translation
        # unit so navigating between the two projects is mechanical.
        Source/Renderer/Graph/Blackboard.cpp
        Source/Renderer/Graph/DependencyGraph.cpp
        Source/Renderer/Graph/FrameGraph.cpp
        Source/Renderer/Graph/FrameGraphPass.cpp
        Source/Renderer/Graph/FrameGraphResources.cpp
        Source/Renderer/Graph/FrameGraphTexture.cpp
        Source/Renderer/Graph/PassNode.cpp
        Source/Renderer/Graph/ResourceNode.cpp
        Source/Renderer/Graph/ResourceCreationContext.cpp
        Source/Renderer/Graph/Resource.cpp
        Source/Core/Memory/FrameArena.cpp
        Source/Core/Memory/LinearAllocator.cpp
        Source/Renderer/Graph/Blackboard.h
        Source/Renderer/Graph/FrameGraphId.h
        Source/Renderer/Graph/FrameGraphPass.h
        Source/Renderer/Graph/FrameGraphRenderPass.h
        Source/Renderer/Graph/FrameGraphResources.h
        Source/Renderer/Graph/FrameGraphTexture.h
        Source/Renderer/Graph/FrameGraphTypes.h
        Source/Renderer/Graph/FrameGraphDummyLink.h
        Source/Renderer/Graph/FrameGraph.h
        Source/Renderer/Graph/details/DependencyGraph.h
        Source/Renderer/Graph/details/PassNode.h
        Source/Renderer/Graph/details/Resource.h
        Source/Renderer/Graph/details/ResourceAllocator.h
        Source/Renderer/Graph/details/ResourceCreationContext.h
        Source/Renderer/Graph/details/ResourceNode.h
        Source/Renderer/Graph/details/Utilities.h
        Source/Renderer/Graph/FrameGraphArena.h)
    target_include_directories(HalcyonFrameGraph PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/Source"
        "${CMAKE_CURRENT_SOURCE_DIR}/Include")
    target_link_libraries(HalcyonFrameGraph PUBLIC HalcyonCore)
    halcyon_enable_warnings(HalcyonFrameGraph)

    add_library(HalcyonRendererExperimental STATIC
        Source/Renderer/Graph/BarrierPlanner.cpp
        Source/Renderer/Graph/BarrierPlanner.h
        Source/Renderer/Resources/BindlessTable.cpp
        Source/Renderer/Resources/BindlessTable.h
        Source/Renderer/Quality/FrameBudgetController.cpp
        Source/Renderer/Quality/FrameBudgetController.h
    )
    target_include_directories(HalcyonRendererExperimental
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_link_libraries(HalcyonRendererExperimental PUBLIC HalcyonCore HalcyonFrameGraph)
    halcyon_enable_warnings(HalcyonRendererExperimental)
    target_link_libraries(HalcyonRenderer PRIVATE HalcyonRendererExperimental)
endif()

find_program(HALCYON_DXC_EXECUTABLE
    NAMES dxc
    HINTS "$ENV{VULKAN_SDK}/Bin"
    DOC "DirectX Shader Compiler used to validate HLSL and produce SPIR-V"
)
if(HALCYON_DXC_EXECUTABLE)
    # Validate every generated module when SPIR-V Tools is available.  This
    # is intentionally an additive build target: DXC remains the required
    # compiler, while spirv-val catches descriptor, capability, and layout
    # errors before a Vulkan device is touched.
    find_program(HALCYON_SPIRV_VAL_EXECUTABLE
        NAMES spirv-val
        HINTS "$ENV{VULKAN_SDK}/Bin"
        DOC "SPIR-V validator used for generated M3 shader modules"
    )
    set(HALCYON_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/Shaders")
    set(HALCYON_PBR_VERTEX_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/pbr.vert.spv")
    set(HALCYON_TRANSPARENT_VERTEX_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/forward_transparent.vert.spv")
    set(HALCYON_FULLSCREEN_VERTEX_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/fullscreen.vert.spv")
    set(HALCYON_PBR_FRAGMENT_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/pbr.frag.spv")
    set(HALCYON_GBUFFER_FRAGMENT_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/gbuffer.frag.spv")
    set(HALCYON_TRANSPARENT_FRAGMENT_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/forward_transparent.frag.spv")
    set(HALCYON_TONEMAP_FRAGMENT_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/tonemap.frag.spv")
    set(HALCYON_TAA_COMPUTE_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/taa.comp.spv")
    set(HALCYON_CSM_VERTEX_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/csm.vert.spv")
    set(HALCYON_CLUSTER_BUILD_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/cluster_build.comp.spv")
    set(HALCYON_FRUSTUM_CULL_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/frustum_cull.comp.spv")
    set(HALCYON_INDIRECT_BUILD_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/build_indirect_commands.comp.spv")
    set(HALCYON_HIZ_BUILD_SPIRV "${HALCYON_SHADER_OUTPUT_DIR}/hiz_build.comp.spv")
    add_custom_command(OUTPUT "${HALCYON_PBR_VERTEX_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3 -Zpc -T vs_6_6 -E main
                -Fo "${HALCYON_PBR_VERTEX_SPIRV}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/pbr.vert.hlsl"
        DEPENDS Shaders/pbr.vert.hlsl VERBATIM COMMENT "Compiling PBR vertex shader")
    add_custom_command(OUTPUT "${HALCYON_TRANSPARENT_VERTEX_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3 -Zpc -T vs_6_6 -E main
                -Fo "${HALCYON_TRANSPARENT_VERTEX_SPIRV}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/forward_transparent.vert.hlsl"
        DEPENDS Shaders/forward_transparent.vert.hlsl VERBATIM COMMENT "Compiling transparency vertex shader")
    add_custom_command(OUTPUT "${HALCYON_FULLSCREEN_VERTEX_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3 -Zpc -T vs_6_6 -E main
                -Fo "${HALCYON_FULLSCREEN_VERTEX_SPIRV}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/fullscreen.vert.hlsl"
        DEPENDS Shaders/fullscreen.vert.hlsl VERBATIM COMMENT "Compiling fullscreen vertex shader")
    add_custom_command(OUTPUT "${HALCYON_GBUFFER_FRAGMENT_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3 -Zpc -T ps_6_6 -E main
                -Fo "${HALCYON_GBUFFER_FRAGMENT_SPIRV}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/gbuffer.frag.hlsl"
        DEPENDS Shaders/gbuffer.frag.hlsl VERBATIM COMMENT "Compiling G-buffer fragment shader")
    add_custom_command(OUTPUT "${HALCYON_PBR_FRAGMENT_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3 -Zpc -T ps_6_6 -E main
                -Fo "${HALCYON_PBR_FRAGMENT_SPIRV}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/pbr.frag.hlsl"
        DEPENDS Shaders/pbr.frag.hlsl VERBATIM COMMENT "Compiling PBR fragment shader")
    add_custom_command(OUTPUT "${HALCYON_TRANSPARENT_FRAGMENT_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3 -Zpc -T ps_6_6 -E main
                -Fo "${HALCYON_TRANSPARENT_FRAGMENT_SPIRV}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/forward_transparent.frag.hlsl"
        DEPENDS Shaders/forward_transparent.frag.hlsl VERBATIM COMMENT "Compiling transparency shader")
    add_custom_command(OUTPUT "${HALCYON_TONEMAP_FRAGMENT_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3 -Zpc -T ps_6_6 -E main
                -Fo "${HALCYON_TONEMAP_FRAGMENT_SPIRV}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/tonemap.frag.hlsl"
        DEPENDS Shaders/tonemap.frag.hlsl VERBATIM COMMENT "Compiling ACES tonemap shader")
    add_custom_command(OUTPUT "${HALCYON_TAA_COMPUTE_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3 -Zpc -T cs_6_6 -E main
                -Fo "${HALCYON_TAA_COMPUTE_SPIRV}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/taa.comp.hlsl"
        DEPENDS Shaders/taa.comp.hlsl VERBATIM COMMENT "Compiling TAA compute shader")
    add_custom_command(OUTPUT "${HALCYON_CSM_VERTEX_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3 -Zpc -T vs_6_6 -E main
                -Fo "${HALCYON_CSM_VERTEX_SPIRV}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/csm.vert.hlsl"
        DEPENDS Shaders/csm.vert.hlsl VERBATIM COMMENT "Compiling CSM depth vertex shader")
    add_custom_command(OUTPUT "${HALCYON_CLUSTER_BUILD_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3 -Zpc -T cs_6_6 -E main
                -Fo "${HALCYON_CLUSTER_BUILD_SPIRV}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/cluster_build.comp.hlsl"
        DEPENDS Shaders/cluster_build.comp.hlsl VERBATIM COMMENT "Compiling clustered-lighting compute shader")
    add_custom_command(OUTPUT "${HALCYON_FRUSTUM_CULL_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3 -Zpc -T cs_6_6 -E main
                -Fo "${HALCYON_FRUSTUM_CULL_SPIRV}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/frustum_cull.comp.hlsl"
        DEPENDS Shaders/frustum_cull.comp.hlsl VERBATIM COMMENT "Compiling frustum culling shader")
    add_custom_command(OUTPUT "${HALCYON_INDIRECT_BUILD_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3 -Zpc -T cs_6_6 -E main
                -Fo "${HALCYON_INDIRECT_BUILD_SPIRV}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/build_indirect_commands.comp.hlsl"
        DEPENDS Shaders/build_indirect_commands.comp.hlsl VERBATIM COMMENT "Compiling indirect command shader")
    add_custom_command(OUTPUT "${HALCYON_HIZ_BUILD_SPIRV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HALCYON_SHADER_OUTPUT_DIR}"
        COMMAND "${HALCYON_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.3 -Zpc -T cs_6_6 -E main
                -Fo "${HALCYON_HIZ_BUILD_SPIRV}" "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/hiz_build.comp.hlsl"
        DEPENDS Shaders/hiz_build.comp.hlsl VERBATIM COMMENT "Compiling Hi-Z shader")
    add_custom_target(HalcyonShaders ALL
        DEPENDS "${HALCYON_FULLSCREEN_VERTEX_SPIRV}"
            "${HALCYON_PBR_VERTEX_SPIRV}" "${HALCYON_TRANSPARENT_VERTEX_SPIRV}" "${HALCYON_PBR_FRAGMENT_SPIRV}" "${HALCYON_GBUFFER_FRAGMENT_SPIRV}"
            "${HALCYON_TRANSPARENT_FRAGMENT_SPIRV}" "${HALCYON_TONEMAP_FRAGMENT_SPIRV}" "${HALCYON_TAA_COMPUTE_SPIRV}"
            "${HALCYON_CSM_VERTEX_SPIRV}" "${HALCYON_CLUSTER_BUILD_SPIRV}"
            "${HALCYON_FRUSTUM_CULL_SPIRV}" "${HALCYON_INDIRECT_BUILD_SPIRV}" "${HALCYON_HIZ_BUILD_SPIRV}"
    )
    if(HALCYON_SPIRV_VAL_EXECUTABLE)
        add_custom_target(HalcyonShaderValidation ALL
            COMMAND "${HALCYON_SPIRV_VAL_EXECUTABLE}" "${HALCYON_FULLSCREEN_VERTEX_SPIRV}"
            COMMAND "${HALCYON_SPIRV_VAL_EXECUTABLE}" "${HALCYON_PBR_VERTEX_SPIRV}"
            COMMAND "${HALCYON_SPIRV_VAL_EXECUTABLE}" "${HALCYON_TRANSPARENT_VERTEX_SPIRV}"
            COMMAND "${HALCYON_SPIRV_VAL_EXECUTABLE}" "${HALCYON_PBR_FRAGMENT_SPIRV}"
            COMMAND "${HALCYON_SPIRV_VAL_EXECUTABLE}" "${HALCYON_GBUFFER_FRAGMENT_SPIRV}"
            COMMAND "${HALCYON_SPIRV_VAL_EXECUTABLE}" "${HALCYON_TRANSPARENT_FRAGMENT_SPIRV}"
            COMMAND "${HALCYON_SPIRV_VAL_EXECUTABLE}" "${HALCYON_TONEMAP_FRAGMENT_SPIRV}"
            COMMAND "${HALCYON_SPIRV_VAL_EXECUTABLE}" "${HALCYON_TAA_COMPUTE_SPIRV}"
            COMMAND "${HALCYON_SPIRV_VAL_EXECUTABLE}" "${HALCYON_CSM_VERTEX_SPIRV}"
            COMMAND "${HALCYON_SPIRV_VAL_EXECUTABLE}" "${HALCYON_CLUSTER_BUILD_SPIRV}"
            COMMAND "${HALCYON_SPIRV_VAL_EXECUTABLE}" "${HALCYON_FRUSTUM_CULL_SPIRV}"
            COMMAND "${HALCYON_SPIRV_VAL_EXECUTABLE}" "${HALCYON_INDIRECT_BUILD_SPIRV}"
            COMMAND "${HALCYON_SPIRV_VAL_EXECUTABLE}" "${HALCYON_HIZ_BUILD_SPIRV}"
            DEPENDS HalcyonShaders
            COMMENT "Validating generated M3 SPIR-V modules"
            VERBATIM
        )
        add_dependencies(HalcyonRenderer HalcyonShaderValidation)
    else()
        add_dependencies(HalcyonRenderer HalcyonShaders)
    endif()
else()
    message(FATAL_ERROR "DXC is required to build the Vulkan M3 renderer. Set VULKAN_SDK or HALCYON_DXC_EXECUTABLE.")
endif()

# Backend-neutral orchestration layer.  Engine owns scene extraction and the
# Vulkan renderer while exposing only public Halcyon types to applications.
add_library(HalcyonEngine STATIC
    Include/Halcyon/Engine.h
    Include/Halcyon/RenderTypes.h
    Include/Halcyon/Scene.h
    Include/Halcyon/GpuScene.h
    Include/Halcyon/SceneManager.h
    Include/Halcyon/Quality.h
    Include/Halcyon/TraditionalQuality.h
    Include/Halcyon/StaticScene.h
    Include/Halcyon/View.h
    Source/Engine/Engine.cpp
    Source/Engine/SceneManager.cpp
    Source/Engine/EngineInternal.h
    Source/Engine/View.cpp
)
target_include_directories(HalcyonEngine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
    "${CMAKE_CURRENT_SOURCE_DIR}/Include")
target_link_libraries(HalcyonEngine PUBLIC HalcyonRenderer)
target_compile_definitions(HalcyonEngine PUBLIC
    HALCYON_ENABLE_VALIDATION=$<BOOL:${HALCYON_ENABLE_VALIDATION}>)
halcyon_enable_warnings(HalcyonEngine)

# Application owns the platform window and process/event loop.  GLFW remains
# an implementation dependency and is not visible from public headers.
add_library(HalcyonApplication STATIC
    Include/Halcyon/Application.h
    Include/Halcyon/Window.h
    Source/Application/Application.cpp
    Source/Application/DiagnosticsOverlay.cpp
    Source/Application/DiagnosticsOverlay.h
    Source/Application/Window.cpp
    Source/Application/WindowInternal.h
)
target_include_directories(HalcyonApplication PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Source"
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/stb")
target_link_libraries(HalcyonApplication PUBLIC HalcyonEngine PRIVATE glfw)
target_compile_definitions(HalcyonApplication PRIVATE
    GLFW_INCLUDE_NONE
    HALCYON_ENABLE_IMGUI=$<BOOL:${HALCYON_ENABLE_IMGUI}>)
if(HALCYON_ENABLE_IMGUI)
    target_link_libraries(HalcyonApplication PRIVATE Halcyon::ImGui)
endif()
halcyon_enable_warnings(HalcyonApplication)

add_executable(HalcyonSandbox Source/Sandbox/Main.cpp)
target_sources(HalcyonSandbox PRIVATE
    Source/Sandbox/SandboxExample.cpp
    Source/Sandbox/SandboxExample.h)
set_target_properties(HalcyonSandbox PROPERTIES OUTPUT_NAME Halcyon)
target_include_directories(HalcyonSandbox PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source/Sandbox")
target_link_libraries(HalcyonSandbox PRIVATE HalcyonApplication)
target_compile_definitions(HalcyonSandbox PRIVATE
    HALCYON_ENABLE_TRACY=$<BOOL:${HALCYON_TRACY_ACTIVE}>
)
halcyon_configure_asset_root(HalcyonSandbox)
halcyon_enable_warnings(HalcyonSandbox)

# M3 showcase executable.  It intentionally has its own entry point so the
# original sandbox remains a stable M1/M2 regression target.
add_executable(HalcyonM3Demo
    Source/M3Demo/Main.cpp
    Source/M3Demo/M3Demo.cpp
    Source/M3Demo/M3Demo.h)
target_link_libraries(HalcyonM3Demo PRIVATE HalcyonApplication)
target_compile_definitions(HalcyonM3Demo PRIVATE
    HALCYON_ENABLE_TRACY=$<BOOL:${HALCYON_TRACY_ACTIVE}>)
halcyon_configure_asset_root(HalcyonM3Demo)
halcyon_enable_warnings(HalcyonM3Demo)

# Standalone image gate used by CI and local capture workflows.  It is kept
# independent from the renderer so it can decode PNGs on machines without a
# Vulkan device.
add_executable(HalcyonGoldenCompare Source/M3Demo/GoldenCompare.cpp)
target_link_libraries(HalcyonGoldenCompare PRIVATE HalcyonCore Halcyon::Stb)
halcyon_configure_asset_root(HalcyonGoldenCompare)
halcyon_enable_warnings(HalcyonGoldenCompare)

add_custom_target(HalcyonFetchM3Assets
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fetch_m3_assets.ps1"
            -Destination "${CMAKE_CURRENT_SOURCE_DIR}/assets/m3"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fetch_m3_assets.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    USES_TERMINAL)

if(HALCYON_BUILD_EXAMPLES)
    add_subdirectory(Examples)
endif()

# The cooker deliberately has no renderer/Vulkan dependency.  Keeping it as a
# separate executable makes asset-pipeline experiments buildable on machines
# without a GPU and gives the runtime a deterministic manifest contract.
add_executable(HalcyonCooker Source/Cooker/Main.cpp)
target_link_libraries(HalcyonCooker PRIVATE HalcyonCore)
halcyon_configure_asset_root(HalcyonCooker)
halcyon_enable_warnings(HalcyonCooker)

if(HALCYON_BUILD_TESTS)
    include(CTest)

    add_executable(HalcyonCoreTests Tests/CoreTests.cpp)
    target_link_libraries(HalcyonCoreTests PRIVATE HalcyonCore)
    halcyon_enable_warnings(HalcyonCoreTests)
    add_test(NAME Halcyon.Core COMMAND HalcyonCoreTests)

    add_executable(HalcyonMemoryAllocatorTests
        Tests/MemoryAllocatorTests.cpp
        Source/Core/Memory/HeapAllocator.cpp
        Source/Core/Memory/LinearAllocator.cpp
        Source/Core/Memory/FrameArena.cpp)
    target_link_libraries(HalcyonMemoryAllocatorTests PRIVATE HalcyonCore)
    halcyon_enable_warnings(HalcyonMemoryAllocatorTests)
    add_test(NAME Halcyon.MemoryAllocator COMMAND HalcyonMemoryAllocatorTests)

    if(HALCYON_BUILD_EXPERIMENTAL_M2)
        add_executable(HalcyonRendererCoreTests Tests/RendererCoreTests.cpp)
        target_link_libraries(HalcyonRendererCoreTests
            PRIVATE HalcyonRendererExperimental)
        halcyon_enable_warnings(HalcyonRendererCoreTests)
        add_test(NAME Halcyon.RendererCore COMMAND HalcyonRendererCoreTests)

        add_executable(HalcyonShaderReflectionTests
            Tests/ShaderReflectionTests.cpp
            Source/Renderer/Shaders/ShaderReflection.cpp)
        target_link_libraries(HalcyonShaderReflectionTests PRIVATE HalcyonCore)
        target_include_directories(HalcyonShaderReflectionTests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/Source")
        halcyon_enable_warnings(HalcyonShaderReflectionTests)
        add_test(NAME Halcyon.ShaderReflection COMMAND HalcyonShaderReflectionTests)

        if(HALCYON_ENABLE_GOOGLETEST)
            add_executable(HalcyonM2GoogleTests Tests/M2GoogleTests.cpp)
            target_link_libraries(HalcyonM2GoogleTests
                PRIVATE HalcyonRendererExperimental Halcyon::GoogleTest)
            halcyon_enable_warnings(HalcyonM2GoogleTests)
            add_test(NAME Halcyon.M2Google COMMAND HalcyonM2GoogleTests)
        endif()
    endif()

    add_executable(HalcyonCameraTests
        Tests/CameraTests.cpp
        Source/Renderer/Scene/Camera.cpp
    )
    target_link_libraries(HalcyonCameraTests PRIVATE HalcyonCore glm::glm)
    halcyon_enable_warnings(HalcyonCameraTests)
    add_test(NAME Halcyon.Camera COMMAND HalcyonCameraTests)

    add_executable(HalcyonM3QualityTests Tests/M3QualityTests.cpp)
    target_link_libraries(HalcyonM3QualityTests PRIVATE HalcyonCore glm::glm)
    target_include_directories(HalcyonM3QualityTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    halcyon_enable_warnings(HalcyonM3QualityTests)
    add_test(NAME Halcyon.M3Quality COMMAND HalcyonM3QualityTests)

    add_executable(HalcyonStaticSceneLoaderTests Tests/StaticSceneLoaderTests.cpp)
    target_link_libraries(HalcyonStaticSceneLoaderTests PRIVATE HalcyonRenderer)
    target_compile_definitions(HalcyonStaticSceneLoaderTests PRIVATE
        HALCYON_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    halcyon_enable_warnings(HalcyonStaticSceneLoaderTests)
    add_test(NAME Halcyon.StaticSceneLoader COMMAND HalcyonStaticSceneLoaderTests)

    add_executable(HalcyonEcsTests
        Tests/EcsTests.cpp
        Source/Renderer/Scene/Ecs/EntityManager.cpp
        Source/Renderer/Scene/Ecs/TransformManager.cpp
        Source/Renderer/Scene/Ecs/Scene.cpp
        Source/Renderer/Scene/Ecs/RenderExtractor.cpp)
    target_link_libraries(HalcyonEcsTests PRIVATE HalcyonCore glm::glm Halcyon::RobinMap)
    target_include_directories(HalcyonEcsTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    halcyon_enable_warnings(HalcyonEcsTests)
    add_test(NAME Halcyon.Ecs COMMAND HalcyonEcsTests)

    add_executable(HalcyonSceneManagerTests Tests/SceneManagerTests.cpp)
    target_link_libraries(HalcyonSceneManagerTests PRIVATE HalcyonEngine)
    target_compile_definitions(HalcyonSceneManagerTests PRIVATE
        HALCYON_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    halcyon_enable_warnings(HalcyonSceneManagerTests)
    add_test(NAME Halcyon.SceneManager COMMAND HalcyonSceneManagerTests)

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

    add_executable(HalcyonGpuSceneTests Tests/GpuSceneTests.cpp)
    target_link_libraries(HalcyonGpuSceneTests PRIVATE HalcyonCore glm::glm)
    target_include_directories(HalcyonGpuSceneTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    halcyon_enable_warnings(HalcyonGpuSceneTests)
    add_test(NAME Halcyon.GpuScene COMMAND HalcyonGpuSceneTests)

    add_executable(HalcyonRenderExtractorDeltaTests
        Tests/RenderExtractorDeltaTests.cpp
        Source/Renderer/Scene/Ecs/EntityManager.cpp
        Source/Renderer/Scene/Ecs/TransformManager.cpp
        Source/Renderer/Scene/Ecs/Scene.cpp
        Source/Renderer/Scene/Ecs/RenderExtractor.cpp)
    target_link_libraries(HalcyonRenderExtractorDeltaTests PRIVATE HalcyonCore glm::glm Halcyon::RobinMap)
    target_include_directories(HalcyonRenderExtractorDeltaTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    halcyon_enable_warnings(HalcyonRenderExtractorDeltaTests)
    add_test(NAME Halcyon.RenderExtractorDelta COMMAND HalcyonRenderExtractorDeltaTests)

    add_executable(HalcyonFrustumCullingTests Tests/FrustumCullingTests.cpp)
    target_link_libraries(HalcyonFrustumCullingTests PRIVATE HalcyonCore glm::glm)
    target_include_directories(HalcyonFrustumCullingTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    halcyon_enable_warnings(HalcyonFrustumCullingTests)
    add_test(NAME Halcyon.FrustumCulling COMMAND HalcyonFrustumCullingTests)

    add_executable(HalcyonHiZReductionTests Tests/HiZReductionTests.cpp)
    target_link_libraries(HalcyonHiZReductionTests PRIVATE HalcyonCore glm::glm)
    target_include_directories(HalcyonHiZReductionTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    halcyon_enable_warnings(HalcyonHiZReductionTests)
    add_test(NAME Halcyon.HiZReduction COMMAND HalcyonHiZReductionTests)

    # Convenience aggregate target used by IDEs and CI jobs that only need to
    # build the complete CPU test suite.  CTest remains the runner of record.
    add_custom_target(HalcyonTests)
    add_dependencies(HalcyonTests
        HalcyonCoreTests
        HalcyonMemoryAllocatorTests
        HalcyonCameraTests
        HalcyonM3QualityTests
        HalcyonStaticSceneLoaderTests
        HalcyonEcsTests
        HalcyonSceneManagerTests
        HalcyonInfrastructureTests
        HalcyonDeferredDeletionTests
        HalcyonGpuSceneTests
        HalcyonRenderExtractorDeltaTests
        HalcyonFrustumCullingTests
        HalcyonHiZReductionTests)
    if(HALCYON_BUILD_EXPERIMENTAL_M2)
        add_dependencies(HalcyonTests HalcyonRendererCoreTests HalcyonShaderReflectionTests)
        if(HALCYON_ENABLE_GOOGLETEST)
            add_dependencies(HalcyonTests HalcyonM2GoogleTests)
        endif()
    endif()
endif()
