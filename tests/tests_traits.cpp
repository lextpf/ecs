// ============================================================================
// tests_traits.cpp — entity-traits templating (M7). Layer 1: the traits
// vocabulary, basic_entity<Traits> at custom widths, and the alias-identity
// pins proving the default traits reproduce the classic handle bit for bit.
// ============================================================================

#include "test_harness.hpp"

#include <unordered_set>

// A compact handle: 15-bit slots + 16-bit generations in 4 bytes (bit 15 of
// the index stays reserved for the provisional flag).
struct compact_traits
{
    using index_type = std::uint16_t;
    using generation_type = std::uint16_t;
    static constexpr std::uint32_t index_bits = 15;
};

// A wide handle: 48-bit slots + 16-bit generations (48 + 16 = 64 packs).
struct wide_traits
{
    using index_type = std::uint64_t;
    using generation_type = std::uint16_t;
    static constexpr std::uint32_t index_bits = 48;
};

using compact_entity = ecs::basic_entity<compact_traits>;
using wide_entity = ecs::basic_entity<wide_traits>;

// The alias identity, pinned at compile time.
static_assert(std::same_as<ecs::entity, ecs::basic_entity<ecs::default_entity_traits>>);
static_assert(std::same_as<ecs::entity_hash, ecs::basic_entity_hash<ecs::default_entity_traits>>);
static_assert(sizeof(ecs::entity) == 8);
static_assert(sizeof(compact_entity) == 4);
static_assert(compact_entity::index_bits == 15 && compact_entity::generation_bits == 16);
static_assert(wide_entity::index_bits == 48 && wide_entity::generation_bits == 16);

// Compile-time walls, for the record (each fails the entity_traits concept):
//   index_bits == digits(index_type)  // no spare bit left for provisionals
//   index_bits + generation digits > 64  // bits() could not pack
// e.g. struct bad { using index_type = std::uint16_t; using generation_type =
// std::uint16_t; static constexpr std::uint32_t index_bits = 16; };
//   ecs::basic_entity<bad> rejected.

void test_traits_entity_layouts()
{
    section("entity traits: custom widths, same contract");

    // Default-constructed handles are null at every width.
    CHECK(!compact_entity{});
    CHECK(!wide_entity{});
    CHECK(!ecs::entity{});
    CHECK(ecs::entity{} == ecs::no_entity);

    // Index/generation round-trip with the layout's own types.
    const compact_entity c{std::uint16_t{12}, std::uint16_t{3}};
    CHECK(c.index() == 12);
    CHECK(c.generation() == 3);
    CHECK(static_cast<bool>(c));

    const wide_entity w{std::uint64_t{1} << 40, std::uint16_t{9}};
    CHECK(w.index() == (std::uint64_t{1} << 40));
    CHECK(w.generation() == 9);

    // bits() packs index above the generation lane at every width.
    CHECK(c.bits() == ((std::uint64_t{12} << 16) | 3));
    CHECK(w.bits() == (((std::uint64_t{1} << 40) << 16) | 9));
    CHECK((ecs::entity{5, 7}.bits() == ((std::uint64_t{5} << 32) | 7)));

    // Comparisons order by (index, generation) like the classic handle.
    CHECK(compact_entity{} != c);
    CHECK((compact_entity{std::uint16_t{12}, std::uint16_t{4}} > c));
    CHECK((compact_entity{std::uint16_t{12}, std::uint16_t{3}} == c));

    // Hashing works through std::hash and the named hasher at every width.
    std::unordered_set<compact_entity, ecs::basic_entity_hash<compact_traits>> seen;
    seen.insert(c);
    seen.insert(compact_entity{});
    CHECK(seen.size() == 2);
    CHECK(seen.contains(c));
    CHECK(std::hash<wide_entity>{}(w) == std::hash<std::uint64_t>{}(w.bits()));
}
