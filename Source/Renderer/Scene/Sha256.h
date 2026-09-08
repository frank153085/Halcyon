#pragma once

#include "Core/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace Halcyon::Renderer::Scene
{

using Sha256Digest = std::array<std::uint8_t, 32>;

[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] Halcyon::Result<Sha256Digest> sha256File(const std::filesystem::path& path);
[[nodiscard]] std::string sha256Hex(const Sha256Digest& digest);

} // namespace Halcyon::Renderer::Scene
