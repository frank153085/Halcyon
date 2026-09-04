cmake_minimum_required(VERSION 3.28)

# Halcyon owns these wrapper targets.  The vendored projects remain untouched so
# their upstream build systems and license files stay intact.

function(halcyon_add_header_only name include_dir)
    add_library(${name} INTERFACE)
    target_include_directories(${name} SYSTEM INTERFACE
        "${CMAKE_CURRENT_LIST_DIR}/${include_dir}")
    add_library(Halcyon::${name} ALIAS ${name})
endfunction()

halcyon_add_header_only(Vma vkmemalloc/include)
halcyon_add_header_only(Stb stb)
halcyon_add_header_only(RobinMap robin-map/include)

add_library(HalcyonMikkTSpace STATIC mikktspace/mikktspace.c)
target_include_directories(HalcyonMikkTSpace PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/mikktspace")
set_target_properties(HalcyonMikkTSpace PROPERTIES C_STANDARD 11)
add_library(Halcyon::MikkTSpace ALIAS HalcyonMikkTSpace)

add_library(HalcyonImGui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
)
target_include_directories(HalcyonImGui PUBLIC "${CMAKE_CURRENT_LIST_DIR}/imgui")
if(HALCYON_ENABLE_IMGUI)
    target_sources(HalcyonImGui PRIVATE
        imgui/backends/imgui_impl_glfw.cpp
        imgui/backends/imgui_impl_vulkan.cpp)
    target_include_directories(HalcyonImGui PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/imgui/backends")
    target_link_libraries(HalcyonImGui PUBLIC glfw Vulkan::Vulkan)
endif()
add_library(Halcyon::ImGui ALIAS HalcyonImGui)

option(HALCYON_ENABLE_TRACY "Enable optional Tracy CPU instrumentation" OFF)
if(HALCYON_ENABLE_TRACY)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/tracy/public/TracyClient.cpp")
        add_library(HalcyonTracy STATIC tracy/public/TracyClient.cpp)
        target_include_directories(HalcyonTracy SYSTEM PUBLIC
            "${CMAKE_CURRENT_LIST_DIR}/tracy/public")
        target_compile_definitions(HalcyonTracy PUBLIC TRACY_ENABLE)
        if(WIN32)
            target_link_libraries(HalcyonTracy PUBLIC ws2_32)
        endif()
        add_library(Halcyon::Tracy ALIAS HalcyonTracy)
    else()
        message(WARNING "Tracy submodule is not available; instrumentation is disabled")
    endif()
endif()

option(HALCYON_ENABLE_GOOGLETEST "Build the GoogleTest dependency" OFF)
if(HALCYON_ENABLE_GOOGLETEST)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(gtest_build_samples OFF CACHE BOOL "" FORCE)
    add_subdirectory(googletest EXCLUDE_FROM_ALL)
    add_library(Halcyon::GoogleTest ALIAS gtest)
endif()

option(HALCYON_ENABLE_SPIRV_TOOLS "Build SPIR-V validation and optimization tools" OFF)
if(HALCYON_ENABLE_SPIRV_TOOLS)
    set(SPIRV-Headers_SOURCE_DIR
        "${CMAKE_CURRENT_LIST_DIR}/spirv-headers"
        CACHE PATH "Pinned SPIR-V Headers source" FORCE)
    set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
    set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
    set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
    set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
    add_subdirectory(spirv-tools EXCLUDE_FROM_ALL)
    if(TARGET SPIRV-Tools-static)
        add_library(Halcyon::SPIRVTools ALIAS SPIRV-Tools-static)
    elseif(TARGET SPIRV-Tools)
        add_library(Halcyon::SPIRVTools ALIAS SPIRV-Tools)
    endif()
endif()

option(HALCYON_ENABLE_BASISU "Build Basis Universal texture tooling" OFF)
if(HALCYON_ENABLE_BASISU)
    set(BASISU_STATIC ON CACHE BOOL "" FORCE)
    set(BASISU_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BASISU_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
    add_subdirectory(basisu EXCLUDE_FROM_ALL)
    add_library(Halcyon::BasisU ALIAS basisu_encoder)
endif()

option(HALCYON_ENABLE_MESHOPTIMIZER "Build meshoptimizer" OFF)
if(HALCYON_ENABLE_MESHOPTIMIZER)
    set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
    set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
    set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(meshoptimizer EXCLUDE_FROM_ALL)
    add_library(Halcyon::Meshoptimizer ALIAS meshoptimizer)
endif()

option(HALCYON_ENABLE_DRACO "Build Draco geometry compression" OFF)
if(HALCYON_ENABLE_DRACO)
    set(DRACO_TESTS OFF CACHE BOOL "" FORCE)
    set(DRACO_BUILD_EXECUTABLES OFF CACHE BOOL "" FORCE)
    set(DRACO_TRANSCODER_SUPPORTED OFF CACHE BOOL "" FORCE)
    if(POLICY CMP0148)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0148 OLD)
    endif()
    add_subdirectory(draco EXCLUDE_FROM_ALL)
    if(POLICY CMP0148)
        cmake_policy(POP)
    endif()
    if(TARGET draco::draco)
        add_library(HalcyonDraco INTERFACE)
        target_link_libraries(HalcyonDraco INTERFACE draco::draco)
        add_library(Halcyon::Draco ALIAS HalcyonDraco)
    endif()
endif()

option(HALCYON_ENABLE_BENCHMARK "Build Google Benchmark" OFF)
if(HALCYON_ENABLE_BENCHMARK)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    add_subdirectory(benchmark EXCLUDE_FROM_ALL)
    add_library(HalcyonBenchmark INTERFACE)
    target_link_libraries(HalcyonBenchmark INTERFACE benchmark::benchmark)
    add_library(Halcyon::Benchmark ALIAS HalcyonBenchmark)
endif()

option(HALCYON_ENABLE_ZSTD "Build Zstandard" OFF)
if(HALCYON_ENABLE_ZSTD)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    add_subdirectory(zstd/build/cmake EXCLUDE_FROM_ALL)
    if(TARGET libzstd_static)
        add_library(Halcyon::Zstd ALIAS libzstd_static)
    elseif(TARGET libzstd)
        add_library(Halcyon::Zstd ALIAS libzstd)
    endif()
endif()
