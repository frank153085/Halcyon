#include "ShaderReflection.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <unordered_map>

namespace Halcyon::Renderer::Shaders
{
namespace
{

constexpr std::uint32_t kSpirvMagic = 0x07230203u;
constexpr std::uint16_t kOpTypeInt = 21;
constexpr std::uint16_t kOpTypeFloat = 22;
constexpr std::uint16_t kOpTypeVector = 23;
constexpr std::uint16_t kOpTypeMatrix = 24;
constexpr std::uint16_t kOpTypeImage = 25;
constexpr std::uint16_t kOpTypeSampler = 26;
constexpr std::uint16_t kOpTypeSampledImage = 27;
constexpr std::uint16_t kOpTypeArray = 28;
constexpr std::uint16_t kOpTypeRuntimeArray = 29;
constexpr std::uint16_t kOpTypeStruct = 30;
constexpr std::uint16_t kOpTypePointer = 32;
constexpr std::uint16_t kOpConstant = 43;
constexpr std::uint16_t kOpVariable = 59;
constexpr std::uint16_t kOpDecorate = 71;
constexpr std::uint16_t kOpMemberDecorate = 72;

constexpr std::uint32_t kDecorationBinding = 33;
constexpr std::uint32_t kDecorationDescriptorSet = 34;
constexpr std::uint32_t kDecorationOffset = 35;
constexpr std::uint32_t kDecorationLocation = 30;

constexpr std::uint32_t kStorageUniformConstant = 0;
constexpr std::uint32_t kStorageUniform = 2;
constexpr std::uint32_t kStorageOutput = 3;
constexpr std::uint32_t kStoragePushConstant = 9;
constexpr std::uint32_t kStorageStorageBuffer = 12;

struct TypeInfo
{
    std::uint16_t opcode = 0;
    std::uint32_t elementType = 0;
    std::uint32_t lengthId = 0;
    std::uint32_t componentType = 0;
    std::uint32_t componentCount = 0;
    std::uint32_t width = 0;
    std::uint32_t storageClass = 0;
    std::uint32_t imageSampled = 0;
    std::vector<std::uint32_t> members;
};

struct VariableInfo
{
    std::uint32_t typeId = 0;
    std::uint32_t storageClass = 0;
};

struct DecorationInfo
{
    std::uint32_t set = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t binding = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t location = std::numeric_limits<std::uint32_t>::max();
};

[[nodiscard]] std::uint32_t scalarSize(const TypeInfo& type) noexcept
{
    return type.width == 0 ? 0u : type.width / 8u;
}

[[nodiscard]] std::uint32_t typeSize(const std::unordered_map<std::uint32_t, TypeInfo>& types,
    const std::unordered_map<std::uint32_t, std::uint32_t>& constants,
    const std::unordered_map<std::uint64_t, std::uint32_t>& memberOffsets,
    std::uint32_t typeId)
{
    const auto found = types.find(typeId);
    if (found == types.end())
    {
        return 0;
    }
    const TypeInfo& type = found->second;
    switch (type.opcode)
    {
        case kOpTypeInt:
        case kOpTypeFloat:
            return scalarSize(type);
        case kOpTypeVector:
        case kOpTypeMatrix:
        {
            const auto elementSize = typeSize(types, constants, memberOffsets, type.componentType);
            return elementSize == 0 || type.componentCount == 0 ? 0
                                                                : elementSize * type.componentCount;
        }
        case kOpTypeArray:
        {
            const auto elementSize = typeSize(types, constants, memberOffsets, type.elementType);
            const auto length = constants.find(type.lengthId);
            return elementSize == 0 || length == constants.end() ? 0 : elementSize * length->second;
        }
        case kOpTypeRuntimeArray:
            return 0;
        case kOpTypeStruct:
        {
            std::uint32_t size = 0;
            for (std::uint32_t member = 0; member < type.members.size(); ++member)
            {
                const auto offset =
                    memberOffsets.find((static_cast<std::uint64_t>(typeId) << 32u) | member);
                const auto memberSize =
                    typeSize(types, constants, memberOffsets, type.members[member]);
                if (offset == memberOffsets.end() || memberSize == 0)
                {
                    return 0;
                }
                size = std::max(size, offset->second + memberSize);
            }
            return size;
        }
        default:
            return 0;
    }
}

[[nodiscard]] ResourceType resourceType(const TypeInfo& type) noexcept
{
    if (type.storageClass == kStorageUniform)
    {
        return ResourceType::UniformBuffer;
    }
    if (type.storageClass == kStorageStorageBuffer)
    {
        return ResourceType::StorageBuffer;
    }
    if (type.storageClass != kStorageUniformConstant)
    {
        return ResourceType::Unknown;
    }
    switch (type.opcode)
    {
        case kOpTypeSampler:
            return ResourceType::Sampler;
        case kOpTypeImage:
            return type.imageSampled == 2u ? ResourceType::StorageImage
                                           : ResourceType::SampledImage;
        case kOpTypeSampledImage:
            return ResourceType::SampledImage;
        default:
            return ResourceType::Unknown;
    }
}

} // namespace

Halcyon::Result<ShaderReflection> reflectSpirv(std::span<const std::uint32_t> words)
{
    if (words.size() < 5u || words.front() != kSpirvMagic)
    {
        return Halcyon::Result<ShaderReflection>::failure(
            {Halcyon::ErrorCode::InvalidArgument, "invalid SPIR-V header"});
    }

    std::unordered_map<std::uint32_t, TypeInfo> types;
    std::unordered_map<std::uint32_t, VariableInfo> variables;
    std::unordered_map<std::uint32_t, DecorationInfo> decorations;
    std::unordered_map<std::uint32_t, std::uint32_t> constants;
    std::unordered_map<std::uint64_t, std::uint32_t> memberOffsets;

    for (std::size_t offset = 5; offset < words.size();)
    {
        const std::uint32_t instruction = words[offset];
        const auto wordCount = static_cast<std::uint16_t>(instruction >> 16u);
        const auto opcode = static_cast<std::uint16_t>(instruction & 0xffffu);
        if (wordCount == 0 || offset + wordCount > words.size())
        {
            return Halcyon::Result<ShaderReflection>::failure(
                {Halcyon::ErrorCode::InvalidArgument, "truncated SPIR-V instruction"});
        }
        const auto operand = [&](std::uint16_t index) -> std::uint32_t
        {
            return index < wordCount ? words[offset + index] : 0u;
        };
        switch (opcode)
        {
            case kOpTypeInt:
            case kOpTypeFloat:
            {
                TypeInfo type;
                type.opcode = opcode;
                type.width = operand(2);
                types[operand(1)] = std::move(type);
                break;
            }
            case kOpTypeVector:
            case kOpTypeMatrix:
            {
                TypeInfo type;
                type.opcode = opcode;
                type.componentType = operand(2);
                type.componentCount = operand(3);
                types[operand(1)] = std::move(type);
                break;
            }
            case kOpTypeImage:
            {
                TypeInfo type;
                type.opcode = opcode;
                type.elementType = operand(2);
                type.imageSampled = operand(7);
                types[operand(1)] = std::move(type);
                break;
            }
            case kOpTypeSampler:
            case kOpTypeSampledImage:
            {
                TypeInfo type;
                type.opcode = opcode;
                type.elementType = operand(2);
                types[operand(1)] = std::move(type);
                break;
            }
            case kOpTypeArray:
            case kOpTypeRuntimeArray:
            {
                TypeInfo type;
                type.opcode = opcode;
                type.elementType = operand(2);
                type.lengthId = operand(3);
                types[operand(1)] = std::move(type);
                break;
            }
            case kOpTypeStruct:
            {
                TypeInfo type;
                type.opcode = opcode;
                for (std::uint16_t index = 2; index < wordCount; ++index)
                {
                    type.members.push_back(operand(index));
                }
                types[operand(1)] = std::move(type);
                break;
            }
            case kOpTypePointer:
            {
                TypeInfo type;
                type.opcode = opcode;
                type.storageClass = operand(2);
                type.elementType = operand(3);
                types[operand(1)] = std::move(type);
                break;
            }
            case kOpConstant:
                constants[operand(2)] = operand(3);
                break;
            case kOpVariable:
                variables[operand(2)] = VariableInfo{operand(1), operand(3)};
                break;
            case kOpDecorate:
            {
                auto& decoration = decorations[operand(1)];
                if (operand(2) == kDecorationBinding)
                {
                    decoration.binding = operand(3);
                }
                else if (operand(2) == kDecorationDescriptorSet)
                {
                    decoration.set = operand(3);
                }
                else if (operand(2) == kDecorationLocation)
                {
                    decoration.location = operand(3);
                }
                break;
            }
            case kOpMemberDecorate:
                if (operand(3) == kDecorationOffset)
                {
                    memberOffsets[(static_cast<std::uint64_t>(operand(1)) << 32u) | operand(2)] =
                        operand(4);
                }
                break;
            default:
                break;
        }
        offset += wordCount;
    }

    ShaderReflection reflection;
    for (const auto& [variableId, variable] : variables)
    {
        const auto decoration = decorations.find(variableId);
        const auto pointer = types.find(variable.typeId);
        if (decoration == decorations.end() || pointer == types.end() ||
            decoration->second.set == std::numeric_limits<std::uint32_t>::max() ||
            decoration->second.binding == std::numeric_limits<std::uint32_t>::max() ||
            pointer->second.opcode != kOpTypePointer)
        {
            continue;
        }

        const TypeInfo* descriptor = &pointer->second;
        std::uint32_t arraySize = 1;
        const auto pointee = types.find(descriptor->elementType);
        if (pointee == types.end())
        {
            continue;
        }
        descriptor = &pointee->second;
        if (descriptor->opcode == kOpTypeArray || descriptor->opcode == kOpTypeRuntimeArray)
        {
            if (descriptor->opcode == kOpTypeArray)
            {
                const auto length = constants.find(descriptor->lengthId);
                arraySize = length == constants.end() ? 0u : length->second;
            }
            else
            {
                arraySize = 0;
            }
            const auto arrayElement = types.find(descriptor->elementType);
            if (arrayElement == types.end())
            {
                continue;
            }
            descriptor = &arrayElement->second;
        }

        const auto type = resourceType(TypeInfo{descriptor->opcode,
            descriptor->elementType,
            descriptor->lengthId,
            descriptor->componentType,
            descriptor->componentCount,
            descriptor->width,
            variable.storageClass,
            descriptor->imageSampled,
            descriptor->members});
        if (type != ResourceType::Unknown)
        {
            reflection.resources.push_back(ResourceBinding{
                decoration->second.set, decoration->second.binding, arraySize, type, variableId});
        }
    }

    for (const auto& [variableId, variable] : variables)
    {
        if (variable.storageClass != kStorageOutput)
        {
            continue;
        }
        const auto decoration = decorations.find(variableId);
        if (decoration != decorations.end() &&
            decoration->second.location != std::numeric_limits<std::uint32_t>::max())
        {
            reflection.outputLocations.push_back(decoration->second.location);
        }
    }

    for (const auto& [variableId, variable] : variables)
    {
        if (variable.storageClass != kStoragePushConstant)
        {
            continue;
        }
        const auto pointer = types.find(variable.typeId);
        if (pointer == types.end() || pointer->second.opcode != kOpTypePointer)
        {
            continue;
        }
        reflection.pushConstants.push_back(PushConstantRange{
            variableId, typeSize(types, constants, memberOffsets, pointer->second.elementType)});
    }

    std::sort(reflection.resources.begin(),
        reflection.resources.end(),
        [](const ResourceBinding& lhs, const ResourceBinding& rhs)
        {
            return lhs.set == rhs.set ? lhs.binding < rhs.binding : lhs.set < rhs.set;
        });
    std::sort(reflection.outputLocations.begin(), reflection.outputLocations.end());
    reflection.outputLocations.erase(
        std::unique(reflection.outputLocations.begin(), reflection.outputLocations.end()),
        reflection.outputLocations.end());
    return Halcyon::Result<ShaderReflection>::success(std::move(reflection));
}

} // namespace Halcyon::Renderer::Shaders
