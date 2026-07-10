#define GLFW_INCLUDE_VULKAN   // 让GLFW自动include vulkan.h，必须在glfw3.h之前定义
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <cstdio>
#include <vector>
#include <cstring>

int main()
{
    // ---------- 1. 初始化GLFW，创建窗口（不创建OpenGL上下文） ----------
    if (!glfwInit())
    {
        printf("GLFW初始化失败\n");
        return -1;
    }

    if (!glfwVulkanSupported())
    {
        printf("当前环境不支持Vulkan\n");
        glfwTerminate();
        return -1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Halcyon - Vulkan Test", nullptr, nullptr);
    if (!window)
    {
        printf("窗口创建失败\n");
        glfwTerminate();
        return -1;
    }

    // ---------- 2. 创建Vulkan Instance ----------
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Halcyon";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Halcyon Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    // 获取GLFW要求的扩展（跨平台窗口系统集成必须的扩展，如VK_KHR_surface等）
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = glfwExtensionCount;
    createInfo.ppEnabledExtensionNames = glfwExtensions;
    createInfo.enabledLayerCount = 0;   // 先不开验证层，最小化测试

    VkInstance instance;
    VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);

    if (result != VK_SUCCESS)
    {
        printf("Vulkan Instance创建失败, VkResult = %d\n", result);
        glfwDestroyWindow(window);
        glfwTerminate();
        return -1;
    }

    printf("Vulkan Instance创建成功!\n");

    // ---------- 3. 查询物理设备（GPU），验证Vulkan能识别到显卡 ----------
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

    if (deviceCount == 0)
    {
        printf("没有找到支持Vulkan的GPU\n");
    }
    else
    {
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        printf("找到 %u 个支持Vulkan的GPU:\n", deviceCount);
        for (const auto& device : devices)
        {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);
            printf("  - %s\n", props.deviceName);
        }
    }

    // ---------- 4. 创建Window Surface（验证GLFW与Vulkan的窗口系统集成） ----------
    VkSurfaceKHR surface;
    result = glfwCreateWindowSurface(instance, window, nullptr, &surface);
    if (result != VK_SUCCESS)
    {
        printf("Surface创建失败, VkResult = %d\n", result);
    }
    else
    {
        printf("Surface创建成功!\n");
    }

    // ---------- 5. 消息循环 ----------
    printf("进入消息循环（关闭窗口退出）\n");
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
    }

    // ---------- 6. 清理 ----------
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    glfwDestroyWindow(window);
    glfwTerminate();

    printf("测试完成，正常退出\n");
    return 0;
}
