#pragma once
#include <any>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Halcyon::Renderer::Graph
{
class Blackboard final
{
public:
    template <typename T>
    void put(std::string_view key, T value)
    {
        values_[std::string(key)] = std::move(value);
    }
    template <typename T>
    T* get(std::string_view key) noexcept
    {
        auto it = values_.find(std::string(key));
        return it == values_.end() ? nullptr : std::any_cast<T>(&it->second);
    }
    template <typename T>
    const T* get(std::string_view key) const noexcept
    {
        auto it = values_.find(std::string(key));
        return it == values_.end() ? nullptr : std::any_cast<T>(&it->second);
    }
    bool contains(std::string_view key) const noexcept;
    template <typename T, typename... Args>
    T& getOrEmplace(std::string_view key, Args&&... args)
    {
        auto it = values_.find(std::string(key));
        if (it == values_.end())
        {
            it = values_.emplace(std::string(key), std::make_any<T>(std::forward<Args>(args)...))
                     .first;
        }
        return *std::any_cast<T>(&it->second);
    }
    bool remove(std::string_view key) noexcept;
    void clear() noexcept;

private:
    std::unordered_map<std::string, std::any> values_;
};
} // namespace Halcyon::Renderer::Graph
