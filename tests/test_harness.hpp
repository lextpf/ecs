// ============================================================================
// test_harness.hpp — the shared spine of quiver's test suite. No framework:
// a CHECK macro, inline counters every test TU shares, a violation capturer,
// and the fixture types reused across suites. main() lives in tests.cpp and
// remains the single registration point for every TU's test functions.
// ============================================================================
#pragma once

#include <quiver.hpp>

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

namespace ecs = quiver;

// ------------------------------------------------------------------- harness

// inline (not static): one counter shared by every test TU, so main()'s
// totals cover the whole suite.
inline int checks_run = 0;
inline int checks_failed = 0;

#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        ++checks_run;                                                   \
        if (!(cond))                                                    \
        {                                                               \
            ++checks_failed;                                            \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (false)

#define CHECK_VALID(world) CHECK((world).validate().has_value())

inline void section(const char* name)
{
    std::printf("--- %s\n", name);
}

// Captures violations instead of aborting, for tests that provoke them.
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

// ---------------------------------------------------- shared test components

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
    static constexpr auto quiver_storage = ecs::storage::stable;
};

// Counts constructions/destructions to prove destructor balance everywhere.
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

// Stable-storage variant of Counted (local classes cannot host the static
// policy member).
struct StableCounted : Counted
{
    using Counted::Counted;
    static constexpr auto quiver_storage = ecs::storage::stable;
};

// Over-aligned payload; the move constructor verifies the holder honoured it.
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

// An injectable sort algorithm: same contract as the default (permute the
// range exactly as a comparison sort would), different tie behavior.
struct stable_algo
{
    template <class It, class Cmp>
    void operator()(It first, It last, Cmp cmp) const
    {
        std::stable_sort(first, last, cmp);
    }
};

// A minimal in-memory archive: raw bytes, trivially-copyable values only
// (quiver's writer/reader concepts put the encoding on the archive's side).
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

// ----------------------------------------------- cross-TU test declarations
// Each test TU defines its functions with external linkage; main() in
// tests.cpp calls them through these declarations.

// tests_world.cpp (M1: membership, mutation, ordering parity)
void test_has_all_any();
void test_amend();
void test_driven_by();
void test_sort_algorithm();
void test_custom_pool_from_scratch();

// tests_bonds.cpp (M2/M3: N-ary bonds and bond views)
void test_bond_n_ary();
void test_bond_n_ary_paths();
void test_bond_n_ary_violations();  // defined (and called) under QUIVER_CHECKS only
void test_bond_n_ary_unbond();
void test_bond_observed_views();
void test_bond_partition_sort();
void test_bond_view_count();

// tests_queries.cpp (M4: any_of combinators)
void test_any_of_basics();
void test_any_of_driving();
void test_any_of_composition();
void test_any_of_violations();  // defined (and called) under QUIVER_CHECKS only

// tests_reactive.cpp (M5: watcher condition monitors)
void test_watcher_entered();
void test_watcher_changed();
void test_watcher_lifetime();
void test_watcher_violations();  // defined (and called) under QUIVER_CHECKS only

// tests_meta.cpp (M6/M8: any, reflection, meta×ECS bridge)
void test_any_basics();
void test_reflection_registry();
void test_reflection_fields();
void test_reflection_methods();
void test_reflection_construct();
void test_reflection_ecs_fields();
void test_reflection_ecs_verbs();

// tests_traits.cpp (M7: entity-traits templating)
void test_traits_entity_layouts();
void test_traits_compact_world();
void test_traits_relink();
