#pragma once

#include "FrameGraphTypes.h"

#include <compare>

namespace Halcyon::Renderer::Graph
{

class FrameGraphHandle
{
public:
    using Index = std::uint32_t;
    using Version = std::uint32_t;
    constexpr FrameGraphHandle() noexcept = default;
    constexpr FrameGraphHandle(Index index, Version version, std::uint32_t epoch = 1) noexcept
            : index_(index),
              version_(version),
              epoch_(epoch)
    {
    }
    static constexpr FrameGraphHandle invalid() noexcept
    {
        return {};
    }
    bool isInitialized() const noexcept
    {
        return index_ != kInvalidIndex && version_ != 0;
    }
    bool valid() const noexcept
    {
        return isInitialized();
    }
    Index index() const noexcept
    {
        return index_;
    }
    Version version() const noexcept
    {
        return version_;
    }
    Version generation() const noexcept
    {
        return version_;
    }
    std::uint32_t epoch() const noexcept
    {
        return epoch_;
    }
    std::uint64_t packed() const noexcept
    {
        return (static_cast<std::uint64_t>(epoch_) << 48u) |
               (static_cast<std::uint64_t>(version_) << 32u) | index_;
    }
    void clear() noexcept
    {
        index_ = kInvalidIndex;
        version_ = 0;
        epoch_ = 0;
    }
    constexpr friend bool operator==(FrameGraphHandle, FrameGraphHandle) noexcept = default;
    constexpr friend auto operator<=>(FrameGraphHandle, FrameGraphHandle) noexcept = default;

protected:
    Index index_ = kInvalidIndex;
    Version version_ = 0;
    std::uint32_t epoch_ = 0;
};

template <typename Resource>
class FrameGraphId : public FrameGraphHandle
{
public:
    using ResourceType = Resource;
    constexpr FrameGraphId() noexcept = default;
    constexpr FrameGraphId(Index index, Version version, std::uint32_t epoch = 1) noexcept
            : FrameGraphHandle(index, version, epoch)
    {
    }
    static constexpr FrameGraphId invalid() noexcept
    {
        return {};
    }
};

} // namespace Halcyon::Renderer::Graph

namespace std
{
template <>
struct hash<Halcyon::Renderer::Graph::FrameGraphHandle>
{
    size_t operator()(const Halcyon::Renderer::Graph::FrameGraphHandle& h) const noexcept
    {
        return static_cast<size_t>(h.packed() ^ (h.packed() >> 32u));
    }
};
template <typename Resource>
struct hash<Halcyon::Renderer::Graph::FrameGraphId<Resource>>
{
    size_t operator()(const Halcyon::Renderer::Graph::FrameGraphId<Resource>& h) const noexcept
    {
        return std::hash<Halcyon::Renderer::Graph::FrameGraphHandle>{}(h);
    }
};
} // namespace std
