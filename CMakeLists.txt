cmake_minimum_required(VERSION 4.2)
project(Halcyon)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)



# 关闭GLFW不需要的功能，减少编译时间
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory(ThirdParty/glfw)

add_executable(Halcyon Source/main.cpp)

# 关键：Vulkan模式，禁止GLFW自动引入OpenGL相关头文件
#target_compile_definitions(glfw PRIVATE GLFW_INCLUDE_VULKAN)

# 链接到你的目标
target_link_libraries(Halcyon PRIVATE glfw)



