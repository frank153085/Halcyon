#define GLFW_INCLUDE_NONE   // 禁止GLFW自动包含任何图形API头文件（OpenGL/Vulkan都不要）
#include <GLFW/glfw3.h>
#include <cstdio>

int main()
{
    if (!glfwInit())
    {
        printf("GLFW初始化失败\n");
        return -1;
    }

    printf("GLFW版本: %s\n", glfwGetVersionString());

    // 不创建任何图形上下文，纯窗口测试
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Halcyon - GLFW Test", nullptr, nullptr);
    if (!window)
    {
        printf("窗口创建失败\n");
        glfwTerminate();
        return -1;
    }

    printf("窗口创建成功，进入消息循环（关闭窗口退出）\n");

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
    }

    glfwDestroyWindow(window);
    glfwTerminate();

    printf("测试完成，正常退出\n");
    return 0;
}
