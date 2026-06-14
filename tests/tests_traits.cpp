#include "test_harness.hpp"

#include <unordered_set>

struct compact_traits
{
    using index_type = std::uint16_t;
    using generation_type = std::uint16_t;
    static constexpr std::uint32_t index_bits = 15;
};

struct wide_traits
{
    using index_type = std::uint64_t;
    using generation_type = std::uint16_t;
    static constexpr std::uint32_t index_bits = 48;
};

using compact_entity = ecs::basic_entity<compact_traits>;
using wide_entity = ecs::basic_entity<wide_traits>;

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

static_assert(std::same_as<ecs::entity, ecs::basic_entity<ecs::default_entity_traits>>);
static_assert(std::same_as<ecs::entity_hash, ecs::basic_entity_hash<ecs::default_entity_traits>>);
static_assert(sizeof(ecs::entity) == 8);
static_assert(sizeof(compact_entity) == 4);
static_assert(compact_entity::index_bits == 15 && compact_entity::generation_bits == 16);
static_assert(wide_entity::index_bits == 48 && wide_entity::generation_bits == 16);

TEST(Traits, TraitsEntityLayouts)
{
    EXPECT_FALSE(compact_entity{});
    EXPECT_FALSE(wide_entity{});
    EXPECT_FALSE(ecs::entity{});
    EXPECT_EQ(ecs::entity{}, ecs::no_entity);

    const compact_entity c{std::uint16_t{12}, std::uint16_t{3}};
    EXPECT_EQ(c.index(), 12);
    EXPECT_EQ(c.generation(), 3);
    EXPECT_TRUE(static_cast<bool>(c));

    const wide_entity w{std::uint64_t{1} << 40, std::uint16_t{9}};
    EXPECT_EQ(w.index(), (std::uint64_t{1} << 40));
    EXPECT_EQ(w.generation(), 9);

    EXPECT_EQ(c.bits(), ((std::uint64_t{12} << 16) | 3));
    EXPECT_EQ(w.bits(), (((std::uint64_t{1} << 40) << 16) | 9));
    EXPECT_EQ((ecs::entity{5, 7}.bits()), ((std::uint64_t{5} << 32) | 7));

    EXPECT_NE(compact_entity{}, c);
    EXPECT_GT((compact_entity{std::uint16_t{12}, std::uint16_t{4}}), c);
    EXPECT_EQ((compact_entity{std::uint16_t{12}, std::uint16_t{3}}), c);

    std::unordered_set<compact_entity, ecs::basic_entity_hash<compact_traits>> seen;
    seen.insert(c);
    seen.insert(compact_entity{});
    EXPECT_EQ(seen.size(), 2);
    EXPECT_TRUE(seen.contains(c));
    EXPECT_EQ(std::hash<wide_entity>{}(w), std::hash<std::uint64_t>{}(w.bits()));
}

TEST(Traits, TraitsCompactWorld)
{
    using compact_registry = ecs::basic_registry<compact_traits>;
    compact_registry w;

    const compact_entity e = w.create();
    EXPECT_TRUE(w.alive(e));
    w.add<Pos>(e, Pos{1});
    EXPECT_EQ(w.get<Pos>(e).x, 1);
    w.amend<Pos>(e, [](Pos& p) { p.x = 11; });

    const compact_entity f = w.create();
    w.add<Pos>(f, Pos{2});
    w.add<Vel>(f, Vel{20});

    std::size_t rows = 0;
    w.view<Pos>().each([&](compact_entity ent, Pos&) { rows += w.alive(ent) ? 1 : 0; });
    EXPECT_EQ(rows, 2);
    EXPECT_TRUE((w.has_all<Pos, Vel>(f)));

    ecs::basic_command_buffer<compact_traits> cmd;
    const compact_entity prov = cmd.create();
    EXPECT_NE((prov.index() & (std::uint16_t{1} << 15)), 0);
    cmd.add<Pos>(prov, Pos{3});
    cmd.add<Hp>(e, Hp{5});
    const auto applied = w.apply(cmd);
    EXPECT_EQ(applied.applied, 3);
    EXPECT_EQ(w.view<Pos>().count(), 3);

    ecs::basic_command_buffer<compact_traits> cmd2;
    const compact_entity packed = cmd2.create(Pos{4}, Hp{1});
    EXPECT_NE((packed.index() & (std::uint16_t{1} << 15)), 0);
    const auto applied2 = w.apply(cmd2);
    EXPECT_EQ(applied2.applied, 3);
    EXPECT_EQ(w.view<Pos>().count(), 4);

    const compact_entity g = w.create();
    const auto slot = g.index();
    w.destroy(g);
    const compact_entity g2 = w.create();
    EXPECT_EQ(g2.index(), slot);
    EXPECT_EQ(g2.generation(), static_cast<std::uint16_t>(g.generation() + 1));
    EXPECT_FALSE(w.alive(g));
    EXPECT_TRUE(w.alive(g2));

    byte_writer out;
    ecs::pack<Pos, Vel, Hp>(w, out);
    ecs::basic_registry<compact_traits> fresh;
    byte_reader in{&out.data};
    EXPECT_TRUE((ecs::unpack<Pos, Vel, Hp>(fresh, in).has_value()));
    EXPECT_EQ(fresh.live_count(), w.live_count());

    ecs::registry classic;
    classic.create(Pos{9});
    byte_writer classic_out;
    ecs::pack<Pos>(classic, classic_out);
    ecs::basic_registry<compact_traits> wrong;
    byte_reader classic_in{&classic_out.data};
    const auto refused = ecs::unpack<Pos>(wrong, classic_in);
    EXPECT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ecs::fault_code::archive_mismatch);

    EXPECT_TRUE(RegistryValid(w));
    EXPECT_TRUE(RegistryValid(fresh));
}

TEST(Traits, TraitsWideWorld)
{
    using wide_registry = ecs::basic_registry<wide_traits>;
    wide_registry w;

    const wide_entity e = w.create(Pos{1}, Vel{10});
    const wide_entity f = w.create(Pos{2});
    EXPECT_TRUE(w.alive(e) && w.alive(f));
    EXPECT_EQ(w.get<Pos>(e).x, 1);

    std::size_t rows = 0;
    w.view<Pos>().each([&](wide_entity ent, Pos&) { rows += w.alive(ent) ? 1 : 0; });
    EXPECT_EQ(rows, 2);

    ecs::basic_command_buffer<wide_traits> cmd;
    const wide_entity prov = cmd.create(Pos{3}, Hp{7});
    EXPECT_NE((prov.index() & (std::uint64_t{1} << 48)), 0);
    const auto applied = w.apply(cmd);
    EXPECT_EQ(applied.applied, 3);
    EXPECT_EQ(w.view<Pos>().count(), 3);

    const wide_entity far{std::uint64_t{1} << 20, std::uint16_t{5}};
    EXPECT_TRUE(w.restore_entity(far).has_value());
    EXPECT_TRUE(w.alive(far));
    EXPECT_EQ(w.current_handle(far.index()), far);
    w.add<Pos>(far, Pos{9});
    EXPECT_EQ(w.get<Pos>(far).x, 9);
    w.destroy(far);
    EXPECT_FALSE(w.alive(far));

    byte_writer out;
    ecs::pack<Pos, Vel, Hp>(w, out);
    wide_registry fresh;
    byte_reader in{&out.data};
    EXPECT_TRUE((ecs::unpack<Pos, Vel, Hp>(fresh, in).has_value()));
    EXPECT_EQ(fresh.live_count(), w.live_count());

    ecs::basic_registry<compact_traits> compact;
    compact.create(Pos{4});
    byte_writer compact_out;
    ecs::pack<Pos>(compact, compact_out);
    wide_registry wrong;
    byte_reader compact_in{&compact_out.data};
    const auto refused = ecs::unpack<Pos>(wrong, compact_in);
    EXPECT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ecs::fault_code::archive_mismatch);

    EXPECT_TRUE(RegistryValid(w));
    EXPECT_TRUE(RegistryValid(fresh));
}

TEST(Traits, TraitsRelink)
{
    static_assert(ecs::relink_traits<CompactSquad, compact_traits>::links);
    static_assert(!ecs::relink_traits<CompactSquad>::links);
    static_assert(ecs::relink_traits<CompactLink, compact_traits>::links);
    static_assert(!ecs::relink_traits<Pos, compact_traits>::links);

    using compact_registry = ecs::basic_registry<compact_traits>;
    compact_registry source;
    const compact_entity captain = source.create();
    source.add<Pos>(captain, Pos{1});
    const compact_entity grunt = source.create();
    source.add<Pos>(grunt, Pos{2});
    source.add<CompactSquad>(grunt, CompactSquad{captain, 7});
    source.add<CompactLink>(grunt, CompactLink{captain});

    byte_writer out;
    ecs::pack<Pos, CompactSquad, CompactLink>(source, out);

    compact_registry host;
    host.create();
    byte_reader in{&out.data};
    const auto grafted = ecs::graft<Pos, CompactSquad, CompactLink>(host, in);
    EXPECT_TRUE(grafted.has_value());
    const compact_entity new_captain = grafted->resolve(captain);
    const compact_entity new_grunt = grafted->resolve(grunt);
    EXPECT_TRUE(host.alive(new_captain) && host.alive(new_grunt));
    EXPECT_TRUE(new_captain != captain || new_grunt != grunt);
    EXPECT_EQ(host.get<CompactSquad>(new_grunt).leader, new_captain);
    EXPECT_EQ(host.get<CompactSquad>(new_grunt).morale, 7);
    EXPECT_EQ(host.get<CompactLink>(new_grunt).target, new_captain);
    EXPECT_TRUE(RegistryValid(host));
}
