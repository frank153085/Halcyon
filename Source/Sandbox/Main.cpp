#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include "Core/Log.h"
#include "Halcyon/Renderer.h"
#include "Renderer/Scene/Camera.h"

#include <GLFW/glfw3.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#ifndef HALCYON_ENABLE_VALIDATION
#define HALCYON_ENABLE_VALIDATION 1
#endif

namespace
{

void glfwErrorCallback(int error, const char* description)
{
    HALCYON_LOG_ERROR(
        "GLFW error ", error, ": ", description != nullptr ? description : "unknown error");
}

void framebufferSizeCallback(GLFWwindow* window, int width, int height)
{
    auto* renderer = static_cast<Halcyon::Vulkan::Renderer*>(glfwGetWindowUserPointer(window));
    if (renderer == nullptr)
    {
        return;
    }

    const auto result = renderer->resize({width > 0 ? static_cast<std::uint32_t>(width) : 0u,
        height > 0 ? static_cast<std::uint32_t>(height) : 0u});
    if (!result)
    {
        HALCYON_LOG_ERROR("Resize request failed: ", result.error().describe());
    }
}

void logCapabilities(const Halcyon::Vulkan::Capabilities& capabilities)
{
    HALCYON_LOG_INFO("GPU: ",
        capabilities.deviceName,
        " (Vulkan ",
        VK_API_VERSION_MAJOR(capabilities.deviceApiVersion),
        '.',
        VK_API_VERSION_MINOR(capabilities.deviceApiVersion),
        '.',
        VK_API_VERSION_PATCH(capabilities.deviceApiVersion),
        ')');
    HALCYON_LOG_INFO("Base tier: dynamicRendering=",
        capabilities.dynamicRendering,
        ", synchronization2=",
        capabilities.synchronization2,
        ", timelineSemaphore=",
        capabilities.timelineSemaphore);
    HALCYON_LOG_INFO("GPU-driven tier: descriptorIndexing=",
        capabilities.descriptorIndexing,
        ", bufferDeviceAddress=",
        capabilities.bufferDeviceAddress,
        ", indirectCount=",
        capabilities.indirectCount,
        ", barycentric=",
        capabilities.fragmentBarycentric);
    HALCYON_LOG_INFO("Optional ray query: ", capabilities.rayQuery);
}

} // namespace

namespace
{

struct CommandLine
{
    int width = 1280;
    int height = 720;
    std::uint64_t frameLimit = 0; // zero means run until the window is closed.
    bool validation = HALCYON_ENABLE_VALIDATION != 0;
    bool resourceTest = false;
    bool help = false;
};

bool parseUnsigned(std::string_view text, std::uint64_t& value)
{
    if (text.empty())
    {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const char character : text)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u)
        {
            return false;
        }
        parsed = parsed * 10u + digit;
    }
    value = parsed;
    return true;
}

bool parseCommandLine(int argc, char** argv, CommandLine& commandLine)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index] != nullptr ? argv[index] : "";
        if (argument == "--help" || argument == "-h")
        {
            commandLine.help = true;
            continue;
        }

        const auto consumeValue = [&](std::string_view name, std::uint64_t& output)
        {
            if (argument == name && index + 1 < argc)
            {
                return parseUnsigned(argv[++index] != nullptr ? argv[index] : "", output);
            }
            if (argument.starts_with(name) && argument.size() > name.size() &&
                argument[name.size()] == '=')
            {
                return parseUnsigned(argument.substr(name.size() + 1u), output);
            }
            return false;
        };

        std::uint64_t value = 0;
        if (consumeValue("--frames", value))
        {
            commandLine.frameLimit = value;
            continue;
        }
        if (consumeValue("--width", value) && value > 0 && value <= 16384u)
        {
            commandLine.width = static_cast<int>(value);
            continue;
        }
        if (consumeValue("--height", value) && value > 0 && value <= 16384u)
        {
            commandLine.height = static_cast<int>(value);
            continue;
        }
        if (argument == "--no-validation" || argument == "--validation=0")
        {
            commandLine.validation = false;
            continue;
        }
        if (argument == "--validation" || argument == "--validation=1")
        {
            commandLine.validation = true;
            continue;
        }
        if (argument == "--resource-test")
        {
            commandLine.resourceTest = true;
            continue;
        }

        HALCYON_LOG_ERROR("Unknown or malformed command-line option: ", argument);
        return false;
    }
    return true;
}

void printUsage()
{
    std::printf("Halcyon sandbox options:\n"
                "  --frames N       render N frames and exit (default: until close)\n"
                "  --width N        initial window width (default: 1280)\n"
                "  --height N       initial window height (default: 720)\n"
                "  --no-validation  disable Vulkan validation layers\n"
                "  --resource-test  load the sample OBJ and PNG before playback\n"
                "  --help           show this message\n");
}

} // namespace

int main(int argc, char** argv)
{
    CommandLine commandLine;
    if (!parseCommandLine(argc, argv, commandLine))
    {
        printUsage();
        return EXIT_FAILURE;
    }
    if (commandLine.help)
    {
        printUsage();
        return EXIT_SUCCESS;
    }

    glfwSetErrorCallback(glfwErrorCallback);
    if (glfwInit() != GLFW_TRUE)
    {
        HALCYON_LOG_CRITICAL("GLFW initialization failed");
        return EXIT_FAILURE;
    }

    if (glfwVulkanSupported() != GLFW_TRUE)
    {
        HALCYON_LOG_CRITICAL("The active driver/loader does not support Vulkan");
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow* window =
        glfwCreateWindow(commandLine.width, commandLine.height, "Halcyon M1", nullptr, nullptr);
    if (window == nullptr)
    {
        HALCYON_LOG_CRITICAL("Window creation failed");
        glfwTerminate();
        return EXIT_FAILURE;
    }

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    Halcyon::Vulkan::Renderer renderer;
    Halcyon::Vulkan::RendererConfig config;
    config.initialExtent = {
        framebufferWidth > 0 ? static_cast<std::uint32_t>(framebufferWidth) : 1280u,
        framebufferHeight > 0 ? static_cast<std::uint32_t>(framebufferHeight) : 720u};
    config.framesInFlight = 3;
    config.enableValidation = commandLine.validation;
    // Ray Query belongs to the optional M6 experiment.  Keep the M0/M1
    // sandbox on the portable base tier even when the GPU advertises RT.
    config.rayQuery = Halcyon::Vulkan::FeatureMode::Disabled;
    config.startupTexturePath = "assets/models/monkey/color.png";
    config.startupMeshPath = "assets/models/monkey/monkey.obj";

    const auto initializeResult = renderer.initialize(window, config);
    if (!initializeResult)
    {
        HALCYON_LOG_CRITICAL(
            "Renderer initialization failed: ", initializeResult.error().describe());
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwSetWindowUserPointer(window, &renderer);
    glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);
    logCapabilities(renderer.capabilities());
    if (commandLine.resourceTest)
    {
        HALCYON_LOG_INFO("Resource playback mode is enabled; assets are loaded by the renderer.");
    }

    // The sandbox owns example-specific scene policy.  The renderer only
    // consumes these values through FramePacket and remains independent of
    // this camera placement, projection choice, and animation speed.
    Halcyon::Renderer::Scene::Camera camera;
    Halcyon::Renderer::Scene::Perspective perspective{};
    perspective.verticalFovRadians = glm::radians(55.0f);
    perspective.nearPlane = 0.1f;
    perspective.farPlane = 100.0f;
    const auto cameraPerspectiveResult = camera.setPerspective(perspective);
    const auto cameraViewportResult = camera.setViewport(
        Halcyon::Renderer::Scene::ViewportExtent{config.initialExtent.width,
            config.initialExtent.height});
    const auto cameraLookAtResult = camera.lookAt(
        glm::vec3{0.0f, 0.15f, 3.2f}, glm::vec3{0.0f, 0.0f, 0.0f});
    if (!cameraPerspectiveResult || !cameraViewportResult || !cameraLookAtResult)
    {
        HALCYON_LOG_CRITICAL("Failed to initialize sandbox camera");
        glfwSetWindowUserPointer(window, nullptr);
        renderer.shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    std::uint64_t frameIndex = 0;
    Halcyon::Vulkan::InstanceData instance{};
    const float rotationSpeedRadiansPerSecond = glm::radians(30.0f);
    const auto playbackStart = std::chrono::steady_clock::now();
    int exitCode = EXIT_SUCCESS;
    std::string reportedError;
    while (glfwWindowShouldClose(window) == GLFW_FALSE &&
           (commandLine.frameLimit == 0 || frameIndex < commandLine.frameLimit))
    {
        glfwPollEvents();

        Halcyon::Vulkan::FramePacket packet;
        packet.frameIndex = frameIndex++;
        int currentFramebufferWidth = 0;
        int currentFramebufferHeight = 0;
        glfwGetFramebufferSize(window, &currentFramebufferWidth, &currentFramebufferHeight);
        if (currentFramebufferWidth > 0 && currentFramebufferHeight > 0)
        {
            (void)camera.setViewport({static_cast<std::uint32_t>(currentFramebufferWidth),
                static_cast<std::uint32_t>(currentFramebufferHeight)});
        }
        packet.camera = camera.data();

        // Keep the sample animation in the application layer.  The transform
        // is serialized into the backend-neutral InstanceData layout.
        const double elapsedSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - playbackStart).count();
        const float angle = static_cast<float>(elapsedSeconds) * rotationSpeedRadiansPerSecond;
        const glm::mat4 model = glm::rotate(
            glm::mat4{1.0f}, angle, glm::vec3{0.0f, 1.0f, 0.0f});
        std::memcpy(instance.transform.data(), glm::value_ptr(model), sizeof(model));
        packet.instances = std::span<const Halcyon::Vulkan::InstanceData>{&instance, 1};
        const Halcyon::Vulkan::FrameStats stats = renderer.render(packet);

        if (stats.deviceLost)
        {
            HALCYON_LOG_CRITICAL("Vulkan device lost: ", renderer.lastError());
            exitCode = EXIT_FAILURE;
            break;
        }
        if (stats.fatalError || !renderer.initialized())
        {
            HALCYON_LOG_CRITICAL("Renderer entered a fatal state: ", renderer.lastError());
            exitCode = EXIT_FAILURE;
            break;
        }
        if (!renderer.lastError().empty() && renderer.lastError() != reportedError)
        {
            reportedError = renderer.lastError();
            HALCYON_LOG_ERROR("Renderer: ", reportedError);
        }

        if (stats.minimized)
        {
            glfwWaitEventsTimeout(0.05);
        }

        if (stats.rendered && frameIndex % 120u == 0u)
        {
            char title[160]{};
            if (stats.gpuFrameMs >= 0.0)
            {
                std::snprintf(title,
                    sizeof(title),
                    "Halcyon M1 | CPU %.2f ms | GPU %.2f ms",
                    stats.cpuFrameMs,
                    stats.gpuFrameMs);
            }
            else
            {
                std::snprintf(title, sizeof(title), "Halcyon M1 | CPU %.2f ms", stats.cpuFrameMs);
            }
            glfwSetWindowTitle(window, title);
        }
    }

    glfwSetWindowUserPointer(window, nullptr);
    renderer.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return exitCode;
}
