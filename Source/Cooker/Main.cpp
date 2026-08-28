// HalcyonCooker is intentionally small in the learning repository.  It does
// not try to replace a full glTF toolchain; it provides a deterministic,
// dependency-free resource manifest that later meshopt/fastgltf stages can
// extend without changing the runtime cache contract.

#include "Core/Result.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace
{

struct FileRecord
{
    std::string relativePath;
    std::uint64_t byteCount = 0;
    std::uint64_t hash = 0;
};

[[nodiscard]] std::uint64_t fnv1a(std::istream& input)
{
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;
    std::array<char, 16 * 1024> bytes{};
    while (input)
    {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const auto count = input.gcount();
        for (std::streamsize i = 0; i < count; ++i)
        {
            hash ^= static_cast<std::uint8_t>(bytes[static_cast<std::size_t>(i)]);
            hash *= prime;
        }
    }
    return hash;
}

Halcyon::Result<std::vector<FileRecord>> collect(const fs::path& root)
{
    std::error_code error;
    if (!fs::exists(root, error) || error || !fs::is_directory(root, error))
    {
        return Halcyon::Result<std::vector<FileRecord>>::failure(Halcyon::MakeError(
            Halcyon::ErrorCode::NotFound, "input directory does not exist", root.string()));
    }

    std::vector<FileRecord> records;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                             error), end;
         it != end && !error; it.increment(error))
    {
        if (!it->is_regular_file(error) || error)
        {
            continue;
        }
        std::ifstream stream(it->path(), std::ios::binary);
        if (!stream)
        {
            return Halcyon::Result<std::vector<FileRecord>>::failure(Halcyon::MakeError(
                Halcyon::ErrorCode::Io, "unable to read asset", it->path().string()));
        }
        std::error_code sizeError;
        const auto size = fs::file_size(it->path(), sizeError);
        if (sizeError)
        {
            return Halcyon::Result<std::vector<FileRecord>>::failure(Halcyon::MakeError(
                Halcyon::ErrorCode::Io, "unable to stat asset", it->path().string()));
        }
        records.push_back(FileRecord{
            fs::relative(it->path(), root, sizeError).generic_string(),
            static_cast<std::uint64_t>(size), fnv1a(stream)});
    }
    if (error)
    {
        return Halcyon::Result<std::vector<FileRecord>>::failure(Halcyon::MakeError(
            Halcyon::ErrorCode::Io, "unable to enumerate input directory", error.message()));
    }
    std::sort(records.begin(), records.end(), [](const FileRecord& lhs, const FileRecord& rhs) {
        return lhs.relativePath < rhs.relativePath;
    });
    return Halcyon::Ok(std::move(records));
}

Halcyon::Result<void> writeManifest(const fs::path& output,
                                    const fs::path& root,
                                    const std::vector<FileRecord>& records)
{
    const fs::path temporary = output.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        return Halcyon::Err(Halcyon::MakeError(
            Halcyon::ErrorCode::Io, "unable to create manifest", temporary.string()));
    }
    stream << "{\n  \"format\": 1,\n  \"root\": \"" << root.generic_string()
           << "\",\n  \"files\": [\n";
    for (std::size_t i = 0; i < records.size(); ++i)
    {
        const auto& record = records[i];
        stream << "    {\"path\": \"" << record.relativePath << "\", \"bytes\": "
               << record.byteCount << ", \"fnv1a64\": \"0x" << std::hex
               << std::setw(16) << std::setfill('0') << record.hash << std::dec
               << "\"}" << (i + 1u == records.size() ? "\n" : ",\n");
    }
    stream << "  ]\n}\n";
    stream.close();
    if (!stream)
    {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return Halcyon::Err(Halcyon::MakeError(
            Halcyon::ErrorCode::Io, "unable to flush manifest", output.string()));
    }
    std::error_code error;
    fs::rename(temporary, output, error);
    if (error)
    {
        // Windows cannot rename over an existing file.  Remove only the
        // explicitly requested output, then retry the atomic move.
        fs::remove(output, error);
        fs::rename(temporary, output, error);
    }
    if (error)
    {
        fs::remove(temporary, error);
        return Halcyon::Err(Halcyon::MakeError(
            Halcyon::ErrorCode::Io, "unable to publish manifest", output.string()));
    }
    return Halcyon::Ok();
}

void usage()
{
    std::cout << "HalcyonCooker --input <asset-directory> --output <manifest.json>\n";
}

} // namespace

int main(int argc, char** argv)
{
    fs::path input;
    fs::path output;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument = argv[i] != nullptr ? argv[i] : "";
        if (argument == "--help" || argument == "-h")
        {
            usage();
            return 0;
        }
        if ((argument == "--input" || argument == "-i") && i + 1 < argc)
        {
            input = argv[++i];
            continue;
        }
        if ((argument == "--output" || argument == "-o") && i + 1 < argc)
        {
            output = argv[++i];
            continue;
        }
        std::cerr << "Unknown or incomplete option: " << argument << '\n';
        usage();
        return 2;
    }
    if (input.empty() || output.empty())
    {
        usage();
        return 2;
    }

    const auto records = collect(input);
    if (!records)
    {
        std::cerr << records.error().describe() << '\n';
        return 1;
    }
    const auto result = writeManifest(output, input, records.value());
    if (!result)
    {
        std::cerr << result.error().describe() << '\n';
        return 1;
    }
    std::cout << "Cooked " << records.value().size() << " files into "
              << output.string() << '\n';
    return 0;
}
