cmake_minimum_required(VERSION 4.2)
project(Halcyon)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- 查找系统安装的Vulkan SDK ----
find_package(Vulkan REQUIRED)

# ---- GLFW ----
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(ThirdParty/glfw)

# ---- GLM ----
add_subdirectory(ThirdParty/glm)

add_executable(Halcyon Source/main.cpp)

# 关键：Vulkan模式，禁止GLFW自动引入OpenGL相关头文件
#target_compile_definitions(glfw PRIVATE GLFW_INCLUDE_VULKAN)

# 链接到你的目标
target_link_libraries(Halcyon PRIVATE
        glfw
        glm::glm
        Vulkan::Vulkan       # find_package(Vulkan)成功后会自动提供这个target
)



