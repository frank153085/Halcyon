#pragma once

// Generation checked handles.  A tag makes handles for different resource
// types non-interchangeable at compile time (e.g. TextureHandle cannot be
// passed where a MeshHandle is expected).

#include <cstddef>
#include <compare>
#include <cstdint>
#include <functional>
#include <limits>
#include <type_traits>

namespace Halcyon::Core
{

struct DefaultHandleTag
{
};

template <typename Tag,
          typename IndexT = std::uint32_t,
          typename GenerationT = std::uint32_t>
class Handle
{
    static_assert(std::is_integral_v<IndexT> && std::is_unsigned_v<IndexT>,
                  "Handle index type must be an unsigned integer");
    static_assert(std::is_integral_v<GenerationT> && std::is_unsigned_v<GenerationT>,
                  "Handle generation type must be an unsigned integer");

public:
    using tag_type = Tag;
    using index_type = IndexT;
    using generation_type = GenerationT;

    static constexpr index_type kInvalidIndex = std::numeric_limits<index_type>::max();
    // Generation zero is deliberately reserved for invalid handles.  New
    // slots start at one and recycled slots skip zero on wrap-around.
    static constexpr generation_type kInvalidGeneration = generation_type{0};

    constexpr Handle() noexcept = default;

    constexpr Handle(index_type index, generation_type generation) noexcept
        : index_(index), generation_(generation)
    {
    }

    [[nodiscard]] static constexpr Handle invalid() noexcept { return {}; }

    [[nodiscard]] static constexpr Handle fromParts(index_type index,
                                                    generation_type generation) noexcept
    {
        return Handle{index, generation};
    }

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return index_ != kInvalidIndex && generation_ != kInvalidGeneration;
    }
    [[nodiscard]] constexpr bool valid() const noexcept { return isValid(); }
    explicit constexpr operator bool() const noexcept { return isValid(); }

    [[nodiscard]] constexpr index_type index() const noexcept { return index_; }
    [[nodiscard]] constexpr generation_type generation() const noexcept { return generation_; }
    [[nodiscard]] constexpr index_type slot() const noexcept { return index_; }

    /** Pack the two fields into a stable 64-bit value for diagnostics/maps. */
    [[nodiscard]] constexpr std::uint64_t packed() const noexcept
    {
        constexpr unsigned indexBits = static_cast<unsigned>(sizeof(index_type) * 8u);
        constexpr unsigned generationBits = static_cast<unsigned>(sizeof(generation_type) * 8u);
        static_assert(indexBits + generationBits <= 64u,
                      "Handle fields must fit in 64 bits to use packed()");
        return (static_cast<std::uint64_t>(generation_) << indexBits) |
               static_cast<std::uint64_t>(index_);
    }

    friend constexpr bool operator==(Handle, Handle) noexcept = default;
    friend constexpr auto operator<=>(Handle, Handle) noexcept = default;

private:
    index_type index_{kInvalidIndex};
    generation_type generation_{kInvalidGeneration};
};

template <typename HandleT>
struct HandleTraits;

template <typename Tag, typename IndexT, typename GenerationT>
struct HandleTraits<Handle<Tag, IndexT, GenerationT>>
{
    using handle_type = Handle<Tag, IndexT, GenerationT>;
    using tag_type = Tag;
    using index_type = IndexT;
    using generation_type = GenerationT;
};

template <typename T>
struct IsHandle : std::false_type
{
};

template <typename Tag, typename IndexT, typename GenerationT>
struct IsHandle<Handle<Tag, IndexT, GenerationT>> : std::true_type
{
};

template <typename T>
inline constexpr bool IsHandleV = IsHandle<T>::value;

} // namespace Halcyon::Core

namespace std
{
template <typename Tag, typename IndexT, typename GenerationT>
struct hash<Halcyon::Core::Handle<Tag, IndexT, GenerationT>>
{
    [[nodiscard]] std::size_t operator()(
        const Halcyon::Core::Handle<Tag, IndexT, GenerationT>& handle) const noexcept
    {
        // Hash-combine the fields rather than truncating packed() on 32-bit
        // platforms.
        const std::size_t indexHash = static_cast<std::size_t>(handle.index());
        const std::size_t generationHash = static_cast<std::size_t>(handle.generation());
        return indexHash ^ (generationHash + static_cast<std::size_t>(0x9e3779b9u) +
                            (indexHash << 6u) + (indexHash >> 2u));
    }
};
} // namespace std

namespace Halcyon
{
using Core::DefaultHandleTag;
using Core::Handle;
using Core::HandleTraits;
using Core::IsHandle;
template <typename T>
inline constexpr bool IsHandleV = Core::IsHandleV<T>;
} // namespace Halcyon
