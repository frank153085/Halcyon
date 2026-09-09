#include "Sha256.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace Halcyon::Renderer::Scene
{
namespace
{

constexpr std::array<std::uint32_t, 64> k = {0x428a2f98u, 0x71374491u, 0xb5c0fbcfu,
    0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u,
    0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u,
    0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u,
    0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu,
    0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu,
    0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu,
    0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t rotate(std::uint32_t value, std::uint32_t bits) noexcept
{
    return (value >> bits) | (value << (32u - bits));
}

void processBlock(std::array<std::uint32_t, 8>& state, const std::uint8_t* block) noexcept
{
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i)
        words[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24u) |
            (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16u) |
            (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8u) | block[i * 4 + 3];
    for (std::size_t i = 16; i < words.size(); ++i)
    {
        const std::uint32_t s0 = rotate(words[i - 15], 7) ^ rotate(words[i - 15], 18) ^
            (words[i - 15] >> 3u);
        const std::uint32_t s1 = rotate(words[i - 2], 17) ^ rotate(words[i - 2], 19) ^
            (words[i - 2] >> 10u);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    auto a = state[0]; auto b = state[1]; auto c = state[2]; auto d = state[3];
    auto e = state[4]; auto f = state[5]; auto g = state[6]; auto h = state[7];
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        const std::uint32_t sum1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
        const std::uint32_t choice = (e & f) ^ (~e & g);
        const std::uint32_t temporary1 = h + sum1 + choice + k[i] + words[i];
        const std::uint32_t sum0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temporary2 = sum0 + majority;
        h = g; g = f; f = e; e = d + temporary1; d = c; c = b; b = a;
        a = temporary1 + temporary2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

struct Sha256Accumulator
{
    std::array<std::uint32_t, 8> state{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
        0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<std::uint8_t, 64> pending{};
    std::size_t pendingSize = 0;
    std::uint64_t byteCount = 0;

    void update(std::span<const std::byte> bytes) noexcept
    {
        byteCount += static_cast<std::uint64_t>(bytes.size());
        const auto* source = reinterpret_cast<const std::uint8_t*>(bytes.data());
        std::size_t remaining = bytes.size();
        if (pendingSize != 0u)
        {
            const std::size_t copied = std::min<std::size_t>(64u - pendingSize, remaining);
            if (copied != 0u)
                std::memcpy(pending.data() + pendingSize, source, copied);
            pendingSize += copied;
            source += copied;
            remaining -= copied;
            if (pendingSize == 64u)
            {
                processBlock(state, pending.data());
                pendingSize = 0u;
            }
        }
        while (remaining >= 64u)
        {
            processBlock(state, source);
            source += 64u;
            remaining -= 64u;
        }
        if (remaining != 0u)
        {
            std::memcpy(pending.data(), source, remaining);
            pendingSize = remaining;
        }
    }

    [[nodiscard]] Sha256Digest finish() noexcept
    {
        std::array<std::uint8_t, 128> tail{};
        std::memcpy(tail.data(), pending.data(), pendingSize);
        tail[pendingSize] = 0x80u;
        const std::size_t tailSize = pendingSize < 56u ? 64u : 128u;
        const std::uint64_t bitCount = byteCount * 8u;
        for (std::size_t i = 0; i < 8u; ++i)
            tail[tailSize - 1u - i] = static_cast<std::uint8_t>(bitCount >> (i * 8u));
        processBlock(state, tail.data());
        if (tailSize == 128u)
            processBlock(state, tail.data() + 64u);
        Sha256Digest result{};
        for (std::size_t i = 0; i < state.size(); ++i)
            for (std::size_t byte = 0; byte < 4u; ++byte)
                result[i * 4u + byte] =
                    static_cast<std::uint8_t>(state[i] >> (24u - byte * 8u));
        return result;
    }
};

} // namespace

Sha256Digest sha256(std::span<const std::byte> bytes) noexcept
{
    Sha256Accumulator accumulator;
    accumulator.update(bytes);
    return accumulator.finish();
}

Halcyon::Result<Sha256Digest> sha256File(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return Halcyon::Result<Sha256Digest>::failure(
        {Halcyon::ErrorCode::NotFound, "unable to open source for hashing", path.string()});
    Sha256Accumulator accumulator;
    std::array<char, 1024u * 1024u> chunk{};
    while (stream)
    {
        stream.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto count = stream.gcount();
        if (count > 0)
            accumulator.update(std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(chunk.data()),
                static_cast<std::size_t>(count)});
    }
    if (!stream.eof())
        return Halcyon::Result<Sha256Digest>::failure(
            {Halcyon::ErrorCode::Io, "unable to read source for hashing", path.string()});
    return Halcyon::Result<Sha256Digest>::success(accumulator.finish());
}

std::string sha256Hex(const Sha256Digest& digest)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto value : digest) stream << std::setw(2) << static_cast<unsigned>(value);
    return stream.str();
}

} // namespace Halcyon::Renderer::Scene
