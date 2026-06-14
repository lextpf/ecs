#pragma once

#include <gtest/gtest.h>

#include <ecs.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

template <class Registry>
inline ::testing::AssertionResult RegistryValid(const Registry& w)
{
    const auto r = w.validate();
    if (r.has_value())
    {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "registry.validate() faulted: code=" << static_cast<int>(r.error().code);
}

inline int violations_seen = 0;
inline std::string last_violation;

inline void counting_handler(const char* message)
{
    ++violations_seen;
    last_violation = message;
}

struct violation_scope
{
    violation_scope()
        : previous(ecs::set_violation_handler(&counting_handler))
    {
        violations_seen = 0;
        last_violation.clear();
    }

    ~violation_scope() { ecs::set_violation_handler(previous); }

    ecs::violation_handler previous;
};

struct Pos
{
    int x = 0;
};

struct Vel
{
    int v = 0;
};

struct Hp
{
    int hp = 0;
};

struct TagA
{
};

struct TagB
{
};

struct Stable
{
    int value = 0;
    static constexpr auto ecs_storage = ecs::storage::stable;
};

struct Counted
{
    inline static int live = 0;
    inline static int total_ctors = 0;
    inline static int total_dtors = 0;

    int value;

    explicit Counted(int v = 0)
        : value(v)
    {
        ++live;
        ++total_ctors;
    }

    Counted(const Counted& other)
        : value(other.value)
    {
        ++live;
        ++total_ctors;
    }

    Counted(Counted&& other) noexcept
        : value(other.value)
    {
        ++live;
        ++total_ctors;
    }

    Counted& operator=(const Counted&) = default;
    Counted& operator=(Counted&&) = default;

    ~Counted()
    {
        --live;
        ++total_dtors;
    }
};

struct StableCounted : Counted
{
    using Counted::Counted;
    static constexpr auto ecs_storage = ecs::storage::stable;
};

inline int misaligned_payloads = 0;

struct alignas(64) Aligned
{
    Aligned() = default;

    Aligned(Aligned&& other) noexcept
    {
        if (reinterpret_cast<std::uintptr_t>(&other) % 64 != 0)
        {
            ++misaligned_payloads;
        }
        lanes[0] = other.lanes[0];
    }

    Aligned& operator=(Aligned&&) = default;

    float lanes[16] = {};
};

struct stable_algo
{
    template <class It, class Cmp>
    void operator()(It first, It last, Cmp cmp) const
    {
        std::stable_sort(first, last, cmp);
    }
};

struct byte_writer
{
    std::vector<std::byte> data;

    template <class T>
        requires std::is_trivially_copyable_v<T>
    void operator()(const T& value)
    {
        const auto* raw = reinterpret_cast<const std::byte*>(&value);
        data.insert(data.end(), raw, raw + sizeof(T));
    }
};

struct byte_reader
{
    const std::vector<std::byte>* data = nullptr;
    std::size_t pos = 0;

    template <class T>
        requires std::is_trivially_copyable_v<T>
    void operator()(T& value)
    {
        std::memcpy(&value, data->data() + pos, sizeof(T));
        pos += sizeof(T);
    }
};
