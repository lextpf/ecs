// ============================================================================
// tests_traits.cpp -- entity traits at custom widths: handle layouts, compact
// and wide worlds end-to-end, graft relinks, and the alias-identity pins.
// ============================================================================

#include "test_harness.hpp"

#include <unordered_set>

// A compact handle: 15:16 in 4 bytes; bit 15 stays reserved for provisionals.
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

// Relink subjects: one with a per-layout hook, one with a traits-generic template member.
struct CompactSquad
{
    compact_entity leader;
    int morale = 0;

    void ecs_relink(const ecs::basic_graft_map<compact_traits>& map)
    {
        leader = map.resolve(leader);
    }
};

struct CompactLink
{
    compact_entity target;

    template <class Traits>
    void ecs_relink(const ecs::basic_graft_map<Traits>& map)
    {
        target = map.resolve(target);
    }
};

// The alias identity, pinned at compile time.
static_assert(std::same_as<ecs::entity, ecs::basic_entity<ecs::default_entity_traits>>);
static_assert(std::same_as<ecs::entity_hash, ecs::basic_entity_hash<ecs::default_entity_traits>>);
static_assert(sizeof(ecs::entity) == 8);
static_assert(sizeof(compact_entity) == 4);
static_assert(compact_entity::index_bits == 15 && compact_entity::generation_bits == 16);
static_assert(wide_entity::index_bits == 48 && wide_entity::generation_bits == 16);

// Compile-time walls (each fails the entity_traits concept):
//   index_bits == digits(index_type)  // no spare bit for provisionals
//   index_bits + generation digits > 64  // bits() could not pack

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

void test_traits_compact_world()
{
    section("entity traits: a compact 15:16 world end-to-end");
    using compact_world = ecs::basic_world<compact_traits>;
    compact_world w;

    // The verbs run on 4-byte handles; component type ids are process-wide.
    const compact_entity e = w.spawn();
    CHECK(w.alive(e));
    w.add<Pos>(e, Pos{1});
    CHECK(w.get<Pos>(e).x == 1);
    w.amend<Pos>(e, [](Pos& p) { p.x = 11; });

    const compact_entity f = w.spawn();
    w.add<Pos>(f, Pos{2});
    w.add<Vel>(f, Vel{20});

    // Selections hand back the world's own entity type.
    std::size_t rows = 0;
    w.select<Pos>().each([&](compact_entity ent, Pos&) { rows += w.alive(ent) ? 1 : 0; });
    CHECK(rows == 2);
    CHECK((w.has_all<Pos, Vel>(f)));

    // Bonds mirror-partition exactly as in the classic world.
    w.bond<Pos, Vel>();
    CHECK((w.bonded<Pos, Vel>().count() == 1));

    // Command buffers mint provisionals at the traits' spare bit (bit 15).
    ecs::basic_command_buffer<compact_traits> cmd;
    const compact_entity prov = cmd.spawn();
    CHECK((prov.index() & (std::uint16_t{1} << 15)) != 0);
    cmd.add<Pos>(prov, Pos{3});
    cmd.add<Hp>(e, Hp{5});
    const auto applied = w.apply(cmd);
    CHECK(applied.applied == 3);
    CHECK(w.select<Pos>().count() == 3);

    // Value-pack spawns mint their provisional at the same spare bit.
    ecs::basic_command_buffer<compact_traits> cmd2;
    const compact_entity packed = cmd2.spawn(Pos{4}, Hp{1});
    CHECK((packed.index() & (std::uint16_t{1} << 15)) != 0);
    const auto applied2 = w.apply(cmd2);
    CHECK(applied2.applied == 3);  // one spawn, two adds
    CHECK(w.select<Pos>().count() == 4);

    // Kill recycles the slot and bumps the 16-bit generation.
    const compact_entity g = w.spawn();
    const auto slot = g.index();
    w.kill(g);
    const compact_entity g2 = w.spawn();
    CHECK(g2.index() == slot);
    CHECK(g2.generation() == static_cast<std::uint16_t>(g.generation() + 1));
    CHECK(!w.alive(g));
    CHECK(w.alive(g2));

    // Archives round-trip under the compact layout stamp.
    byte_writer out;
    ecs::pack<Pos, Vel, Hp>(w, out);
    ecs::basic_world<compact_traits> fresh;
    byte_reader in{&out.data};
    CHECK((ecs::unpack<Pos, Vel, Hp>(fresh, in).has_value()));
    CHECK(fresh.live_count() == w.live_count());
    fresh.bond<Pos, Vel>();  // bonds are per-world state: rebuild over loaded rows
    CHECK((fresh.bonded<Pos, Vel>().count() == 1));

    // A stream packed with DIFFERENT traits refuses with archive_mismatch.
    ecs::world classic;
    classic.spawn(Pos{9});
    byte_writer classic_out;
    ecs::pack<Pos>(classic, classic_out);
    ecs::basic_world<compact_traits> wrong;
    byte_reader classic_in{&classic_out.data};
    const auto refused = ecs::unpack<Pos>(wrong, classic_in);
    CHECK(!refused.has_value());
    CHECK(refused.error().code == ecs::fault_code::archive_mismatch);

    CHECK_VALID(w);
    CHECK_VALID(fresh);
}

void test_traits_wide_world()
{
    section("entity traits: a wide 48:16 world end-to-end");
    using wide_world = ecs::basic_world<wide_traits>;
    wide_world w;

    // The verbs run on 8-byte handles with 48-bit slots.
    const wide_entity e = w.spawn(Pos{1}, Vel{10});
    const wide_entity f = w.spawn(Pos{2});
    CHECK(w.alive(e) && w.alive(f));
    CHECK(w.get<Pos>(e).x == 1);

    std::size_t rows = 0;
    w.select<Pos>().each([&](wide_entity ent, Pos&) { rows += w.alive(ent) ? 1 : 0; });
    CHECK(rows == 2);

    // Bonds mirror-partition at the wide width.
    w.bond<Pos, Vel>();
    CHECK((w.bonded<Pos, Vel>().count() == 1));

    // Command buffers mint provisionals at the traits' spare bit (bit 48).
    ecs::basic_command_buffer<wide_traits> cmd;
    const wide_entity prov = cmd.spawn(Pos{3}, Hp{7});
    CHECK((prov.index() & (std::uint64_t{1} << 48)) != 0);
    const auto applied = w.apply(cmd);
    CHECK(applied.applied == 3);
    CHECK(w.select<Pos>().count() == 3);

    // restore_entity claims a slot far past 2^16; the paged sparse index pays
    // one page for it. Slots are dense per high-water mark -- keep it modest.
    const wide_entity far{std::uint64_t{1} << 20, std::uint16_t{5}};
    CHECK(w.restore_entity(far).has_value());
    CHECK(w.alive(far));
    CHECK(w.current_handle(far.index()) == far);
    w.add<Pos>(far, Pos{9});
    CHECK(w.get<Pos>(far).x == 9);
    w.kill(far);
    CHECK(!w.alive(far));

    // Archives round-trip under the wide layout stamp.
    byte_writer out;
    ecs::pack<Pos, Vel, Hp>(w, out);
    wide_world fresh;
    byte_reader in{&out.data};
    CHECK((ecs::unpack<Pos, Vel, Hp>(fresh, in).has_value()));
    CHECK(fresh.live_count() == w.live_count());

    // A compact stream refuses to load into a wide world.
    ecs::basic_world<compact_traits> compact;
    compact.spawn(Pos{4});
    byte_writer compact_out;
    ecs::pack<Pos>(compact, compact_out);
    wide_world wrong;
    byte_reader compact_in{&compact_out.data};
    const auto refused = ecs::unpack<Pos>(wrong, compact_in);
    CHECK(!refused.has_value());
    CHECK(refused.error().code == ecs::fault_code::archive_mismatch);

    CHECK_VALID(w);
    CHECK_VALID(fresh);
}

void test_traits_relink()
{
    section("entity traits: graft relinks under custom layouts");

    // The trait detects per-layout hooks at their own traits -- and only there.
    static_assert(ecs::relink_traits<CompactSquad, compact_traits>::links);
    static_assert(!ecs::relink_traits<CompactSquad>::links);
    static_assert(ecs::relink_traits<CompactLink, compact_traits>::links);
    static_assert(!ecs::relink_traits<Pos, compact_traits>::links);

    using compact_world = ecs::basic_world<compact_traits>;
    compact_world source;
    const compact_entity captain = source.spawn();
    source.add<Pos>(captain, Pos{1});
    const compact_entity grunt = source.spawn();
    source.add<Pos>(grunt, Pos{2});
    source.add<CompactSquad>(grunt, CompactSquad{captain, 7});
    source.add<CompactLink>(grunt, CompactLink{captain});

    byte_writer out;
    ecs::pack<Pos, CompactSquad, CompactLink>(source, out);

    compact_world host;
    host.spawn();  // pre-populated: grafted ids must be fresh
    byte_reader in{&out.data};
    const auto grafted = ecs::graft<Pos, CompactSquad, CompactLink>(host, in);
    CHECK(grafted.has_value());
    const compact_entity new_captain = grafted->resolve(captain);
    const compact_entity new_grunt = grafted->resolve(grunt);
    CHECK(host.alive(new_captain) && host.alive(new_grunt));
    CHECK(new_captain != captain || new_grunt != grunt);  // fresh spawns
    CHECK(host.get<CompactSquad>(new_grunt).leader == new_captain);
    CHECK(host.get<CompactSquad>(new_grunt).morale == 7);
    CHECK(host.get<CompactLink>(new_grunt).target == new_captain);
    CHECK_VALID(host);
}
