#pragma once

// Backend-neutral GPU scene contract. The Vulkan implementation may mirror
// these arrays into device-local buffers, while tests and tools can exercise
// slot lifetime and incremental updates without a graphics device.

#include "Ecs/Entity.h"
#include "FramePacket.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

namespace Halcyon::Renderer::Scene
{

enum class RenderPathMode : std::uint8_t
{
    DeferredIndexed,
    GpuDrivenIndexed,
};

[[nodiscard]] inline bool sphereInsideFrustum(
    const std::array<glm::vec4, 6>& planes, const glm::vec4& sphere) noexcept
{
    for (const glm::vec4& plane : planes)
    {
        if (glm::dot(glm::vec3(plane), glm::vec3(sphere)) + plane.w + sphere.w < 0.0f)
            return false;
    }
    return true;
}

struct alignas(16) TransformRow
{
    std::array<float, 16> model{};
};

struct alignas(16) BoundsRow
{
    std::array<float, 4> sphereCenterRadius{};
    std::array<float, 4> aabbMin{};
    std::array<float, 4> aabbMax{};
};

struct MeshMaterialRow
{
    std::uint32_t meshIndex = 0;
    std::uint32_t materialIndex = 0;
    std::uint32_t flags = 0;
    std::uint32_t lodState = 0;
};

// The high bit is owned by the renderer and never comes from an ECS
// RenderableFlags value. It marks a slot that must remain on the CPU fallback
// path (for example, a mesh with no consolidated index data) while the rest
// of the frame can still use GPU-driven submission.
inline constexpr std::uint32_t kGpuSceneCpuFallbackFlag = 1u << 31u;

// Draw metadata indexes a renderer-owned consolidated vertex/index stream.
// Keeping it separate from instances lets one indirect command list contain
// arbitrary meshes without rebinding vertex or index buffers per object.
struct MeshDrawRow
{
    std::uint32_t indexCount = 0;
    std::uint32_t firstIndex = 0;
    std::int32_t vertexOffset = 0;
    std::uint32_t reserved = 0;
};
static_assert(sizeof(MeshDrawRow) == 16);

struct alignas(16) MaterialGpuData
{
    std::array<float, 4> baseColorFactor{1, 1, 1, 1};
    std::array<float, 4> emissiveFactor{0, 0, 0, 0};
    std::array<float, 4> factors{0, 1, 0.5f, 0};
    std::array<std::uint32_t, 8> textureIndices{};
};
static_assert(sizeof(MaterialGpuData) == 80);

struct MaterialSoA
{
    std::vector<MaterialGpuData> materials;
};

struct GpuSceneDirtyRange
{
    std::uint32_t first = 0;
    std::uint32_t count = 0;

    [[nodiscard]] bool empty() const noexcept
    {
        return count == 0;
    }
};

class GpuSceneSlotAllocator final
{
public:
    static constexpr std::uint32_t invalidSlot = std::numeric_limits<std::uint32_t>::max();
    struct SlotHandle
    {
        std::uint32_t index = invalidSlot;
        std::uint32_t generation = 0;
        [[nodiscard]] bool valid() const noexcept
        {
            return index != invalidSlot && generation != 0;
        }
        friend constexpr bool operator==(const SlotHandle&, const SlotHandle&) noexcept = default;
    };

    explicit GpuSceneSlotAllocator(std::uint32_t capacity = 1024)
            : generations_(capacity, 1u), states_(capacity, State::Free)
    {
        free_.reserve(capacity);
        for (std::uint32_t i = capacity; i-- > 0;)
        {
            free_.push_back(i);
        }
    }

    [[nodiscard]] std::uint32_t allocate()
    {
        (void)collect(completedTimeline_);
        if (free_.empty())
        {
            return invalidSlot;
        }
        const std::uint32_t slot = free_.back();
        free_.pop_back();
        states_[slot] = State::Live;
        return slot;
    }

    void ensureCapacity(std::uint32_t required)
    {
        const std::uint32_t oldCapacity = capacity();
        if (required <= oldCapacity) return;
        generations_.resize(required, 1u);
        states_.resize(required, State::Free);
        free_.reserve(free_.size() + required - oldCapacity);
        for (std::uint32_t slot = required; slot-- > oldCapacity;)
            free_.push_back(slot);
    }

    [[nodiscard]] SlotHandle allocateHandle()
    {
        const std::uint32_t slot = allocate();
        return slot == invalidSlot ? SlotHandle{} : SlotHandle{slot, generations_[slot]};
    }

    [[nodiscard]] bool release(std::uint32_t slot, std::uint64_t retireTimeline)
    {
        if (slot >= states_.size() || states_[slot] != State::Live)
        {
            return false;
        }
        states_[slot] = State::Pending;
        pending_.push_back({slot, retireTimeline});
        return true;
    }

    [[nodiscard]] bool release(SlotHandle handle, std::uint64_t retireTimeline)
    {
        return handle.valid() && handle.index < states_.size() &&
               generations_[handle.index] == handle.generation &&
               release(handle.index, retireTimeline);
    }

    [[nodiscard]] std::size_t collect(std::uint64_t completedTimeline)
    {
        completedTimeline_ = std::max(completedTimeline_, completedTimeline);
        std::size_t reclaimed = 0;
        std::size_t write = 0;
        for (const Pending pending : pending_)
        {
            if (pending.timeline <= completedTimeline_)
            {
                states_[pending.slot] = State::Free;
                generations_[pending.slot] = generations_[pending.slot] ==
                            std::numeric_limits<std::uint32_t>::max()
                        ? 1u
                        : generations_[pending.slot] + 1u;
                free_.push_back(pending.slot);
                ++reclaimed;
            }
            else
            {
                pending_[write++] = pending;
            }
        }
        pending_.resize(write);
        return reclaimed;
    }

    [[nodiscard]] bool contains(std::uint32_t slot) const noexcept
    {
        return slot < states_.size() && states_[slot] == State::Live;
    }
    [[nodiscard]] bool contains(SlotHandle handle) const noexcept
    {
        return handle.valid() && handle.index < states_.size() &&
               generations_[handle.index] == handle.generation && contains(handle.index);
    }
    [[nodiscard]] std::uint32_t capacity() const noexcept
    {
        return static_cast<std::uint32_t>(states_.size());
    }
    [[nodiscard]] std::uint32_t availableCount() const noexcept
    {
        return static_cast<std::uint32_t>(free_.size());
    }
    [[nodiscard]] std::uint64_t completedTimeline() const noexcept
    {
        return completedTimeline_;
    }

private:
    enum class State : std::uint8_t
    {
        Free,
        Live,
        Pending,
    };
    struct Pending
    {
        std::uint32_t slot;
        std::uint64_t timeline;
    };
    std::vector<std::uint32_t> generations_;
    std::vector<State> states_;
    std::vector<std::uint32_t> free_;
    std::vector<Pending> pending_;
    std::uint64_t completedTimeline_ = 0;
};

struct GpuSceneSoA
{
    std::vector<TransformRow> transforms;
    std::vector<BoundsRow> bounds;
    std::vector<MeshMaterialRow> meshMaterials;
    MaterialSoA materials;
};

// CPU mirror that applies RenderExtractor deltas while retaining stable slot
// indices. The Vulkan uploader can consume dirtyRanges() after each update.
class GpuSceneState final
{
public:
    explicit GpuSceneState(std::uint32_t capacity = 1024)
        : allocator_(capacity), soa_{std::vector<TransformRow>(capacity),
                  std::vector<BoundsRow>(capacity), std::vector<MeshMaterialRow>(capacity), {}}
    {
    }

    void reset(std::uint32_t capacity = 1024)
    {
        allocator_ = GpuSceneSlotAllocator(capacity);
        soa_.transforms.assign(capacity, {});
        soa_.bounds.assign(capacity, {});
        soa_.meshMaterials.assign(capacity, {});
        soa_.materials.materials.clear();
        entityToSlot_.clear();
        dirty_.clear();
    }

    [[nodiscard]] bool applyCreated(Ecs::Entity entity, const InstanceData& instance,
        BoundsRow bounds = {})
    {
        if (entityToSlot_.contains(entity)) return false;
        std::uint32_t slot = allocator_.allocate();
        if (slot == GpuSceneSlotAllocator::invalidSlot)
        {
            const std::uint32_t oldCapacity = allocator_.capacity();
            if (oldCapacity == std::numeric_limits<std::uint32_t>::max()) return false;
            const std::uint32_t nextCapacity = oldCapacity >
                    std::numeric_limits<std::uint32_t>::max() / 2u
                ? std::numeric_limits<std::uint32_t>::max()
                : std::max(1u, oldCapacity * 2u);
            allocator_.ensureCapacity(nextCapacity);
            soa_.transforms.resize(nextCapacity);
            soa_.bounds.resize(nextCapacity);
            soa_.meshMaterials.resize(nextCapacity);
            slot = allocator_.allocate();
        }
        if (slot == GpuSceneSlotAllocator::invalidSlot) return false;
        entityToSlot_[entity] = slot;
        soa_.transforms[slot].model = instance.transform;
        soa_.bounds[slot] = bounds;
        soa_.meshMaterials[slot] = {instance.meshId, instance.materialId, instance.flags, 0};
        markDirty(slot);
        return true;
    }

    [[nodiscard]] bool applyUpdated(Ecs::Entity entity, const InstanceData& instance,
        BoundsRow bounds = {})
    {
        const auto found = entityToSlot_.find(entity);
        if (found == entityToSlot_.end()) return false;
        const std::uint32_t slot = found->second;
        soa_.transforms[slot].model = instance.transform;
        soa_.bounds[slot] = bounds;
        soa_.meshMaterials[slot] = {instance.meshId, instance.materialId, instance.flags,
            soa_.meshMaterials[slot].lodState};
        markDirty(slot);
        return true;
    }

    [[nodiscard]] bool applyDestroyed(Ecs::Entity entity, std::uint64_t retireTimeline)
    {
        const auto found = entityToSlot_.find(entity);
        if (found == entityToSlot_.end()) return false;
        const bool released = allocator_.release(found->second, retireTimeline);
        if (released)
        {
            const auto slot = found->second;
            soa_.bounds[slot].sphereCenterRadius[3] = -1.0f;
            soa_.meshMaterials[slot].meshIndex = std::numeric_limits<std::uint32_t>::max();
            markDirty(slot);
            entityToSlot_.erase(found);
        }
        return released;
    }

    [[nodiscard]] std::size_t collect(std::uint64_t completedTimeline)
    {
        return allocator_.collect(completedTimeline);
    }
    [[nodiscard]] const GpuSceneSoA& soa() const noexcept { return soa_; }
    [[nodiscard]] const std::vector<GpuSceneDirtyRange>& dirtyRanges() const noexcept { return dirty_; }
    void clearDirtyRanges() noexcept { dirty_.clear(); }
    [[nodiscard]] std::uint32_t slot(Ecs::Entity entity) const noexcept
    {
        const auto found = entityToSlot_.find(entity);
        return found == entityToSlot_.end() ? GpuSceneSlotAllocator::invalidSlot : found->second;
    }

private:
    void markDirty(std::uint32_t slot)
    {
        if (!dirty_.empty() && dirty_.back().first + dirty_.back().count == slot)
            ++dirty_.back().count;
        else
            dirty_.push_back({slot, 1});
    }

    GpuSceneSlotAllocator allocator_;
    GpuSceneSoA soa_;
    std::unordered_map<Ecs::Entity, std::uint32_t, Ecs::Entity::Hasher> entityToSlot_;
    std::vector<GpuSceneDirtyRange> dirty_;
};

[[nodiscard]] inline BoundsRow computeWorldBounds(
    const glm::vec3& localMin, const glm::vec3& localMax, const glm::mat4& model) noexcept
{
    glm::vec3 worldMin(std::numeric_limits<float>::max());
    glm::vec3 worldMax(std::numeric_limits<float>::lowest());
    for (unsigned mask = 0; mask < 8; ++mask)
    {
        const glm::vec3 corner{(mask & 1u) ? localMax.x : localMin.x,
            (mask & 2u) ? localMax.y : localMin.y,
            (mask & 4u) ? localMax.z : localMin.z};
        const glm::vec3 transformed = glm::vec3(model * glm::vec4(corner, 1.0f));
        worldMin = glm::min(worldMin, transformed);
        worldMax = glm::max(worldMax, transformed);
    }
    const glm::vec3 center = (worldMin + worldMax) * 0.5f;
    const float radius = glm::length(worldMax - center);
    return BoundsRow{{center.x, center.y, center.z, radius},
        {worldMin.x, worldMin.y, worldMin.z, 0.0f},
        {worldMax.x, worldMax.y, worldMax.z, 0.0f}};
}

// CPU reference for the Hi-Z shader. Input and output are row-major depth
// images; edge texels are clamped so odd dimensions remain conservative.
[[nodiscard]] inline std::vector<float> reduceHiZ2x2Min(
    const std::vector<float>& source, std::uint32_t width, std::uint32_t height,
    std::uint32_t& outputWidth, std::uint32_t& outputHeight)
{
    outputWidth = std::max(1u, (width + 1u) / 2u);
    outputHeight = std::max(1u, (height + 1u) / 2u);
    std::vector<float> output(static_cast<std::size_t>(outputWidth) * outputHeight,
        std::numeric_limits<float>::infinity());
    if (source.empty() || width == 0 || height == 0)
        return output;
    for (std::uint32_t y = 0; y < outputHeight; ++y)
        for (std::uint32_t x = 0; x < outputWidth; ++x)
        {
            float minimum = std::numeric_limits<float>::infinity();
            for (std::uint32_t oy = 0; oy < 2; ++oy)
                for (std::uint32_t ox = 0; ox < 2; ++ox)
                {
                    const std::uint32_t sx = std::min(width - 1u, x * 2u + ox);
                    const std::uint32_t sy = std::min(height - 1u, y * 2u + oy);
                    const std::size_t index = static_cast<std::size_t>(sy) * width + sx;
                    if (index < source.size()) minimum = std::min(minimum, source[index]);
                }
            output[static_cast<std::size_t>(y) * outputWidth + x] = minimum;
        }
    return output;
}

} // namespace Halcyon::Renderer::Scene
