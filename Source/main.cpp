#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>

int main()
{
    // ---- GLM基本测试：向量与矩阵运算 ----
    glm::vec3 a(1.0f, 2.0f, 3.0f);
    glm::vec3 b(4.0f, 5.0f, 6.0f);
    glm::vec3 sum = a + b;

    printf("GLM测试: (%.1f, %.1f, %.1f)\n", sum.x, sum.y, sum.z);

    glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    printf("GLM矩阵测试: model[3][0] = %.1f (应为1.0)\n", model[3][0]);

    // ---- 原GLFW测试逻辑 ----
    if (!glfwInit())
    {
        printf("GLFW初始化失败\n");
        return -1;
    }

    printf("GLFW版本: %s\n", glfwGetVersionString());

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Halcyon - GLFW+GLM Test", nullptr, nullptr);
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
