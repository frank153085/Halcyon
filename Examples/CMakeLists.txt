# Each example is a real executable target with its own output directory.  A
# new example therefore adds a binary instead of replacing an older one, which
# makes it possible to compare milestones side by side.
add_library(HalcyonExampleSupport STATIC
    Common/ExampleRunner.cpp
    Common/ExampleRunner.h
)
target_include_directories(HalcyonExampleSupport PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Common")
target_link_libraries(HalcyonExampleSupport PUBLIC HalcyonApplication)
target_compile_definitions(HalcyonExampleSupport PRIVATE
    HALCYON_ENABLE_VALIDATION=$<BOOL:${HALCYON_ENABLE_VALIDATION}>
    HALCYON_EXAMPLE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
set_target_properties(HalcyonExampleSupport PROPERTIES FOLDER "Examples")
halcyon_enable_warnings(HalcyonExampleSupport)

function(halcyon_add_example target source)
    add_executable(${target} "${source}")
    target_link_libraries(${target} PRIVATE HalcyonExampleSupport)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${target}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Examples/${target}"
        FOLDER "Examples")
    halcyon_enable_warnings(${target})
endfunction()

add_custom_target(HalcyonExamples)

add_subdirectory(01_Triangle)
add_subdirectory(02_TexturedModel)

add_dependencies(HalcyonExamples
    HalcyonExample01Triangle
    HalcyonExample02TexturedModel)
