#include "Halcyon/Application.h"

#include "Core/Log.h"
#include "DiagnosticsOverlay.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <exception>
#include <fstream>
#include <limits>
#include <cmath>
#include <string_view>
#include <stb_image.h>

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

[[nodiscard]] bool parseFloat(std::string_view text, float& value) noexcept
{
    if (text.empty()) return false;
    std::string copy{text};
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(copy.c_str(), &end);
    if (errno != 0 || end == copy.c_str() || *end != '\0' || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

void printUsage() noexcept
{
    std::printf("Halcyon options:\n"
                "  --frames N       render N frames and exit (default: until close)\n"
                "  --width N        initial window width (default: 1280)\n"
                "  --height N       initial window height (default: 720)\n"
                "  --scene NAME     scene name (damaged-helmet or sponza)\n"
                "  --fixed-dt S     fixed simulation timestep\n"
                "  --exposure EV    HDR exposure\n"
                "  --screenshot P   write a PNG after the final frame\n"
                "  --golden P       compare the final PNG against a golden image\n"
                "  --perf-csv P     write per-frame timings\n"
                "  --no-taa         disable temporal anti-aliasing\n"
                "  --no-clustered-lighting  disable clustered lighting\n"
                "  --no-transparency         disable forward transparency\n"
                "  --no-validation  disable Vulkan validation layers\n"
                "  --validation     enable Vulkan validation layers\n"
                "  --help           show this message\n");
}

[[nodiscard]] bool compareGoldenImage(const std::filesystem::path& actualPath,
    const std::filesystem::path& goldenPath, double threshold, double& score) noexcept
{
    int widthA = 0;
    int heightA = 0;
    int channelsA = 0;
    int widthB = 0;
    int heightB = 0;
    int channelsB = 0;
    stbi_uc* actual = stbi_load(actualPath.string().c_str(), &widthA, &heightA, &channelsA, 4);
    stbi_uc* golden = stbi_load(goldenPath.string().c_str(), &widthB, &heightB, &channelsB, 4);
    if (actual == nullptr || golden == nullptr || widthA != widthB || heightA != heightB)
    {
        stbi_image_free(actual);
        stbi_image_free(golden);
        return false;
    }
    const std::size_t count = static_cast<std::size_t>(widthA) * heightA;
    double meanA = 0.0;
    double meanB = 0.0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t p = i * 4u;
        meanA += (0.2126 * actual[p] + 0.7152 * actual[p + 1] + 0.0722 * actual[p + 2]) / 255.0;
        meanB += (0.2126 * golden[p] + 0.7152 * golden[p + 1] + 0.0722 * golden[p + 2]) / 255.0;
    }
    meanA /= static_cast<double>(count);
    meanB /= static_cast<double>(count);
    double varianceA = 0.0;
    double varianceB = 0.0;
    double covariance = 0.0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t p = i * 4u;
        const double a = (0.2126 * actual[p] + 0.7152 * actual[p + 1] + 0.0722 * actual[p + 2]) / 255.0;
        const double b = (0.2126 * golden[p] + 0.7152 * golden[p + 1] + 0.0722 * golden[p + 2]) / 255.0;
        varianceA += (a - meanA) * (a - meanA);
        varianceB += (b - meanB) * (b - meanB);
        covariance += (a - meanA) * (b - meanB);
    }
    const double n = std::max(1.0, static_cast<double>(count - 1u));
    varianceA /= n;
    varianceB /= n;
    covariance /= n;
    constexpr double c1 = 0.01 * 0.01;
    constexpr double c2 = 0.03 * 0.03;
    score = ((2.0 * meanA * meanB + c1) * (2.0 * covariance + c2)) /
            ((meanA * meanA + meanB * meanB + c1) *
                (varianceA + varianceB + c2));
    stbi_image_free(actual);
    stbi_image_free(golden);
    return score >= threshold;
}

[[nodiscard]] bool parseCommandLine(int argc, char** argv, ApplicationConfig& config) noexcept
{
    bool widthSpecified = false;
    bool heightSpecified = false;
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
            widthSpecified = true;
            continue;
        }
        if (consumeValue("--height", value) && value > 0 && value <= 16384u)
        {
            config.window.initialExtent.height = static_cast<std::uint32_t>(value);
            heightSpecified = true;
            continue;
        }
        const auto consumeString = [&](std::string_view name, std::string& output)
        {
            if (argument == name && index + 1 < argc)
            {
                output = argv[++index] != nullptr ? argv[index] : "";
                return !output.empty();
            }
            if (argument.starts_with(name) && argument.size() > name.size() &&
                argument[name.size()] == '=')
            {
                output = std::string(argument.substr(name.size() + 1u));
                return !output.empty();
            }
            return false;
        };
        std::string stringValue;
        if (consumeString("--scene", stringValue)) { config.sceneName = stringValue; continue; }
        if (consumeString("--screenshot", stringValue)) { config.screenshotPath = stringValue; continue; }
        if (consumeString("--golden", stringValue)) { config.goldenPath = stringValue; continue; }
        if (consumeString("--perf-csv", stringValue)) { config.performanceCsvPath = stringValue; continue; }
        const auto consumeFloat = [&](std::string_view name, float& output)
        {
            std::string text;
            if (argument == name && index + 1 < argc) text = argv[++index] != nullptr ? argv[index] : "";
            else if (argument.starts_with(name) && argument.size() > name.size() && argument[name.size()] == '=') text = std::string(argument.substr(name.size() + 1u));
            else return false;
            return parseFloat(text, output);
        };
        float floatValue = 0.0f;
        if (consumeFloat("--fixed-dt", floatValue) && floatValue >= 0.0f && floatValue <= 1.0f)
        { config.engine.fixedDeltaSeconds = floatValue; continue; }
        if (consumeFloat("--exposure", floatValue) && floatValue >= -32.0f && floatValue <= 32.0f)
        { config.engine.exposure = floatValue; continue; }
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
        if (argument == "--no-taa") { config.engine.enableTaa = false; continue; }
        if (argument == "--no-clustered-lighting") { config.engine.enableClusteredLighting = false; continue; }
        if (argument == "--no-transparency") { config.engine.enableTransparency = false; continue; }
        // Kept as a harmless compatibility switch for the early examples.
        if (argument == "--resource-test")
        {
            continue;
        }

        HALCYON_LOG_ERROR("Unknown or malformed command-line option: ", argument);
        return false;
    }
    if (!config.goldenPath.empty() && !widthSpecified && !heightSpecified)
    {
        config.window.initialExtent = {640, 360};
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
        double deltaSeconds = config.engine.fixedDeltaSeconds > 0.0f
                                  ? static_cast<double>(config.engine.fixedDeltaSeconds)
                                  : std::chrono::duration<double>(now - previousTime).count();
        previousTime = now;
        if (frameIndex == 0 && config.engine.fixedDeltaSeconds <= 0.0f)
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
            const bool finalFrame = config.frameLimit != 0 && frameIndex + 1u >= config.frameLimit;
            if (finalFrame && (!config.screenshotPath.empty() || !config.goldenPath.empty()))
            {
                const std::filesystem::path actualPath = !config.screenshotPath.empty()
                                                              ? config.screenshotPath
                                                              : config.goldenPath.string() + ".actual.png";
                const auto capture = engine->captureScreenshot(actualPath);
                if (!capture)
                {
                    HALCYON_LOG_ERROR("Screenshot failed: ", capture.error().describe());
                    exitCode = EXIT_FAILURE;
                }
                else
                {
                    previousStats.screenshotWritten = true;
                    HALCYON_LOG_INFO("Screenshot written: ", actualPath.string());
                    if (!config.goldenPath.empty())
                    {
                        if (!std::filesystem::exists(config.goldenPath))
                        {
                            HALCYON_LOG_ERROR("Golden image does not exist: ",
                                config.goldenPath.string());
                            exitCode = EXIT_FAILURE;
                        }
                        else
                        {
                            double score = 0.0;
                            const bool passed = compareGoldenImage(
                                actualPath, config.goldenPath, 0.995, score);
                            previousStats.goldenImageCompared = true;
                            previousStats.goldenImagePassed = passed;
                            HALCYON_LOG_INFO("Golden SSIM: ", score,
                                passed ? " (PASS)" : " (FAIL, threshold 0.995)");
                            if (!passed) exitCode = EXIT_FAILURE;
                        }
                    }
                }
            }
            if (!config.performanceCsvPath.empty())
            {
                std::error_code csvDirectoryError;
                if (!config.performanceCsvPath.parent_path().empty())
                {
                    std::filesystem::create_directories(
                        config.performanceCsvPath.parent_path(), csvDirectoryError);
                }
                std::ofstream csv(config.performanceCsvPath,
                    frameIndex == 0 ? std::ios::trunc : std::ios::app);
                if (csv)
                {
                    previousStats.performanceCsvWritten = true;
                    std::string deviceName = engine->capabilities().deviceName;
                    for (char& character : deviceName)
                    {
                        if (character == ',' || character == '"' || character == '\n' ||
                            character == '\r')
                        {
                            character = '_';
                        }
                    }
                    if (frameIndex == 0)
                    {
                        csv << "frame,scene,width,height,device_name,vendor_id,device_id,"
                               "exposure,taa_enabled,clustered_lighting_enabled,"
                               "transparency_enabled,cpu_ms,gpu_ms,primitive_count,"
                               "cluster_overflow,taa_history_valid";
                        for (const auto& pass : previousStats.gpuPasses)
                        {
                            std::string name = pass.name;
                            for (char& character : name)
                            {
                                if (std::isalnum(static_cast<unsigned char>(character)))
                                {
                                    character = static_cast<char>(std::tolower(
                                        static_cast<unsigned char>(character)));
                                }
                                else
                                {
                                    character = '_';
                                }
                            }
                            csv << ",pass_" << name << "_ms";
                        }
                        csv << '\n';
                    }
                    csv << frameIndex << ',' << config.sceneName << ','
                        << config.window.initialExtent.width << ','
                        << config.window.initialExtent.height << ',' << deviceName << ','
                        << engine->capabilities().vendorId << ','
                        << engine->capabilities().deviceId << ','
                        << config.engine.exposure << ','
                        << (config.engine.enableTaa ? 1 : 0) << ','
                        << (config.engine.enableClusteredLighting ? 1 : 0) << ','
                        << (config.engine.enableTransparency ? 1 : 0) << ','
                        << previousStats.cpuFrameMs << ','
                        << previousStats.gpuFrameMs << ',' << previousStats.primitiveCount << ','
                        << previousStats.clusterOverflowCount << ','
                        << (previousStats.taaHistoryValid ? 1 : 0);
                    for (const auto& pass : previousStats.gpuPasses) csv << ',' << pass.gpuFrameMs;
                    csv << '\n';
                }
            }
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
