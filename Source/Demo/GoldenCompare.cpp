#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace
{
struct Image
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = nullptr;

    ~Image() { stbi_image_free(pixels); }
};

[[nodiscard]] bool load(const char* path, Image& image)
{
    image.pixels = stbi_load(path, &image.width, &image.height, &image.channels, 4);
    return image.pixels != nullptr && image.width > 0 && image.height > 0;
}

[[nodiscard]] double ssim(const Image& a, const Image& b)
{
    if (a.width != b.width || a.height != b.height) return 0.0;
    const std::size_t count = static_cast<std::size_t>(a.width) * a.height;
    double meanA = 0.0;
    double meanB = 0.0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t p = i * 4u;
        const double la = (0.2126 * a.pixels[p] + 0.7152 * a.pixels[p + 1] +
                           0.0722 * a.pixels[p + 2]) / 255.0;
        const double lb = (0.2126 * b.pixels[p] + 0.7152 * b.pixels[p + 1] +
                           0.0722 * b.pixels[p + 2]) / 255.0;
        meanA += la;
        meanB += lb;
    }
    meanA /= static_cast<double>(count);
    meanB /= static_cast<double>(count);
    double varianceA = 0.0;
    double varianceB = 0.0;
    double covariance = 0.0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t p = i * 4u;
        const double la = (0.2126 * a.pixels[p] + 0.7152 * a.pixels[p + 1] +
                           0.0722 * a.pixels[p + 2]) / 255.0;
        const double lb = (0.2126 * b.pixels[p] + 0.7152 * b.pixels[p + 1] +
                           0.0722 * b.pixels[p + 2]) / 255.0;
        varianceA += (la - meanA) * (la - meanA);
        varianceB += (lb - meanB) * (lb - meanB);
        covariance += (la - meanA) * (lb - meanB);
    }
    const double n = std::max(1.0, static_cast<double>(count - 1u));
    varianceA /= n;
    varianceB /= n;
    covariance /= n;
    constexpr double c1 = 0.01 * 0.01;
    constexpr double c2 = 0.03 * 0.03;
    return ((2.0 * meanA * meanB + c1) * (2.0 * covariance + c2)) /
           ((meanA * meanA + meanB * meanB + c1) *
               (varianceA + varianceB + c2));
}
} // namespace

int main(int argc, char** argv)
{
    std::string actual;
    std::string golden;
    double threshold = 0.995;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i] != nullptr ? argv[i] : "";
        if ((arg == "--actual" || arg == "-a") && i + 1 < argc) actual = argv[++i];
        else if ((arg == "--golden" || arg == "-g") && i + 1 < argc) golden = argv[++i];
        else if ((arg == "--threshold" || arg == "-t") && i + 1 < argc) threshold = std::atof(argv[++i]);
        else if (arg == "--help" || arg == "-h")
        {
            std::printf("Usage: HalcyonGoldenCompare --actual image.png --golden image.png [--threshold 0.995]\n");
            return EXIT_SUCCESS;
        }
    }
    if (actual.empty() || golden.empty())
    {
        std::fprintf(stderr, "Both --actual and --golden are required.\n");
        return EXIT_FAILURE;
    }
    Image a;
    Image b;
    if (!load(actual.c_str(), a) || !load(golden.c_str(), b))
    {
        std::fprintf(stderr, "Failed to decode one of the PNG files.\n");
        return EXIT_FAILURE;
    }
    const double score = ssim(a, b);
    std::printf("SSIM %.6f (threshold %.6f)\n", score, threshold);
    return score + 1.0e-12 >= threshold ? EXIT_SUCCESS : EXIT_FAILURE;
}
