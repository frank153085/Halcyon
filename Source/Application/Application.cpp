#include "Halcyon/Application.h"

#include "Core/Log.h"
#include "DiagnosticsOverlay.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <string_view>

#ifndef HALCYON_ENABLE_VALIDATION
#define HALCYON_ENABLE_VALIDATION 1
#endif

namespace Halcyon
{
namespace
{

[[nodiscard]] bool parseUnsigned(std::string_view text, std::uint64_t& value) noexcept
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

void printUsage() noexcept
{
    std::printf("Halcyon options:\n"
                "  --frames N       render N frames and exit (default: until close)\n"
                "  --width N        initial window width (default: 1280)\n"
                "  --height N       initial window height (default: 720)\n"
                "  --no-validation  disable Vulkan validation layers\n"
                "  --validation     enable Vulkan validation layers\n"
                "  --help           show this message\n");
}

[[nodiscard]] bool parseCommandLine(int argc, char** argv, ApplicationConfig& config) noexcept
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index] != nullptr ? argv[index] : "";
        if (argument == "--help" || argument == "-h")
        {
            printUsage();
            return false;
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
            config.frameLimit = value;
            continue;
        }
        if (consumeValue("--width", value) && value > 0 && value <= 16384u)
        {
            config.window.initialExtent.width = static_cast<std::uint32_t>(value);
            continue;
        }
        if (consumeValue("--height", value) && value > 0 && value <= 16384u)
        {
            config.window.initialExtent.height = static_cast<std::uint32_t>(value);
            continue;
        }
        if (argument == "--no-validation" || argument == "--validation=0")
        {
            config.engine.enableValidation = false;
            continue;
        }
        if (argument == "--validation" || argument == "--validation=1")
        {
            config.engine.enableValidation = true;
            continue;
        }
        // Kept as a harmless compatibility switch for the early examples.
        if (argument == "--resource-test")
        {
            continue;
        }

        HALCYON_LOG_ERROR("Unknown or malformed command-line option: ", argument);
        return false;
    }
    return true;
}

} // namespace

int Application::run(
    int argc, char** argv, ApplicationConfig config, ApplicationCallbacks callbacks)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index] != nullptr ? argv[index] : "";
        if (argument == "--help" || argument == "-h")
        {
            printUsage();
            return EXIT_SUCCESS;
        }
    }
    if (!parseCommandLine(argc, argv, config))
    {
        return EXIT_FAILURE;
    }

    if (config.window.initialExtent.empty())
    {
        config.window.initialExtent = {1280, 720};
    }
    if (config.window.title.empty())
    {
        config.window.title = "Halcyon";
    }
    if (config.engine.framesInFlight == 0)
    {
        config.engine.framesInFlight = 1;
    }

    auto windowResult = Platform::Window::create(config.window);
    if (!windowResult)
    {
        HALCYON_LOG_CRITICAL("Window creation failed: ", windowResult.error().describe());
        return EXIT_FAILURE;
    }
    auto window = std::move(windowResult.value());

    auto engineResult = Engine::create(*window, config.engine);
    if (!engineResult)
    {
        HALCYON_LOG_CRITICAL("Engine creation failed: ", engineResult.error().describe());
        return EXIT_FAILURE;
    }
    auto engine = std::move(engineResult.value());

    ApplicationInternal::DiagnosticsOverlay diagnostics;
    if (config.enableDiagnostics)
    {
        const auto diagnosticsResult = diagnostics.initialize(*window, *engine);
        if (!diagnosticsResult)
        {
            HALCYON_LOG_WARN(
                "Diagnostics overlay unavailable: ", diagnosticsResult.error().describe());
        }
    }

    bool initializedCallback = true;
    int exitCode = EXIT_SUCCESS;
    if (callbacks.onInitialize)
    {
        try
        {
            const auto initialize = callbacks.onInitialize(*engine);
            if (!initialize)
            {
                HALCYON_LOG_CRITICAL(
                    "Application initialization failed: ", initialize.error().describe());
                initializedCallback = false;
                exitCode = EXIT_FAILURE;
            }
        }
        catch (const std::exception& exception)
        {
            HALCYON_LOG_CRITICAL(
                "Application initialization threw an exception: ", exception.what());
            initializedCallback = false;
            exitCode = EXIT_FAILURE;
        }
        catch (...)
        {
            HALCYON_LOG_CRITICAL("Application initialization threw an unknown exception");
            initializedCallback = false;
            exitCode = EXIT_FAILURE;
        }
    }

    Extent2D previousExtent = window->framebufferExtent();
    std::uint64_t frameIndex = 0;
    double elapsedSeconds = 0.0;
    auto previousTime = std::chrono::steady_clock::now();
    FrameStats previousStats{};

    while (exitCode == EXIT_SUCCESS && !window->shouldClose() &&
           (config.frameLimit == 0 || frameIndex < config.frameLimit))
    {
        window->pollEvents();
        const Extent2D extent = window->framebufferExtent();
        const bool minimized = extent.empty();
        if (extent.width != previousExtent.width || extent.height != previousExtent.height)
        {
            const auto resizeResult = engine->resize(extent);
            if (!resizeResult)
            {
                HALCYON_LOG_ERROR("Resize failed: ", resizeResult.error().describe());
                exitCode = EXIT_FAILURE;
                break;
            }
            previousExtent = extent;
        }

        const auto now = std::chrono::steady_clock::now();
        double deltaSeconds = std::chrono::duration<double>(now - previousTime).count();
        previousTime = now;
        if (frameIndex == 0)
        {
            deltaSeconds = 0.0;
        }
        deltaSeconds = std::clamp(deltaSeconds, 0.0, 0.25);
        elapsedSeconds += deltaSeconds;

        FrameInfo frameInfo{};
        frameInfo.frameIndex = frameIndex;
        frameInfo.deltaSeconds = deltaSeconds;
        frameInfo.elapsedSeconds = elapsedSeconds;
        frameInfo.framebufferExtent = extent;
        frameInfo.input = window->input();
        frameInfo.minimized = minimized;
        if (frameInfo.input.wasKeyPressed(Platform::Key::Escape))
        {
            window->requestClose();
        }

        diagnostics.beginFrame(previousStats, frameIndex, engine->capabilities());

        if (callbacks.onFrame)
        {
            try
            {
                const auto frameResult = callbacks.onFrame(*engine, frameInfo);
                if (!frameResult)
                {
                    diagnostics.endFrame();
                    HALCYON_LOG_CRITICAL(
                        "Application frame callback failed: ", frameResult.error().describe());
                    exitCode = EXIT_FAILURE;
                    break;
                }
            }
            catch (const std::exception& exception)
            {
                diagnostics.endFrame();
                HALCYON_LOG_CRITICAL(
                    "Application frame callback threw an exception: ", exception.what());
                exitCode = EXIT_FAILURE;
                break;
            }
            catch (...)
            {
                diagnostics.endFrame();
                HALCYON_LOG_CRITICAL("Application frame callback threw an unknown exception");
                exitCode = EXIT_FAILURE;
                break;
            }
        }

        diagnostics.endFrame();

        if (!minimized)
        {
            const auto renderResult = engine->render(frameIndex);
            if (!renderResult)
            {
                HALCYON_LOG_CRITICAL("Engine render failed: ", renderResult.error().describe());
                exitCode = EXIT_FAILURE;
                break;
            }
            previousStats = renderResult.value();
        }
        else
        {
            previousStats.minimized = true;
            window->waitEventsTimeout(0.05);
        }

        if (previousStats.rendered && frameIndex % 120u == 0u)
        {
            char title[192]{};
            if (previousStats.gpuFrameMs >= 0.0)
            {
                std::snprintf(title,
                    sizeof(title),
                    "%s | CPU %.2f ms | GPU %.2f ms",
                    config.window.title.c_str(),
                    previousStats.cpuFrameMs,
                    previousStats.gpuFrameMs);
            }
            else
            {
                std::snprintf(title,
                    sizeof(title),
                    "%s | CPU %.2f ms",
                    config.window.title.c_str(),
                    previousStats.cpuFrameMs);
            }
            window->setTitle(title);
        }
        ++frameIndex;
    }

    if (initializedCallback && callbacks.onShutdown)
    {
        try
        {
            callbacks.onShutdown(*engine);
        }
        catch (const std::exception& exception)
        {
            HALCYON_LOG_ERROR(
                "Application shutdown callback threw an exception: ", exception.what());
            exitCode = EXIT_FAILURE;
        }
        catch (...)
        {
            HALCYON_LOG_ERROR("Application shutdown callback threw an unknown exception");
            exitCode = EXIT_FAILURE;
        }
    }
    diagnostics.shutdown();
    engine->shutdown();
    return exitCode;
}

} // namespace Halcyon
