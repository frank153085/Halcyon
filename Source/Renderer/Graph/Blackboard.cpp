#include "Blackboard.h"

namespace Halcyon::Renderer::Graph
{

bool Blackboard::contains(std::string_view key) const noexcept
{
    return values_.find(std::string(key)) != values_.end();
}

bool Blackboard::remove(std::string_view key) noexcept
{
    return values_.erase(std::string(key)) != 0;
}

void Blackboard::clear() noexcept
{
    values_.clear();
}

} // namespace Halcyon::Renderer::Graph
