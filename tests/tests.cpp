#include "test_harness.hpp"

#include <thread>
#include <unordered_set>

struct EmptyPacked
{
    static constexpr auto ecs_storage = ecs::storage::packed;
};

struct MoveOnly
{
    explicit MoveOnly(int v)
        : box(std::make_unique<int>(v))
    {
    }

    std::unique_ptr<int> box;
};

static int anchored_betrayals = 0;

struct Anchored
{
    Anchored()
        : self(this)
    {
    }

    Anchored(Anchored&& other) noexcept
    {
        if (other.self != &other)
        {
            ++anchored_betrayals;
        }
        self = this;
    }

    Anchored& operator=(Anchored&& other) noexcept
    {
        if (other.self != &other)
        {
            ++anchored_betrayals;
        }
        return *this;
    }

    const Anchored* self;
};

struct Pinned
{
    std::atomic<int> charge{0};
    static constexpr auto ecs_storage = ecs::storage::stable;
};
static_assert(!std::is_move_constructible_v<Pinned>);

static int misaligned_big = 0;

struct alignas(128) Big128
{
    Big128() = default;

    explicit Big128(float v) { lanes[0] = v; }

    Big128(Big128&& other) noexcept
    {
        if (reinterpret_cast<std::uintptr_t>(&other) % 128 != 0)
        {
            ++misaligned_big;
        }
        lanes[0] = other.lanes[0];
    }

    Big128& operator=(Big128&&) = default;

    float lanes[32] = {};
};

struct Tint
{
    int color = 0;
};

struct Label
{
    std::string value;
};

struct Squad
{
    ecs::entity leader;
    int morale = 0;

    void ecs_relink(const ecs::graft_map& map) { leader = map.resolve(leader); }
};

struct ForeignLink
{
    ecs::entity target;
};

template <>
struct ecs::relink_traits<ForeignLink>
{
    static constexpr bool links = true;

    static void relink(ForeignLink& value, const ecs::graft_map& map)
    {
        value.target = map.resolve(value.target);
    }
};

struct GpuUploads
{
    int uploaded = 0;
    int freed = 0;

    void upload(ecs::registry&, ecs::entity) { ++uploaded; }
    void free_one(ecs::entity) { ++freed; }
};

struct Telemetry
{
    int v = 0;
};

struct telemetry_pool : ecs::packed_pool_of<Telemetry>
{
    using base = ecs::packed_pool_of<Telemetry>;
    using base::base;

    inline static int erases = 0;
    inline static int wipes = 0;

    void erase_if_present(std::uint32_t index) noexcept override
    {
        ++erases;
        base::erase_if_present(index);
    }

    void wipe() noexcept override
    {
        ++wipes;
        base::wipe();
    }
};

struct LabeledA
{
    int v = 0;
    static constexpr std::string_view ecs_label = "test.labeled_a";
};

struct ForeignVec
{
    float x = 0;
};

struct DupLabel1
{
    int v = 0;
    static constexpr std::string_view ecs_label = "test.duplicate";
};

struct DupLabel2
{
    int v = 0;
    static constexpr std::string_view ecs_label = "test.duplicate";
};

template <>
struct ecs::pool_of<Telemetry>
{
    using type = telemetry_pool;
};

struct Audit
{
    int v = 0;
    static constexpr auto ecs_storage = ecs::storage::stable;
};

struct audit_pool : ecs::stable_pool_of<Audit>
{
    using base = ecs::stable_pool_of<Audit>;
    using base::base;

    inline static int erases = 0;

    void erase_if_present(std::uint32_t index) noexcept override
    {
        ++erases;
        base::erase_if_present(index);
    }
};

template <>
struct ecs::pool_of<Audit>
{
    using type = audit_pool;
};

template <>
inline constexpr std::string_view ecs::component_label<ForeignVec> = "test.foreign_vec";

namespace hookfree
{
inline int free_hits = 0;
inline void on_free_fn(ecs::registry&, ecs::entity)
{
    ++free_hits;
}
inline void on_free_short(ecs::entity)
{
    ++free_hits;
}
}

struct TunedStable
{
    int v = 0;
    static constexpr auto ecs_storage = ecs::storage::stable;
    static constexpr std::size_t ecs_chunk_items = 8;
};

struct OnlyHere1
{
    int a = 0;
};

struct OnlyHere2
{
    int b = 0;
};

TEST(Core, EntityBasics)
{
    EXPECT_EQ(ecs::entity{}, ecs::no_entity);
    EXPECT_FALSE(ecs::no_entity);
    EXPECT_EQ(ecs::no_entity.bits(), (std::uint64_t{0xFFFFFFFFu} << 32));

    ecs::registry w;
    const ecs::entity first = w.create();
    EXPECT_TRUE(static_cast<bool>(first));
    EXPECT_NE(first, ecs::no_entity);
    EXPECT_FALSE(w.alive(ecs::no_entity));
    EXPECT_TRUE(w.alive(first));
    EXPECT_TRUE(first.index() == 0 && first.generation() == 0);

    std::unordered_set<ecs::entity> set;
    set.insert(first);
    set.insert(ecs::no_entity);
    EXPECT_EQ(set.size(), 2);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, LifecycleAndReuse)
{
    ecs::registry w;
    const ecs::entity a = w.create();
    const ecs::entity b = w.create();
    EXPECT_EQ(w.live_count(), 2);
    EXPECT_EQ(w.slot_count(), 2);

    w.destroy(a);
    EXPECT_FALSE(w.alive(a));
    EXPECT_TRUE(w.alive(b));
    EXPECT_EQ(w.live_count(), 1);

    const ecs::entity reused = w.create();
    EXPECT_EQ(reused.index(), a.index());
    EXPECT_EQ(reused.generation(), a.generation() + 1);
    EXPECT_FALSE(w.alive(a));
    EXPECT_TRUE(w.alive(reused));

    EXPECT_EQ(w.current_handle(reused.index()), reused);
    EXPECT_EQ(w.current_handle(9999), ecs::no_entity);

    EXPECT_FALSE(w.has<Pos>(a));
    EXPECT_EQ(w.find<Pos>(a), nullptr);

    w.reserve_entities(100);
    EXPECT_GE(w.capacity(), 100);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, ComponentBasics)
{
    ecs::registry w;
    const ecs::entity e = w.create();

    Pos& p = w.add<Pos>(e, Pos{7});
    EXPECT_EQ(p.x, 7);
    EXPECT_TRUE(w.has<Pos>(e));
    EXPECT_EQ(w.get<Pos>(e).x, 7);
    EXPECT_EQ(w.find<Pos>(e), &p);

    Pos* before = w.find<Pos>(e);
    Pos& replaced = w.replace<Pos>(e, Pos{8});
    EXPECT_EQ(&replaced, before);
    EXPECT_EQ(w.get<Pos>(e).x, 8);

    w.put<Pos>(e, Pos{9});
    EXPECT_EQ(w.get<Pos>(e).x, 9);
    w.put<Vel>(e, Vel{3});
    EXPECT_EQ(w.get<Vel>(e).v, 3);

    EXPECT_TRUE(w.remove<Pos>(e));
    EXPECT_FALSE(w.has<Pos>(e));
    EXPECT_FALSE(w.remove<Pos>(e));

    const ecs::registry& cw = w;
    EXPECT_EQ(cw.get<Vel>(e).v, 3);
    EXPECT_NE(cw.find<Vel>(e), nullptr);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, DestructorBalancePacked)
{
    Counted::total_ctors = 0;
    Counted::total_dtors = 0;
    {
        ecs::registry w;
        std::vector<ecs::entity> es;
        for (int i = 0; i < 64; ++i)
        {
            es.push_back(w.create(Counted{i}));
        }
        for (int i = 0; i < 64; i += 2)
        {
            w.remove<Counted>(es[i]);
        }
        for (int i = 1; i < 64; i += 4)
        {
            w.destroy(es[i]);
        }
        w.put<Counted>(es[3], Counted{99});
        w.purge<Counted>();
        for (int i = 0; i < 8; ++i)
        {
            w.create(Counted{i});
        }
        w.reset();
        for (int i = 0; i < 8; ++i)
        {
            w.create(Counted{i});
        }
        EXPECT_TRUE(RegistryValid(w));
    }
    EXPECT_EQ(Counted::live, 0);
    EXPECT_EQ(Counted::total_ctors, Counted::total_dtors);
}

TEST(Core, Tags)
{
    static_assert(ecs::storage_policy<TagA> == ecs::storage::tag);
    static_assert(ecs::storage_policy<EmptyPacked> == ecs::storage::packed);

    ecs::registry w;
    const ecs::entity a = w.create(Pos{1});
    const ecs::entity b = w.create(Pos{2});
    w.add<TagA>(a);

    EXPECT_TRUE(w.has<TagA>(a));
    EXPECT_FALSE(w.has<TagA>(b));

    int visited = 0;
    w.each<Pos, TagA>(
        [&](ecs::entity e, Pos& pos)
        {
            ++visited;
            EXPECT_EQ(e, a);
            EXPECT_EQ(pos.x, 1);
        });
    EXPECT_EQ(visited, 1);

    visited = 0;
    w.each<TagA>(
        [&](ecs::entity e)
        {
            ++visited;
            EXPECT_EQ(e, a);
        });
    EXPECT_EQ(visited, 1);

    EXPECT_TRUE(w.remove<TagA>(a));
    EXPECT_FALSE(w.has<TagA>(a));

    w.add<EmptyPacked>(a);
    EXPECT_NE(w.find<EmptyPacked>(a), nullptr);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, MoveOnly)
{
    ecs::registry w;
    const ecs::entity e = w.create();
    w.add<MoveOnly>(e, MoveOnly{41});
    EXPECT_EQ(*w.get<MoveOnly>(e).box, 41);

    const ecs::entity e2 = w.create();
    w.add<MoveOnly>(e2, MoveOnly{42});
    EXPECT_TRUE(w.remove<MoveOnly>(e));
    EXPECT_EQ(*w.get<MoveOnly>(e2).box, 42);

    ecs::command_buffer cmd;
    cmd.add<MoveOnly>(e, MoveOnly{43});
    const ecs::apply_result r = w.apply(cmd);
    EXPECT_EQ(r.applied, 1);
    EXPECT_EQ(*w.get<MoveOnly>(e).box, 43);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, StableStorage)
{
    static_assert(ecs::storage_policy<Stable> == ecs::storage::stable);

    ecs::registry w;
    std::vector<ecs::entity> es;
    std::vector<const Stable*> pointers;
    for (int i = 0; i < 1000; ++i)
    {
        const ecs::entity e = w.create();
        pointers.push_back(&w.add<Stable>(e, Stable{i}));
        es.push_back(e);
    }
    for (int i = 0; i < 1000; i += 2)
    {
        w.remove<Stable>(es[i]);
    }
    for (int i = 0; i < 1000; ++i)
    {
        w.add<Stable>(w.create(), Stable{10000 + i});
    }
    for (int i = 1; i < 1000; i += 2)
    {
        EXPECT_EQ(w.find<Stable>(es[i]), pointers[i]);
        EXPECT_EQ(pointers[i]->value, i);
    }
    EXPECT_TRUE(RegistryValid(w));

    Counted::total_ctors = 0;
    Counted::total_dtors = 0;
    {
        ecs::registry sw;
        std::vector<ecs::entity> ses;
        for (int i = 0; i < 100; ++i)
        {
            const ecs::entity e = sw.create();
            sw.add<StableCounted>(e, StableCounted{i});
            ses.push_back(e);
        }
        for (int i = 0; i < 50; ++i)
        {
            sw.remove<StableCounted>(ses[i]);
        }
        for (int i = 0; i < 25; ++i)
        {
            sw.add<StableCounted>(sw.create(), StableCounted{i});
        }
        sw.destroy(ses[60]);
        sw.purge<StableCounted>();
        sw.add<StableCounted>(sw.create(), StableCounted{1});
        sw.reset();
        sw.add<StableCounted>(sw.create(), StableCounted{2});
        EXPECT_TRUE(RegistryValid(sw));
    }
    EXPECT_EQ(Counted::live, 0);
    EXPECT_EQ(Counted::total_ctors, Counted::total_dtors);
}

TEST(Core, Iteration)
{
    ecs::registry w;
    const ecs::entity a = w.create(Pos{1}, Vel{10});
    const ecs::entity b = w.create(Pos{2}, Vel{20});
    const ecs::entity c = w.create(Pos{3});
    (void)c;

    int sum = 0;
    w.each<Pos>([&](Pos& p) { sum += p.x; });
    EXPECT_EQ(sum, 6);

    sum = 0;
    w.each<Pos, Vel>([&](ecs::entity, Pos& p, Vel& v) { sum += p.x * v.v; });
    EXPECT_EQ(sum, 50);

    w.add<Hp>(a, Hp{5});
    w.add<Hp>(b, Hp{6});
    sum = 0;
    w.each<Pos, Vel, Hp>([&](Pos& p, Vel& v, Hp& h) { sum += p.x + v.v + h.hp; });
    EXPECT_EQ(sum, 44);

    std::size_t generic_arity = 0;
    w.each<Pos, Vel>(
        [&](auto&&... args)
        {
            (..., static_cast<void>(args));
            generic_arity = sizeof...(args);
        });
    EXPECT_EQ(generic_arity, 3);

    w.each<Pos, const Vel>([](Pos& p, const Vel& v) { p.x += v.v; });
    EXPECT_EQ(w.get<Pos>(a).x, 11);
    EXPECT_EQ(w.get<Pos>(b).x, 22);

    const ecs::registry& cw = w;
    sum = 0;
    cw.each<Pos>([&](const Pos& p) { sum += p.x; });
    EXPECT_EQ(sum, 36);

    int visits = 0;
    w.each<Pos>(
        [&](ecs::entity, const Pos&)
        {
            ++visits;
            return false;
        });
    EXPECT_EQ(visits, 1);

    visits = 0;
    auto sel = w.view<Pos, Vel>();
    sel.entities(
        [&](ecs::entity e)
        {
            ++visits;
            EXPECT_TRUE(e == a || e == b);
        });
    EXPECT_EQ(visits, 2);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, FiltersAndDriver)
{
    ecs::registry w;

    std::vector<ecs::entity> expected;
    for (int i = 0; i < 50; ++i)
    {
        w.create(Pos{i});
    }
    const ecs::entity v_only = w.create();
    w.add<Vel>(v_only, Vel{1});
    for (int i = 0; i < 5; ++i)
    {
        const ecs::entity e = w.create(Pos{100 + i}, Vel{i});
        if (i % 2 == 0)
        {
            w.add<TagB>(e);
        }
        else
        {
            expected.push_back(e);
        }
    }

    auto sel = w.view<Pos, Vel>(!ecs::exists<TagB>{});

    std::vector<ecs::entity> from_each;
    sel.each([&](ecs::entity e, Pos&, Vel&) { from_each.push_back(e); });
    std::vector<ecs::entity> from_entities;
    sel.entities([&](ecs::entity e) { from_entities.push_back(e); });

    EXPECT_EQ(from_each, from_entities);
    EXPECT_EQ(from_each.size(), expected.size());
    EXPECT_EQ(sel.count(), expected.size());
    EXPECT_FALSE(sel.empty());
    for (const ecs::entity e : expected)
    {
        EXPECT_TRUE(sel.contains(e));
    }
    EXPECT_FALSE(sel.contains(v_only));

    std::vector<ecs::entity> vel_order;
    w.view<Vel>().entities(
        [&](ecs::entity e)
        {
            if (sel.contains(e))
            {
                vel_order.push_back(e);
            }
        });
    EXPECT_EQ(from_each, vel_order);

    EXPECT_EQ(w.view<Vel>().count(), 6);

    EXPECT_EQ((w.view<Pos, Vel>().count()), 5);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, SelectionReuseAndConstWorld)
{
    ecs::registry w;
    auto movers = w.view<Pos, Vel>();
    EXPECT_EQ(movers.count(), 0);

    const ecs::entity e = w.create(Pos{1}, Vel{2});
    EXPECT_EQ(movers.count(), 1);

    const ecs::registry cold;
    auto cold_sel = cold.view<Pos>();
    EXPECT_EQ(cold_sel.count(), 0);
    EXPECT_TRUE(cold_sel.empty());
    int cold_visits = 0;
    cold_sel.each([&](const Pos&) { ++cold_visits; });
    EXPECT_EQ(cold_visits, 0);

    w.reset();
    EXPECT_EQ(movers.count(), 0);
    EXPECT_FALSE(w.alive(e));
    w.create(Pos{1}, Vel{2});
    EXPECT_EQ(movers.count(), 1);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, CommandBufferBasics)
{
    ecs::registry w;
    const ecs::entity a = w.create(Pos{1});
    const ecs::entity b = w.create(Pos{2}, Hp{10});

    ecs::command_buffer cmd;

    cmd.add<Vel>(a, Vel{5});
    cmd.remove<Vel>(a);
    cmd.remove<Hp>(b);
    cmd.add<Hp>(b, Hp{20});
    ecs::apply_result r = w.apply(cmd);
    EXPECT_TRUE(r.applied == 4 && r.skipped == 0);
    EXPECT_FALSE(w.has<Vel>(a));
    EXPECT_EQ(w.get<Hp>(b).hp, 20);

    cmd.put<Pos>(a, Pos{7});
    cmd.destroy(b);
    r = w.apply(cmd);
    EXPECT_EQ(r.applied, 2);
    EXPECT_EQ(w.get<Pos>(a).x, 7);
    EXPECT_FALSE(w.alive(b));

    cmd.add<Vel>(a, Vel{9});
    cmd.destroy(a);
    w.destroy(a);
    const ecs::entity recycled = w.create();
    EXPECT_EQ(recycled.index(), a.index());
    r = w.apply(cmd);
    EXPECT_TRUE(r.applied == 0 && r.skipped == 2);
    EXPECT_FALSE(w.has<Vel>(recycled));
    EXPECT_TRUE(w.alive(recycled));

    const ecs::entity c = w.create();
    cmd.destroy(c);
    cmd.destroy(c);
    r = w.apply(cmd);
    EXPECT_TRUE(r.applied == 1 && r.skipped == 1);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, CommandBufferSpawn)
{
    ecs::registry w;
    ecs::command_buffer cmd;

    const ecs::entity ghost = cmd.create();
    EXPECT_FALSE(w.alive(ghost));
    cmd.add<Pos>(ghost, Pos{77});
    cmd.add<TagA>(ghost);
    const ecs::entity ghost2 = cmd.create();
    cmd.add<Pos>(ghost2, Pos{78});
    cmd.destroy(ghost2);

    const ecs::apply_result r = w.apply(cmd);
    EXPECT_TRUE(r.applied == 6 && r.skipped == 0);

    EXPECT_EQ(w.live_count(), 1);
    int found = 0;
    w.each<Pos, TagA>(
        [&](ecs::entity, Pos& p)
        {
            ++found;
            EXPECT_EQ(p.x, 77);
        });
    EXPECT_EQ(found, 1);

    ecs::command_buffer packs;
    const ecs::entity prov = packs.create(Pos{5}, TagA{});
    EXPECT_FALSE(w.alive(prov));
    packs.add<Hp>(prov, Hp{9});
    const ecs::entity prov2 = packs.create(MoveOnly{43});

    ecs::entity real1 = ecs::no_entity;
    ecs::entity real2 = ecs::no_entity;
    const ecs::apply_result pr = w.apply(packs,
                                         [&](ecs::entity provisional, ecs::entity real)
                                         { (provisional == prov ? real1 : real2) = real; });
    EXPECT_TRUE(pr.applied == 6 && pr.skipped == 0);
    EXPECT_NE(prov2, prov);
    EXPECT_TRUE(w.get<Pos>(real1).x == 5 && w.has<TagA>(real1));
    EXPECT_EQ(w.get<Hp>(real1).hp, 9);
    EXPECT_EQ(*w.get<MoveOnly>(real2).box, 43);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, CommandBufferPayloads)
{
    anchored_betrayals = 0;
    misaligned_payloads = 0;
    Counted::total_ctors = 0;
    Counted::total_dtors = 0;

    ecs::registry w;
    std::vector<ecs::entity> targets;
    for (int i = 0; i < 400; ++i)
    {
        targets.push_back(w.create());
    }

    {
        ecs::command_buffer cmd;
        for (int i = 0; i < 400; ++i)
        {
            cmd.add<Anchored>(targets[i]);
            if (i % 3 == 0)
            {
                cmd.add<Aligned>(targets[i]);
            }
            cmd.add<Counted>(targets[i], Counted{i});
        }
        for (int i = 100; i < 200; ++i)
        {
            w.destroy(targets[i]);
        }
        const ecs::apply_result r = w.apply(cmd);
        EXPECT_GT(r.skipped, 0);
        EXPECT_GT(r.applied, 0);
    }

    {
        ecs::command_buffer never_applied;
        never_applied.add<Counted>(targets[0], Counted{1});
        never_applied.add<Counted>(targets[1], Counted{2});
    }

    EXPECT_EQ(anchored_betrayals, 0);
    EXPECT_EQ(misaligned_payloads, 0);
    w.reset();
    EXPECT_EQ(Counted::live, 0);
    EXPECT_EQ(Counted::total_ctors, Counted::total_dtors);
    EXPECT_TRUE(RegistryValid(w));
}

#if ECS_CHECKS
TEST(Core, Violations)
{
    ecs::registry w;
    const ecs::entity a = w.create(Pos{1});
    const ecs::entity b = w.create(Pos{2});
    w.reserve<Pos>(64);

    {
        violation_scope guard;
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                w.destroy(a);
                return false;
            });
        EXPECT_EQ(violations_seen, 1);
        EXPECT_TRUE(w.alive(a));
    }

    {
        violation_scope guard;
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                EXPECT_FALSE(w.remove<Pos>(b));
                return false;
            });
        EXPECT_EQ(violations_seen, 1);
        EXPECT_TRUE(w.has<Pos>(b));
    }

    {
        violation_scope guard;
        const ecs::entity fresh = w.create();
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                w.add<Pos>(fresh, Pos{50});
                return false;
            });
        EXPECT_EQ(violations_seen, 1);
        EXPECT_EQ(w.get<Pos>(fresh).x, 50);
        w.remove<Pos>(fresh);
    }

    {
        violation_scope guard;
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                w.create(Pos{60});
                return false;
            });
        EXPECT_EQ(violations_seen, 1);
    }

    {
        violation_scope guard;
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                const ecs::entity quiet = w.create();
                EXPECT_TRUE(w.alive(quiet));
                return false;
            });
        EXPECT_EQ(violations_seen, 0);
    }

    {
        violation_scope guard;
        ecs::command_buffer cmd;
        cmd.add<Vel>(a, Vel{1});
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                const ecs::apply_result r = w.apply(cmd);
                EXPECT_TRUE(r.applied == 0 && r.skipped == 0);
                return false;
            });
        EXPECT_EQ(violations_seen, 1);
        EXPECT_FALSE(w.has<Vel>(a));
        const ecs::apply_result r = w.apply(cmd);
        EXPECT_EQ(r.applied, 1);
        EXPECT_TRUE(w.has<Vel>(a));
    }

    {
        violation_scope guard;
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                w.purge<Pos>();
                w.reset();
                w.shrink();
                return false;
            });
        EXPECT_EQ(violations_seen, 3);
        EXPECT_TRUE(w.alive(a));
        EXPECT_TRUE(w.has<Pos>(a));
    }

    {
        violation_scope guard;
        w.each<Pos>(
            [&](ecs::entity ent, Pos&)
            {
                if (ent == a)
                {
                    w.add<Hp>(a, Hp{1});
                    w.remove<Hp>(a);
                }
            });
        EXPECT_EQ(violations_seen, 0);
    }

    {
        violation_scope guard;
        const ecs::entity victim = w.create();
        w.destroy(victim);
        w.destroy(victim);
        EXPECT_EQ(violations_seen, 1);
        w.remove<Pos>(victim);
        EXPECT_EQ(violations_seen, 2);
        EXPECT_FALSE(w.has<Pos>(victim));
        EXPECT_EQ(w.find<Pos>(victim), nullptr);
        EXPECT_EQ(violations_seen, 2);
    }

    w.each<Pos>([](ecs::entity, Pos&) { return false; });
    EXPECT_EQ(ecs::test_access::lock_count<Pos>(w), 0);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, NegativeValidation)
{
    {
        ecs::registry w;
        const ecs::entity e = w.create(Pos{1});
        EXPECT_TRUE(RegistryValid(w));
        ecs::test_access::corrupt_sparse<Pos>(w, e);
        const auto r = w.validate();
        EXPECT_FALSE(r.has_value());
        if (!r)
        {
            EXPECT_EQ(r.error().code, ecs::fault_code::sparse_dense_desync);
            EXPECT_EQ(r.error().pool, ecs::name_of<Pos>());
        }
    }
    {
        ecs::registry w;
        const ecs::entity e = w.create(Pos{1});
        ecs::test_access::corrupt_generation(w, e);
        const auto r = w.validate();
        EXPECT_FALSE(r.has_value());
        if (!r)
        {
            EXPECT_EQ(r.error().code, ecs::fault_code::dense_entity_dead);
        }
    }
}
#endif

TEST(Core, Relationships)
{
    ecs::registry w;
    const ecs::entity parent = w.create();
    const ecs::entity c1 = w.create();
    const ecs::entity c2 = w.create();
    const ecs::entity c3 = w.create();

    w.adopt(parent, c1);
    w.adopt(parent, c2);
    w.adopt(parent, c3);
    EXPECT_EQ(w.parent_of(c1), parent);
    EXPECT_EQ(w.parent_of(parent), ecs::no_entity);

    std::vector<ecs::entity> kids;
    w.children_of(parent, [&](ecs::entity c) { kids.push_back(c); });
    EXPECT_EQ(kids, (std::vector<ecs::entity>{c1, c2, c3}));

    kids.clear();
    w.children_of(parent,
                  [&](ecs::entity c)
                  {
                      kids.push_back(c);
                      return false;
                  });
    EXPECT_EQ(kids.size(), 1);

    w.destroy(c2);
    kids.clear();
    w.children_of(parent, [&](ecs::entity c) { kids.push_back(c); });
    EXPECT_EQ(kids, (std::vector<ecs::entity>{c1, c3}));
    EXPECT_TRUE(RegistryValid(w));

    const ecs::entity foster = w.create();
    w.adopt(foster, c1);
    EXPECT_EQ(w.parent_of(c1), foster);
    kids.clear();
    w.children_of(parent, [&](ecs::entity c) { kids.push_back(c); });
    EXPECT_EQ(kids, (std::vector<ecs::entity>{c3}));

    w.orphan(c3);
    EXPECT_EQ(w.parent_of(c3), ecs::no_entity);
    EXPECT_TRUE(w.alive(c3));

    w.adopt(parent, c3);
    w.destroy(parent);
    EXPECT_TRUE(w.alive(c3));
    EXPECT_EQ(w.parent_of(c3), ecs::no_entity);
    EXPECT_TRUE(RegistryValid(w));

#if ECS_CHECKS
    {
        violation_scope guard;
        w.adopt(c3, c3);
        EXPECT_EQ(violations_seen, 1);
        const ecs::entity x = w.create();
        const ecs::entity y = w.create();
        w.adopt(x, y);
        w.adopt(y, x);
        EXPECT_EQ(violations_seen, 2);
        EXPECT_EQ(w.parent_of(x), ecs::no_entity);

        w.children_of(x,
                      [&](ecs::entity child)
                      {
                          w.orphan(child);
                          return false;
                      });
        EXPECT_EQ(violations_seen, 3);
        EXPECT_EQ(w.parent_of(y), x);
    }
    EXPECT_TRUE(RegistryValid(w));
#endif
}

TEST(Core, OrderedChildren)
{
    ecs::registry w;
    const ecs::entity p = w.create();
    const ecs::entity c1 = w.create();
    const ecs::entity c2 = w.create();
    const ecs::entity c3 = w.create();
    w.adopt(p, c1);
    w.adopt(p, c2);
    w.adopt(p, c3);

    const auto kids = [&]
    {
        std::vector<ecs::entity> v;
        w.children_of(p, [&](ecs::entity c) { v.push_back(c); });
        return v;
    };

    EXPECT_EQ(kids(), (std::vector<ecs::entity>{c1, c2, c3}));

    w.reorder_child(c1);
    EXPECT_EQ(kids(), (std::vector<ecs::entity>{c2, c3, c1}));

    w.reorder_child(c3, c2);
    EXPECT_EQ(kids(), (std::vector<ecs::entity>{c3, c2, c1}));

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, RelationshipHooks)
{
    ecs::registry w;
    const ecs::entity p1 = w.create();
    const ecs::entity p2 = w.create();
    const ecs::entity c = w.create();

    struct Rec
    {
        ecs::entity child;
        ecs::entity parent;
        int kind;
    };
    std::vector<Rec> log;

    ecs::scoped_relationship_hook ah(
        w,
        w.on_adopt(
            +[](ecs::registry&, ecs::entity ch, ecs::entity pa, void* u)
            { static_cast<std::vector<Rec>*>(u)->push_back(Rec{ch, pa, 0}); },
            &log));
    ecs::scoped_relationship_hook oh(
        w,
        w.on_orphan(
            +[](ecs::registry&, ecs::entity ch, ecs::entity pa, void* u)
            { static_cast<std::vector<Rec>*>(u)->push_back(Rec{ch, pa, 1}); },
            &log));
    ecs::scoped_relationship_hook rh(
        w,
        w.on_reorder(
            +[](ecs::registry&, ecs::entity ch, ecs::entity pa, void* u)
            { static_cast<std::vector<Rec>*>(u)->push_back(Rec{ch, pa, 2}); },
            &log));

    w.adopt(p1, c);
    ASSERT_EQ(log.size(), 1U);
    EXPECT_EQ(log[0].child, c);
    EXPECT_EQ(log[0].parent, p1);
    EXPECT_EQ(log[0].kind, 0);

    const ecs::entity c2 = w.create();
    w.adopt(p1, c2);
    ASSERT_EQ(log.size(), 2U);

    w.reorder_child(c2, c);
    ASSERT_EQ(log.size(), 3U);
    EXPECT_EQ(log[2].child, c2);
    EXPECT_EQ(log[2].parent, p1);
    EXPECT_EQ(log[2].kind, 2);

    w.adopt(p2, c);
    ASSERT_EQ(log.size(), 5U);
    EXPECT_EQ(log[3].kind, 1);
    EXPECT_EQ(log[3].child, c);
    EXPECT_EQ(log[3].parent, p1);
    EXPECT_EQ(log[4].kind, 0);
    EXPECT_EQ(log[4].child, c);
    EXPECT_EQ(log[4].parent, p2);

    w.orphan(c);
    ASSERT_EQ(log.size(), 6U);
    EXPECT_EQ(log[5].kind, 1);
    EXPECT_EQ(log[5].parent, p2);

    ah.release();
    oh.release();
    rh.release();
    w.adopt(p1, c);
    EXPECT_EQ(log.size(), 6U);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, RelationshipTraversal)
{
    ecs::registry w;
    const ecs::entity root = w.create();
    const ecs::entity a = w.create();
    const ecs::entity b = w.create();
    const ecs::entity a1 = w.create();
    const ecs::entity a2 = w.create();
    w.adopt(root, a);
    w.adopt(root, b);
    w.adopt(a, a1);
    w.adopt(a, a2);

    std::vector<ecs::entity> seen;
    w.descendants_of(root, [&](ecs::entity e) { seen.push_back(e); });
    EXPECT_EQ(seen, (std::vector<ecs::entity>{a, a1, a2, b}));

    std::vector<ecs::entity> up;
    w.ancestors_of(a1, [&](ecs::entity e) { up.push_back(e); });
    EXPECT_EQ(up, (std::vector<ecs::entity>{a, root}));

    EXPECT_EQ(w.root_of(a1), root);
    EXPECT_EQ(w.root_of(root), root);
    EXPECT_EQ(w.depth_of(a1), 2U);
    EXPECT_EQ(w.depth_of(a), 1U);
    EXPECT_EQ(w.depth_of(root), 0U);

    seen.clear();
    w.descendants_of(root,
                     [&](ecs::entity e)
                     {
                         seen.push_back(e);
                         return false;
                     });
    EXPECT_EQ(seen.size(), 1U);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, Roots)
{
    ecs::registry w;
    const ecs::entity r1 = w.create();
    const ecs::entity r2 = w.create();
    const ecs::entity child = w.create();
    const ecs::entity loose = w.create();
    w.adopt(r1, child);
    w.adopt(r2, w.create());

    std::vector<ecs::entity> found;
    w.roots([&](ecs::entity e) { found.push_back(e); });
    std::ranges::sort(found, [](ecs::entity x, ecs::entity y) { return x.bits() < y.bits(); });
    std::vector<ecs::entity> expected{r1, r2};
    std::ranges::sort(expected, [](ecs::entity x, ecs::entity y) { return x.bits() < y.bits(); });
    EXPECT_EQ(found, expected);
    EXPECT_TRUE(std::ranges::find(found, loose) == found.end());
    EXPECT_TRUE(std::ranges::find(found, child) == found.end());
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, DestroySubtree)
{
    ecs::registry w;
    const ecs::entity root = w.create();
    const ecs::entity a = w.create();
    const ecs::entity b = w.create();
    const ecs::entity a1 = w.create();
    const ecs::entity a2 = w.create();
    w.adopt(root, a);
    w.adopt(root, b);
    w.adopt(a, a1);
    w.adopt(a, a2);

    const std::size_t destroyed = w.destroy_subtree(a);
    EXPECT_EQ(destroyed, 3U);
    EXPECT_FALSE(w.alive(a));
    EXPECT_FALSE(w.alive(a1));
    EXPECT_FALSE(w.alive(a2));
    EXPECT_TRUE(w.alive(root));
    EXPECT_TRUE(w.alive(b));

    std::vector<ecs::entity> kids;
    w.children_of(root, [&](ecs::entity c) { kids.push_back(c); });
    EXPECT_EQ(kids, (std::vector<ecs::entity>{b}));
    EXPECT_TRUE(RegistryValid(w));
}

namespace
{
struct NamedTag
{
};
}

TEST(Core, DeferredRelationships)
{
    ecs::registry w;
    const ecs::entity p = w.create();
    const ecs::entity p2 = w.create();
    const ecs::entity a = w.create();
    const ecs::entity a1 = w.create();
    w.adopt(p, a);
    w.adopt(a, a1);

    ecs::command_buffer cmd;
    cmd.orphan(a);
    w.apply(cmd);
    EXPECT_EQ(w.parent_of(a), ecs::no_entity);

    cmd.adopt(p2, a);
    w.apply(cmd);
    EXPECT_EQ(w.parent_of(a), p2);

    cmd.destroy_subtree(p2);
    w.apply(cmd);
    EXPECT_FALSE(w.alive(p2));
    EXPECT_FALSE(w.alive(a));
    EXPECT_FALSE(w.alive(a1));

    const ecs::entity ghost = cmd.create(NamedTag{});
    cmd.adopt(p, ghost);
    w.apply(cmd);
    const ecs::entity spawned = w.view<NamedTag>().first();
    EXPECT_TRUE(w.alive(spawned));
    EXPECT_EQ(w.parent_of(spawned), p);
    EXPECT_TRUE(RegistryValid(w));
}

#if ECS_CHECKS
TEST(Core, RelationshipChecks)
{
    violation_scope guard;
    ecs::registry w;
    const ecs::entity p = w.create();
    const ecs::entity c1 = w.create();
    const ecs::entity c2 = w.create();
    w.adopt(p, c1);
    w.adopt(p, c2);

    w.children_of(p,
                  [&](ecs::entity)
                  {
                      w.reorder_child(c1);
                      return false;
                  });
    EXPECT_EQ(violations_seen, 1);

    w.children_of(p,
                  [&](ecs::entity)
                  {
                      w.destroy_subtree(c1);
                      return false;
                  });
    EXPECT_GE(violations_seen, 2);

    const int before = violations_seen;
    const ecs::entity stranger = w.create();
    w.reorder_child(c1, stranger);
    EXPECT_EQ(violations_seen, before + 1);

    w.reorder_child(stranger);
    EXPECT_EQ(violations_seen, before + 2);

    EXPECT_TRUE(RegistryValid(w));
}
#endif

TEST(Core, Snapshot)
{
    ecs::registry source;

    std::vector<ecs::entity> es;
    for (int i = 0; i < 10; ++i)
    {
        es.push_back(source.create(Pos{i}));
    }
    source.destroy(es[3]);
    source.destroy(es[6]);
    es[3] = source.create(Pos{33});
    es[6] = source.create(Pos{66});
    source.add<TagA>(es[6]);

    std::vector<ecs::entity> order;
    source.each([&](ecs::entity e) { order.push_back(e); });
    EXPECT_EQ(order.size(), source.live_count());
    for (std::size_t i = 1; i < order.size(); ++i)
    {
        EXPECT_LT(order[i - 1].index(), order[i].index());
    }

    int seen = 0;
    source.each(
        [&](ecs::entity)
        {
            ++seen;
            return false;
        });
    EXPECT_EQ(seen, 1);

    struct Row
    {
        ecs::entity e;
        Pos pos;
        bool tagged;
    };
    std::vector<Row> saved;
    source.each([&](ecs::entity e)
                { saved.push_back(Row{e, *source.find<Pos>(e), source.has<TagA>(e)}); });

    ecs::registry loaded;
    for (const Row& row : saved)
    {
        const auto restored = loaded.restore_entity(row.e);
        EXPECT_TRUE(restored.has_value());
        EXPECT_EQ(*restored, row.e);
        loaded.add<Pos>(row.e, row.pos);
        if (row.tagged)
        {
            loaded.add<TagA>(row.e);
        }
    }
    EXPECT_EQ(loaded.live_count(), source.live_count());
    for (const Row& row : saved)
    {
        EXPECT_TRUE(loaded.alive(row.e));
        EXPECT_EQ(loaded.get<Pos>(row.e).x, row.pos.x);
        EXPECT_EQ(loaded.has<TagA>(row.e), row.tagged);
    }
    EXPECT_TRUE(RegistryValid(loaded));

    std::unordered_set<std::uint32_t> indices;
    loaded.each([&](ecs::entity e) { indices.insert(e.index()); });
    for (int i = 0; i < 20; ++i)
    {
        const ecs::entity fresh = loaded.create();
        EXPECT_TRUE(indices.insert(fresh.index()).second);
    }
    EXPECT_TRUE(RegistryValid(loaded));

    ecs::registry mid;
    std::vector<ecs::entity> ms;
    for (int i = 0; i < 10; ++i)
    {
        ms.push_back(mid.create());
    }
    mid.destroy(ms[3]);
    mid.destroy(ms[5]);
    mid.destroy(ms[7]);
    const ecs::entity back = ecs::entity{ms[5].index(), ms[5].generation()};
    const auto restored = mid.restore_entity(back);
    EXPECT_TRUE(restored.has_value());
    EXPECT_TRUE(mid.alive(back));
    std::unordered_set<std::uint32_t> live_indices;
    mid.each([&](ecs::entity e) { EXPECT_TRUE(live_indices.insert(e.index()).second); });
    for (int i = 0; i < 5; ++i)
    {
        const ecs::entity fresh = mid.create();
        EXPECT_TRUE(live_indices.insert(fresh.index()).second);
    }
    EXPECT_TRUE(RegistryValid(mid));

    ecs::registry tall;
    const auto high = tall.restore_entity(ecs::entity{100, 5});
    EXPECT_TRUE(high.has_value());
    EXPECT_TRUE(tall.alive(ecs::entity{100, 5}));
    EXPECT_EQ(tall.slot_count(), 101);
    const ecs::entity filler = tall.create();
    EXPECT_LT(filler.index(), 100);
    EXPECT_TRUE(RegistryValid(tall));

    const auto occupied = tall.restore_entity(ecs::entity{100, 9});
    EXPECT_FALSE(occupied.has_value());
    EXPECT_EQ(occupied.error().code, ecs::fault_code::slot_occupied);
    const auto null_restore = tall.restore_entity(ecs::no_entity);
    EXPECT_FALSE(null_restore.has_value());
    EXPECT_EQ(null_restore.error().code, ecs::fault_code::bad_handle);
}

TEST(Core, MoveSemantics)
{
    {
        ecs::registry w;
        ecs::command_buffer a;
        const ecs::entity g1 = a.create();
        a.add<Pos>(g1, Pos{1});
        a.create();
        a.create();

        ecs::command_buffer b{std::move(a)};
        EXPECT_TRUE(a.empty());

        const ecs::entity g2 = a.create();
        a.add<Pos>(g2, Pos{2});
        ecs::apply_result r = w.apply(a);
        EXPECT_TRUE(r.applied == 2 && r.skipped == 0);
        int with_two = 0;
        w.each<Pos>([&](Pos& p) { with_two += p.x == 2 ? 1 : 0; });
        EXPECT_EQ(with_two, 1);

        r = w.apply(b);
        EXPECT_TRUE(r.applied == 4 && r.skipped == 0);
        int with_one = 0;
        w.each<Pos>([&](Pos& p) { with_one += p.x == 1 ? 1 : 0; });
        EXPECT_EQ(with_one, 1);

        Counted::total_ctors = 0;
        Counted::total_dtors = 0;
        {
            ecs::command_buffer lhs;
            ecs::command_buffer rhs;
            lhs.add<Counted>(w.create(), Counted{1});
            rhs.add<Counted>(w.create(), Counted{2});
            lhs = std::move(rhs);
            const ecs::entity g3 = lhs.create();
            lhs.add<Counted>(g3, Counted{3});
            r = w.apply(lhs);
            EXPECT_TRUE(r.applied == 3 && r.skipped == 0);
        }
        w.reset();
        EXPECT_EQ(Counted::live, 0);
        EXPECT_EQ(Counted::total_ctors, Counted::total_dtors);
        EXPECT_TRUE(RegistryValid(w));
    }

#if ECS_CHECKS
    {
        violation_scope guard;
        ecs::registry w;
        ecs::command_buffer a;
        ecs::command_buffer b;
        const ecs::entity foreign = a.create();
        b.destroy(foreign);
        b.create();
        const ecs::apply_result r = w.apply(b);
        EXPECT_EQ(violations_seen, 1);
        EXPECT_TRUE(r.applied == 1 && r.skipped == 1);
        EXPECT_EQ(w.live_count(), 1);

        ecs::command_buffer c;
        const ecs::entity stale = c.create();
        c.clear();
        c.add<Pos>(stale, Pos{9});
        const ecs::apply_result r2 = w.apply(c);
        EXPECT_EQ(violations_seen, 2);
        EXPECT_TRUE(r2.applied == 0 && r2.skipped == 1);
    }
#endif

    {
        ecs::registry w;
        ecs::command_buffer cmd;
        anchored_betrayals = 0;
        for (int round = 0; round < 3; ++round)
        {
            for (int i = 0; i < 200; ++i)
            {
                const ecs::entity e = cmd.create();
                cmd.add<Anchored>(e);
            }
            const ecs::apply_result r = w.apply(cmd);
            EXPECT_EQ(r.skipped, 0);
        }
        EXPECT_EQ(anchored_betrayals, 0);
        EXPECT_TRUE(RegistryValid(w));
    }

    {
        ecs::registry w1;
        w1.create(Pos{1}, Vel{1});
        w1.create(Pos{2});
        auto sel = w1.view<Pos>();

        ecs::registry w2{std::move(w1)};
        EXPECT_EQ(w2.live_count(), 2);
        EXPECT_EQ(sel.count(), 2);
        EXPECT_EQ(w1.live_count(), 0);
        EXPECT_EQ(w1.slot_count(), 0);
        w1.create(Pos{3});
        EXPECT_EQ(w1.live_count(), 1);
        EXPECT_TRUE(RegistryValid(w1));
        EXPECT_TRUE(RegistryValid(w2));

        ecs::registry w3;
        w3.create(Pos{7});
        w3 = std::move(w2);
        EXPECT_EQ(w3.live_count(), 2);
        EXPECT_EQ(w2.live_count(), 0);
        w2.create();
        EXPECT_TRUE(RegistryValid(w2));
        EXPECT_TRUE(RegistryValid(w3));
    }
}

TEST(Core, IterationLockUnwinding)
{
    ecs::registry w;
    const ecs::entity parent = w.create();
    const ecs::entity child = w.create();
    w.adopt(parent, child);

    struct probe_exception
    {
    };

    w.create(Pos{1});
    try
    {
        w.each<Pos>([](ecs::entity, Pos&) -> bool { throw probe_exception{}; });
    }
    catch (const probe_exception&)
    {
    }

    try
    {
        w.children_of(parent, [](ecs::entity) -> bool { throw probe_exception{}; });
    }
    catch (const probe_exception&)
    {
    }

#if ECS_CHECKS
    EXPECT_EQ(ecs::test_access::lock_count<Pos>(w), 0);
    {
        violation_scope guard;
        w.orphan(child);
        EXPECT_EQ(violations_seen, 0);
        EXPECT_EQ(w.parent_of(child), ecs::no_entity);
    }
#else
    w.orphan(child);
    EXPECT_EQ(w.parent_of(child), ecs::no_entity);
#endif
    EXPECT_TRUE(RegistryValid(w));
}

#if ECS_CHECKS
TEST(Core, TagAddRefusedDuringIteration)
{
    violation_scope guard;
    ecs::registry w;
    const ecs::entity a = w.create();
    const ecs::entity outsider = w.create();
    w.add<TagA>(a);
    w.each<TagA>(
        [&](ecs::entity)
        {
            w.add<TagA>(outsider);
            return false;
        });
    EXPECT_EQ(violations_seen, 1);
    EXPECT_FALSE(w.has<TagA>(outsider));
    EXPECT_TRUE(RegistryValid(w));
}
#endif

TEST(Core, NonMovableComponents)
{
    ecs::registry w;
    const ecs::entity e = w.create();
    Pinned& p = w.add<Pinned>(e);
    p.charge.store(41);
    p.charge.fetch_add(1);
    EXPECT_EQ(w.get<Pinned>(e).charge.load(), 42);
    EXPECT_EQ(w.find<Pinned>(e), &p);

    w.replace<Pinned>(e);
    EXPECT_EQ(w.get<Pinned>(e).charge.load(), 0);
    EXPECT_EQ(w.find<Pinned>(e), &p);

    int visited = 0;
    w.each<Pinned>([&](ecs::entity, Pinned& got) { visited += got.charge.load() == 0 ? 1 : 0; });
    EXPECT_EQ(visited, 1);

    EXPECT_TRUE(w.remove<Pinned>(e));
    w.add<Pinned>(e);
    w.purge<Pinned>();
    w.add<Pinned>(w.create());
    w.reset();
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, OveralignedCommandPayloads)
{
    misaligned_big = 0;
    ecs::registry w;
    ecs::command_buffer cmd;
    for (int round = 0; round < 2; ++round)
    {
        std::vector<ecs::entity> es;
        for (int i = 0; i < 8; ++i)
        {
            const ecs::entity e = w.create();
            cmd.add<Pos>(e, Pos{i});
            cmd.add<Big128>(e, Big128{static_cast<float>(i)});
            es.push_back(e);
        }
        const ecs::apply_result r = w.apply(cmd);
        EXPECT_EQ(r.skipped, 0);
        for (const ecs::entity e : es)
        {
            EXPECT_EQ(reinterpret_cast<std::uintptr_t>(w.find<Big128>(e)) % 128, 0);
        }
    }
    EXPECT_EQ(misaligned_big, 0);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, ApplySpawnCallback)
{
    ecs::registry w;
    ecs::command_buffer cmd;
    const ecs::entity g1 = cmd.create();
    cmd.add<Pos>(g1, Pos{1});
    const ecs::entity g2 = cmd.create();
    cmd.destroy(g2);

    std::vector<std::pair<ecs::entity, ecs::entity>> mapping;
    const ecs::apply_result r = w.apply(cmd,
                                        [&](ecs::entity provisional, ecs::entity real)
                                        { mapping.emplace_back(provisional, real); });
    EXPECT_TRUE(r.applied == 4 && r.skipped == 0);
    EXPECT_EQ(mapping.size(), 2);
    EXPECT_EQ(mapping[0].first, g1);
    EXPECT_TRUE(w.alive(mapping[0].second));
    EXPECT_EQ(w.get<Pos>(mapping[0].second).x, 1);
    EXPECT_EQ(mapping[1].first, g2);
    EXPECT_FALSE(w.alive(mapping[1].second));
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, HashIdentity)
{
    static_assert(ecs::hash_of<Pos>() != 0);
    static_assert(ecs::hash_of<Pos>() != ecs::hash_of<Vel>());
    static_assert(ecs::hash_of<Pos>() == ecs::hash_of<Pos>());

    ecs::registry w;
    w.create(Pos{1});
    bool saw = false;
    w.each_pool(
        [&](const ecs::pool_info& info)
        {
            if (info.name == ecs::name_of<Pos>())
            {
                saw = true;
                EXPECT_EQ(info.name_hash, ecs::hash_of<Pos>());
            }
        });
    EXPECT_TRUE(saw);
}

TEST(Core, RangeIteration)
{
    ecs::registry w;
    const ecs::entity a = w.create(Pos{1}, Vel{10});
    const ecs::entity b = w.create(Pos{2}, Vel{20});
    const ecs::entity c = w.create(Pos{3}, Vel{30});
    w.add<TagB>(b);

    auto sel = w.view<Pos, const Vel>(!ecs::exists<TagB>{});
    std::vector<ecs::entity> from_each;
    sel.each([&](ecs::entity e, Pos&, const Vel&) { from_each.push_back(e); });
    std::vector<ecs::entity> from_range;
    int sum = 0;
    for (auto&& [e, p, v] : sel.each())
    {
        from_range.push_back(e);
        p.x += 100;
        sum += v.v;
    }
    EXPECT_EQ(from_each, from_range);
    EXPECT_EQ(from_range, (std::vector<ecs::entity>{a, c}));
    EXPECT_EQ(sum, 40);
    EXPECT_EQ(w.get<Pos>(a).x, 101);

    for (auto&& [e, p, v] : sel.each())
    {
        static_cast<void>(p);
        static_cast<void>(v);
        if (e == a)
        {
            break;
        }
    }
#if ECS_CHECKS
    EXPECT_EQ(ecs::test_access::lock_count<Pos>(w), 0);
    EXPECT_EQ(ecs::test_access::lock_count<Vel>(w), 0);

    {
        violation_scope guard;
        auto r = w.view<Pos>().each();
        static_cast<void>(r.begin());
        EXPECT_EQ(ecs::test_access::lock_count<Pos>(w), 1);
        EXPECT_FALSE(w.remove<Pos>(c));
        EXPECT_EQ(violations_seen, 1);
    }
    EXPECT_EQ(ecs::test_access::lock_count<Pos>(w), 0);
#endif

    const ecs::registry& cw = w;
    int const_sum = 0;
    for (auto&& [e, p] : cw.view<Pos>().each())
    {
        static_cast<void>(e);
        const_sum += p.x;
    }
    EXPECT_EQ(const_sum, (101 + 2 + 103));
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, MaybeComponents)
{
    ecs::registry w;
    const ecs::entity plain = w.create(Pos{1});
    const ecs::entity tinted = w.create(Pos{2});
    w.add<Tint>(tinted, Tint{7});
    const ecs::entity excluded = w.create(Pos{3});
    w.add<TagB>(excluded);

    auto sel = w.view<Pos, ecs::maybe<Tint>>(!ecs::exists<TagB>{});
    EXPECT_EQ(sel.count(), 2);
    int with = 0;
    int without = 0;
    sel.each(
        [&](ecs::entity e, Pos&, Tint* tint)
        {
            if (tint != nullptr)
            {
                ++with;
                EXPECT_EQ(e, tinted);
                EXPECT_EQ(tint->color, 7);
                tint->color = 8;
            }
            else
            {
                ++without;
                EXPECT_EQ(e, plain);
            }
        });
    EXPECT_TRUE(with == 1 && without == 1);
    EXPECT_EQ(w.get<Tint>(tinted).color, 8);
    EXPECT_TRUE(sel.contains(plain));
    EXPECT_FALSE(sel.contains(excluded));

    EXPECT_EQ((w.view<Pos, ecs::maybe<Tint>>().count()), 3);

    for (auto&& [e, p, tint] : sel.each())
    {
        static_cast<void>(p);
        EXPECT_EQ((tint != nullptr), (e == tinted));
    }

    const ecs::registry& cw = w;
    int rows = 0;
    cw.each<Pos, ecs::maybe<Tint>>(
        [&](const Pos&, const Tint* tint)
        {
            ++rows;
            if (tint != nullptr)
            {
                EXPECT_EQ(tint->color, 8);
            }
        },
        !ecs::exists<TagB>{});
    EXPECT_EQ(rows, 2);

    ecs::registry fresh;
    fresh.create(Pos{1});
    const ecs::registry& cfresh = fresh;
    int fresh_rows = 0;
    cfresh.each<Pos, ecs::maybe<Tint>>(
        [&](const Pos&, const Tint* tint)
        {
            ++fresh_rows;
            EXPECT_EQ(tint, nullptr);
        });
    EXPECT_EQ(fresh_rows, 1);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, Sort)
{
    ecs::registry w;
    for (int i = 0; i < 32; ++i)
    {
        w.create(Pos{31 - i}, Vel{i});
    }

    w.sort<Pos>([](const Pos& a, const Pos& b) { return a.x < b.x; });
    int expected = 0;
    bool ordered = true;
    w.each<Pos>([&](const Pos& p) { ordered = ordered && p.x == expected++; });
    EXPECT_TRUE(ordered);
    EXPECT_TRUE(RegistryValid(w));

    ecs::registry sw;
    std::vector<const Stable*> ptrs;
    std::vector<ecs::entity> ses;
    for (int i = 0; i < 16; ++i)
    {
        const ecs::entity e = sw.create();
        ptrs.push_back(&sw.add<Stable>(e, Stable{100 - i}));
        ses.push_back(e);
    }
    sw.sort<Stable>([](const Stable& a, const Stable& b) { return a.value < b.value; });
    for (std::size_t i = 0; i < ses.size(); ++i)
    {
        EXPECT_EQ(sw.find<Stable>(ses[i]), ptrs[i]);
    }
    int last = -1;
    bool stable_ordered = true;
    sw.each<Stable>(
        [&](const Stable& s)
        {
            stable_ordered = stable_ordered && s.value > last;
            last = s.value;
        });
    EXPECT_TRUE(stable_ordered);
    EXPECT_TRUE(RegistryValid(sw));

    ecs::registry tw;
    const ecs::entity t1 = tw.create();
    const ecs::entity t2 = tw.create();
    tw.add<TagA>(t2);
    tw.add<TagA>(t1);
    tw.sort<TagA>([](ecs::entity a, ecs::entity b) { return a.index() < b.index(); });
    std::vector<ecs::entity> order;
    tw.view<TagA>().entities([&](ecs::entity e) { order.push_back(e); });
    EXPECT_EQ(order, (std::vector<ecs::entity>{t1, t2}));
    EXPECT_TRUE(RegistryValid(tw));

#if ECS_CHECKS
    {
        violation_scope guard;
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                w.sort<Pos>([](const Pos& a, const Pos& b) { return a.x > b.x; });
                return false;
            });
        EXPECT_EQ(violations_seen, 1);
    }
#endif
}

TEST(Core, QueryHelpers)
{
    ecs::registry w;
    EXPECT_EQ(w.view<Pos>().first(), ecs::no_entity);

    const ecs::entity a = w.create(Pos{1}, Vel{2});
    w.add<TagA>(a);
    EXPECT_EQ(w.view<TagA>().first(), a);
    EXPECT_EQ(w.view<Pos>(!ecs::exists<TagA>{}).first(), ecs::no_entity);

    std::vector<std::string_view> names;
    w.components_of(a, [&](const ecs::pool_info& info) { names.push_back(info.name); });
    EXPECT_EQ(names.size(), 3);
    w.destroy(a);
    names.clear();
    w.components_of(a, [&](const ecs::pool_info& info) { names.push_back(info.name); });
    EXPECT_TRUE(names.empty());

    const ecs::entity m = w.create(Pos{5}, Vel{6});
    auto [mp, mv] = w.get<Pos, Vel>(m);
    EXPECT_TRUE(mp.x == 5 && mv.v == 6);
    mp.x = 50;
    EXPECT_EQ(w.get<Pos>(m).x, 50);
    const ecs::registry& cw = w;
    auto [cp, cv] = cw.get<Pos, Vel>(m);
    static_assert(std::is_same_v<decltype(cp), const Pos&>);
    EXPECT_TRUE(cp.x == 50 && cv.v == 6);

    const ecs::entity parent = w.create();
    EXPECT_EQ(w.child_count(parent), 0);
    const ecs::entity c1 = w.create();
    const ecs::entity c2 = w.create();
    w.adopt(parent, c1);
    w.adopt(parent, c2);
    EXPECT_EQ(w.child_count(parent), 2);
    w.destroy(c1);
    EXPECT_EQ(w.child_count(parent), 1);

    static_assert(noexcept(w.destroy(parent)));
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, Hooks)
{
    ecs::registry w;
    struct counters
    {
        int added = 0;
        int removed = 0;
        int replaced = 0;
        int last_removed_value = -1;
        ecs::registry* expected_world = nullptr;
        bool world_ok = true;
    } c;
    c.expected_world = &w;

    const ecs::hook_token t_add = w.on_add<Pos>(
        [](ecs::registry& world, ecs::entity, void* user)
        {
            auto* k = static_cast<counters*>(user);
            k->world_ok = k->world_ok && (&world == k->expected_world);
            ++k->added;
        },
        &c);
    const ecs::hook_token t_rem = w.on_remove<Pos>(
        [](ecs::registry& world, ecs::entity e, void* user)
        {
            auto* k = static_cast<counters*>(user);
            k->last_removed_value = world.get<Pos>(e).x;
            ++k->removed;
        },
        &c);
    const ecs::hook_token t_rep = w.on_replace<Pos>([](ecs::registry&, ecs::entity, void* user)
                                                    { ++static_cast<counters*>(user)->replaced; },
                                                    &c);
    EXPECT_TRUE(static_cast<bool>(t_add) && static_cast<bool>(t_rem) && static_cast<bool>(t_rep));

    const ecs::entity a = w.create(Pos{1});
    w.add<Pos>(w.create());
    EXPECT_EQ(c.added, 2);
    EXPECT_TRUE(c.world_ok);

    w.replace<Pos>(a, Pos{5});
    w.put<Pos>(a, Pos{6});
    EXPECT_EQ(c.replaced, 2);

    w.remove<Pos>(a);
    EXPECT_EQ(c.removed, 1);
    EXPECT_EQ(c.last_removed_value, 6);

    ecs::command_buffer cmd;
    const ecs::entity g = cmd.create();
    cmd.add<Pos>(g, Pos{9});
    w.apply(cmd);
    EXPECT_EQ(c.added, 3);

    w.purge<Pos>();
    EXPECT_EQ(c.removed, 3);
    w.create(Pos{1});
    w.reset();
    EXPECT_EQ(c.removed, 4);

    EXPECT_EQ(c.added, 4);
    EXPECT_TRUE(w.unhook(t_add));
    EXPECT_FALSE(w.unhook(t_add));
    w.create(Pos{1});
    EXPECT_EQ(c.added, 4);
    EXPECT_TRUE(w.unhook(t_rem));
    EXPECT_TRUE(w.unhook(t_rep));

#if ECS_CHECKS
    {
        violation_scope guard;
        static int fired = 0;
        fired = 0;
        const ecs::hook_token t = w.on_add<Vel>(
            [](ecs::registry& world, ecs::entity e, void*)
            {
                ++fired;
                world.remove<Vel>(e);
            },
            nullptr);
        const ecs::entity v = w.create();
        w.add<Vel>(v, Vel{1});
        EXPECT_EQ(fired, 1);
        EXPECT_EQ(violations_seen, 1);
        EXPECT_TRUE(w.has<Vel>(v));
        EXPECT_TRUE(w.unhook(t));
    }
#endif
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, Blueprint)
{
    ecs::registry w;
    ecs::blueprint goblin;
    goblin.add<Pos>(7);
    goblin.add<Hp>(30);
    goblin.add<TagA>();
    EXPECT_EQ(goblin.size(), 3);

    const ecs::entity g1 = w.create(goblin);
    const ecs::entity g2 = w.create(goblin);
    EXPECT_TRUE(w.get<Pos>(g1).x == 7 && w.get<Pos>(g2).x == 7);
    EXPECT_TRUE(w.has<TagA>(g1) && w.has<TagA>(g2));

    w.get<Pos>(g1).x = 99;
    EXPECT_EQ(w.get<Pos>(g2).x, 7);
    const ecs::entity g3 = w.create(goblin);
    EXPECT_EQ(w.get<Pos>(g3).x, 7);

#if ECS_CHECKS
    {
        violation_scope guard;
        goblin.add<Pos>(1);
        EXPECT_EQ(violations_seen, 1);
        EXPECT_EQ(goblin.size(), 3);
    }
#endif

    goblin.clear();
    EXPECT_TRUE(goblin.empty());
    goblin.add<Pos>(1);
    ecs::blueprint moved{std::move(goblin)};
    const ecs::entity g4 = w.create(moved);
    EXPECT_EQ(w.get<Pos>(g4).x, 1);

    Counted::total_ctors = 0;
    Counted::total_dtors = 0;
    {
        ecs::blueprint never_used;
        never_used.add<Counted>(5);
    }
    EXPECT_EQ(Counted::total_ctors, Counted::total_dtors);

    ecs::blueprint orc{Pos{4}, Hp{12}, TagA{}};
    EXPECT_EQ(orc.size(), 3);
    const ecs::entity o1 = w.create(orc);
    const ecs::entity o2 = w.create(orc);
    EXPECT_TRUE(w.get<Pos>(o1).x == 4 && w.get<Hp>(o2).hp == 12);
    EXPECT_TRUE(w.has<TagA>(o1) && w.has<TagA>(o2));
    w.get<Pos>(o1).x = 77;
    EXPECT_EQ(w.get<Pos>(o2).x, 4);

    ecs::blueprint chained;
    chained.add<Pos>(1).add<Hp>(2);
    EXPECT_EQ(chained.size(), 2);
    const ecs::entity c1 = w.create(chained);
    EXPECT_TRUE(w.get<Pos>(c1).x == 1 && w.get<Hp>(c1).hp == 2);

    std::pmr::monotonic_buffer_resource scratch;
    ecs::blueprint arena_pack(&scratch, Pos{6});
    EXPECT_EQ(arena_pack.size(), 1);
    EXPECT_EQ(w.get<Pos>(w.create(arena_pack)).x, 6);

    Counted::total_ctors = 0;
    Counted::total_dtors = 0;
    {
        ecs::blueprint braced{Counted{5}};
        EXPECT_EQ(braced.size(), 1);
    }
    EXPECT_EQ(Counted::total_ctors, Counted::total_dtors);

    ecs::blueprint swarm{Pos{8}};
    const std::size_t before = w.view<Pos>().count();
    w.create(swarm, 4);
    EXPECT_EQ(w.view<Pos>().count(), before + 4);

    std::vector<ecs::entity> drones;
    w.create(swarm, 3, [&](ecs::entity e) { drones.push_back(e); });
    EXPECT_EQ(drones.size(), 3);
    w.get<Pos>(drones[1]).x = 80;
    EXPECT_TRUE(w.get<Pos>(drones[0]).x == 8 && w.get<Pos>(drones[2]).x == 8);

    w.create(swarm, 0);
    EXPECT_EQ(w.view<Pos>().count(), before + 7);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, Duplicate)
{
    ecs::registry w;
    const ecs::entity src = w.create(Pos{4}, Vel{5});
    w.add<TagA>(src);
    w.add<Stable>(src, Stable{6});
    w.add<MoveOnly>(src, MoveOnly{7});

    const ecs::entity parent = w.create();
    w.adopt(parent, src);

    const ecs::duplicate_result r = w.duplicate(src);
    EXPECT_TRUE(w.alive(r.clone));
    EXPECT_EQ(r.copied, 4);
    EXPECT_EQ(r.skipped, 1);
    EXPECT_EQ(w.get<Pos>(r.clone).x, 4);
    EXPECT_EQ(w.get<Stable>(r.clone).value, 6);
    EXPECT_TRUE(w.has<TagA>(r.clone));
    EXPECT_FALSE(w.has<MoveOnly>(r.clone));
    EXPECT_EQ(w.parent_of(r.clone), ecs::no_entity);
    EXPECT_EQ(w.parent_of(src), parent);

    w.get<Pos>(r.clone).x = 40;
    EXPECT_EQ(w.get<Pos>(src).x, 4);

    static_assert(std::is_convertible_v<ecs::duplicate_result, ecs::entity>);
    const ecs::entity direct = w.duplicate(src);
    EXPECT_TRUE(w.alive(direct));
    EXPECT_NE(direct, src);
    EXPECT_EQ(w.get<Pos>(direct).x, 4);

#if ECS_CHECKS
    {
        violation_scope guard;
        const ecs::duplicate_result dead = w.duplicate(ecs::no_entity);
        EXPECT_EQ(violations_seen, 1);
        EXPECT_EQ(dead.clone, ecs::no_entity);
    }
#endif
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, EntityRef)
{
    ecs::registry w;
    ecs::entity_filler hero = w.ref(w.create());
    EXPECT_TRUE(hero.alive());
    hero.add<Pos>(1);
    hero.obtain<Vel>(2);
    EXPECT_TRUE(hero.has<Pos>());
    EXPECT_EQ(hero.get<Pos>().x, 1);
    EXPECT_NE(hero.find<Vel>(), nullptr);
    hero.put<Pos>(3);
    EXPECT_EQ(hero.get<Pos>().x, 3);

    auto [mp, mv] = hero.get<Pos, Vel>();
    static_assert(std::is_same_v<decltype(mp), Pos&>);
    EXPECT_TRUE(mp.x == 3 && mv.v == 2);
    mp.x = 4;
    EXPECT_EQ(hero.get<Pos>().x, 4);
    mp.x = 3;

    ecs::const_entity_filler shade{hero};
    auto [sp, sv] = shade.get<Pos, Vel>();
    static_assert(std::is_same_v<decltype(sp), const Pos&>);
    EXPECT_TRUE(sp.x == 3 && sv.v == 2);

    EXPECT_TRUE(hero.remove<Vel>());

    const ecs::entity parent = w.create();
    w.adopt(parent, hero.id());
    EXPECT_EQ(hero.parent().id(), parent);

    const ecs::registry& cw = w;
    ecs::const_entity_filler seen = cw.ref(hero);
    EXPECT_TRUE(seen.alive());
    EXPECT_EQ(seen.get<Pos>().x, 3);
    EXPECT_NE(seen.find<Pos>(), nullptr);

    hero.destroy();
    EXPECT_FALSE(hero.alive());
    EXPECT_FALSE(seen.alive());
    EXPECT_FALSE(static_cast<bool>(hero));
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, ObtainAndFindAll)
{
    ecs::registry w;
    const ecs::entity e = w.create();

    Pos& fresh = w.obtain<Pos>(e, 5);
    EXPECT_EQ(fresh.x, 5);
    Pos& again = w.obtain<Pos>(e, 99);
    EXPECT_EQ(&again, &fresh);
    EXPECT_EQ(again.x, 5);

    w.obtain<TagA>(e);
    EXPECT_TRUE(w.has<TagA>(e));
    w.obtain<TagA>(e);

    w.add<Vel>(e, Vel{2});
    auto [p, v, h] = w.find_all<Pos, Vel, Hp>(e);
    EXPECT_TRUE(p != nullptr && p->x == 5);
    EXPECT_TRUE(v != nullptr && v->v == 2);
    EXPECT_EQ(h, nullptr);

    w.destroy(e);
    auto [dp, dv, dh] = w.find_all<Pos, Vel, Hp>(e);
    EXPECT_TRUE(dp == nullptr && dv == nullptr && dh == nullptr);

    const ecs::registry& cw = w;
    const ecs::entity e2 = w.create(Pos{1});
    auto [cp2, cv2] = cw.find_all<Pos, Vel>(e2);
    static_assert(std::is_same_v<decltype(cp2), const Pos*>);
    EXPECT_TRUE(cp2 != nullptr && cv2 == nullptr);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, RuntimeQueries)
{
    ecs::registry w;
    std::vector<ecs::entity> expected;
    for (int i = 0; i < 10; ++i)
    {
        const ecs::entity e = w.create(Pos{i});
        if (i % 2 == 0)
        {
            w.add<Vel>(e, Vel{i});
        }
        if (i % 4 == 0)
        {
            w.add<TagB>(e);
        }
        if (i % 2 == 0 && i % 4 != 0)
        {
            expected.push_back(e);
        }
    }

    const ecs::pool_ref by_type = w.find_pool<Pos>();
    EXPECT_TRUE(static_cast<bool>(by_type));
    EXPECT_EQ(by_type.name(), ecs::name_of<Pos>());
    EXPECT_EQ(w.find_pool(by_type.info().id).name(), by_type.name());
    EXPECT_EQ(w.find_pool_by_hash(ecs::hash_of<Pos>()).name(), by_type.name());
    EXPECT_FALSE(w.find_pool_by_hash(0xDEAD));
    EXPECT_EQ(by_type.size(), 10);

    ecs::runtime_selection q;
    q.include(w.find_pool<Pos>()).include(w.find_pool<Vel>()).exclude(w.find_pool<TagB>());
    std::vector<ecs::entity> from_runtime;
    q.entities([&](ecs::entity e) { from_runtime.push_back(e); });
    EXPECT_EQ(from_runtime, expected);
    EXPECT_EQ(q.count(), expected.size());
    EXPECT_TRUE(q.contains(expected.front()));

    std::vector<ecs::entity> from_typed;
    w.view<Pos, Vel>(!ecs::exists<TagB>{})
        .entities([&](ecs::entity e) { from_typed.push_back(e); });
    EXPECT_EQ(from_runtime, from_typed);

    ecs::runtime_selection impossible;
    impossible.include(w.find_pool_by_hash(0xDEAD));
    impossible.include(w.find_pool<Pos>());
    EXPECT_EQ(impossible.count(), 0);

    ecs::runtime_selection lax;
    lax.include(w.find_pool<Pos>()).exclude(ecs::pool_ref{});
    EXPECT_EQ(lax.count(), 10);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, ChunkCapacityAndFootprint)
{
    static_assert(ecs::chunk_capacity<TunedStable> == 8);
    static_assert(ecs::chunk_capacity<Stable> == 1024);

    ecs::registry w;
    w.add<TunedStable>(w.create(), TunedStable{1});
    bool saw = false;
    w.each_pool(
        [&](const ecs::pool_info& info)
        {
            if (info.name == ecs::name_of<TunedStable>())
            {
                saw = true;
                EXPECT_EQ(info.capacity, 8);
                EXPECT_GT(info.index_bytes, 0);
                EXPECT_GT(info.bookkeeping_bytes, 0);
            }
        });
    EXPECT_TRUE(saw);

    const ecs::memory_footprint f = w.footprint();
    EXPECT_GT(f.entity_table_bytes, 0);
    EXPECT_GE(f.component_bytes, 8 * sizeof(TunedStable));
    EXPECT_GT(f.index_bytes, 0);
    EXPECT_EQ(f.total(),
              f.entity_table_bytes + f.component_bytes + f.index_bytes + f.bookkeeping_bytes);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, PmrSupport)
{
    struct counting_resource : std::pmr::memory_resource
    {
        std::size_t allocations = 0;
        std::size_t live_bytes = 0;

        void* do_allocate(std::size_t bytes, std::size_t align) override
        {
            ++allocations;
            live_bytes += bytes;
            return std::pmr::get_default_resource()->allocate(bytes, align);
        }

        void do_deallocate(void* p, std::size_t bytes, std::size_t align) override
        {
            live_bytes -= bytes;
            std::pmr::get_default_resource()->deallocate(p, bytes, align);
        }

        [[nodiscard]] bool do_is_equal(
            const std::pmr::memory_resource& other) const noexcept override
        {
            return this == &other;
        }
    };

    counting_resource arena;
    {
        ecs::registry w(&arena);
        ecs::command_buffer cmd(&arena);
        ecs::blueprint bp(&arena);
        bp.add<Pos>(1);
        bp.add<Stable>(Stable{2});
        for (int i = 0; i < 200; ++i)
        {
            const ecs::entity e = w.create(bp);
            w.add<TagA>(e);
            cmd.add<Vel>(e, Vel{i});
        }
        w.apply(cmd);
        for (int i = 0; i < 100; ++i)
        {
            w.destroy(w.view<Pos>().first());
        }
        w.shrink();
        EXPECT_EQ(w.view<Pos>().count(), 100);
        EXPECT_GT(arena.allocations, 0);
        EXPECT_TRUE(RegistryValid(w));
    }
    EXPECT_EQ(arena.live_bytes, 0);
}

TEST(Core, SortNonInvolution)
{
    ecs::registry w;
    w.create(Pos{30});
    w.create(Pos{10});
    w.create(Pos{20});
    w.sort<Pos>([](const Pos& a, const Pos& b) { return a.x < b.x; });
    std::vector<int> order;
    w.each<Pos>([&](const Pos& p) { order.push_back(p.x); });
    EXPECT_EQ(order, (std::vector<int>{10, 20, 30}));
    EXPECT_TRUE(RegistryValid(w));

    ecs::registry big;
    std::uint32_t state = 0x9E3779B9u;
    for (int i = 0; i < 64; ++i)
    {
        state = (state * 1664525u) + 1013904223u;
        const int v = static_cast<int>(state % 1000);
        const ecs::entity e = big.create(Pos{v});
        big.add<Stable>(e, Stable{v});
    }
    big.sort<Pos>([](const Pos& a, const Pos& b) { return a.x < b.x; });
    big.sort<Stable>([](const Stable& a, const Stable& b) { return a.value > b.value; });
    int prev = -1;
    bool ascending = true;
    big.each<Pos>([&](const Pos& p) { ascending = ascending && p.x >= std::exchange(prev, p.x); });
    EXPECT_TRUE(ascending);
    prev = 1001;
    bool descending = true;
    big.each<Stable>([&](const Stable& s)
                     { descending = descending && s.value <= std::exchange(prev, s.value); });
    EXPECT_TRUE(descending);
    EXPECT_TRUE(RegistryValid(big));
}

TEST(Core, HookEdgeCases)
{
    struct LateA
    {
        int v = 0;
    };
    struct LateB
    {
        int v = 0;
    };
    {
        ecs::registry w;
        ecs::entity grave = w.create();
        const ecs::hook_token t =
            w.on_remove<Pos>([](ecs::registry& world, ecs::entity, void* user)
                             { world.put<LateA>(*static_cast<ecs::entity*>(user), LateA{1}); },
                             &grave);
        const ecs::entity victim = w.create(Pos{1});
        w.destroy(victim);
        EXPECT_TRUE(w.has<LateA>(grave));
        EXPECT_TRUE(RegistryValid(w));
        EXPECT_TRUE(w.unhook(t));

        const ecs::hook_token t2 =
            w.on_remove<Pos>([](ecs::registry& world, ecs::entity, void*)
                             { static_cast<void>(world.find_pool<LateB>()); },
                             nullptr);
        w.create(Pos{2});
        w.reset();
        EXPECT_TRUE(RegistryValid(w));
        EXPECT_TRUE(w.unhook(t2));
    }

#if ECS_CHECKS
    {
        violation_scope guard;
        ecs::registry w;
        const ecs::hook_token t = w.on_remove<Pos>(
            [](ecs::registry& world, ecs::entity e, void*) { world.add<TagA>(e); }, nullptr);
        const ecs::entity victim = w.create(Pos{1});
        w.destroy(victim);
        EXPECT_EQ(violations_seen, 1);
        EXPECT_TRUE(RegistryValid(w));
        const ecs::entity recycled = w.create();
        EXPECT_EQ(recycled.index(), victim.index());
        EXPECT_FALSE(w.has<TagA>(recycled));
        EXPECT_TRUE(w.unhook(t));
    }
#endif

    {
        ecs::registry w;
        const ecs::hook_token t = w.on_add<Pos>(
            [](ecs::registry& world, ecs::entity e, void*)
            {
                if (!world.has<Vel>(e))
                {
                    world.add<Vel>(e, Vel{99});
                }
            },
            nullptr);
        const ecs::entity src = w.create();
        w.add<Pos>(src, Pos{1});
        w.replace<Vel>(src, Vel{5});
        const ecs::duplicate_result r = w.duplicate(src);
        EXPECT_TRUE(w.alive(r.clone));
        EXPECT_EQ(w.get<Vel>(r.clone).v, 99);
        EXPECT_TRUE(RegistryValid(w));
        EXPECT_TRUE(w.unhook(t));
    }

    {
        ecs::registry w;
        ecs::entity last;
        for (int i = 0; i < 8; ++i)
        {
            const ecs::entity e = w.create();
            w.add<Label>(e, Label{std::string(64, static_cast<char>('a' + i))});
            last = e;
        }
        const ecs::duplicate_result r = w.duplicate(last);
        EXPECT_EQ(w.get<Label>(r.clone).value, w.get<Label>(last).value);
        EXPECT_TRUE(RegistryValid(w));
    }

#if ECS_CHECKS
    {
        violation_scope guard;
        ecs::registry w;
        const ecs::hook_token t = w.on_add<Pos>([](ecs::registry&, ecs::entity, void*) {}, nullptr);
        w.create(Pos{1});
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                EXPECT_FALSE(w.unhook(t));
                return false;
            });
        EXPECT_EQ(violations_seen, 1);
        EXPECT_TRUE(w.unhook(t));
    }
#endif
}

TEST(Core, ArenaWarmOveraligned)
{
    struct counting_resource : std::pmr::memory_resource
    {
        std::size_t allocations = 0;

        void* do_allocate(std::size_t bytes, std::size_t align) override
        {
            ++allocations;
            return std::pmr::get_default_resource()->allocate(bytes, align);
        }

        void do_deallocate(void* p, std::size_t bytes, std::size_t align) override
        {
            std::pmr::get_default_resource()->deallocate(p, bytes, align);
        }

        [[nodiscard]] bool do_is_equal(
            const std::pmr::memory_resource& other) const noexcept override
        {
            return this == &other;
        }
    };

    counting_resource arena;
    ecs::registry w;
    ecs::command_buffer cmd(&arena);
    const ecs::entity e = w.create();
    cmd.add<Big128>(e, Big128{1.0f});
    w.apply(cmd);
    w.remove<Big128>(e);
    const std::size_t after_warmup = arena.allocations;
    for (int frame = 0; frame < 16; ++frame)
    {
        cmd.add<Big128>(e, Big128{2.0f});
        w.apply(cmd);
        w.remove<Big128>(e);
    }
    EXPECT_EQ(arena.allocations, after_warmup);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, RuntimeSelectionSelfMutation)
{
    ecs::registry w;
    for (int i = 0; i < 4; ++i)
    {
        w.create(Pos{i});
    }
    w.add<Vel>(w.view<Pos>().first(), Vel{1});

    ecs::runtime_selection q;
    q.include(w.find_pool<Pos>());
    std::size_t visited = 0;
    q.entities(
        [&](ecs::entity)
        {
            if (visited++ == 0)
            {
                q.include(w.find_pool<Vel>());
            }
            return true;
        });
    EXPECT_EQ(visited, 4);
    EXPECT_EQ(q.count(), 1);
#if ECS_CHECKS
    EXPECT_EQ(ecs::test_access::lock_count<Pos>(w), 0);
    EXPECT_EQ(ecs::test_access::lock_count<Vel>(w), 0);
    {
        violation_scope guard;
        w.add<Pos>(w.create(), Pos{9});
        EXPECT_EQ(violations_seen, 0);
    }
#endif
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, EmptyEntityRef)
{
    ecs::entity_filler empty;
    EXPECT_FALSE(empty.alive());
    EXPECT_FALSE(static_cast<bool>(empty));
    EXPECT_FALSE(empty.has<Pos>());
    EXPECT_EQ(empty.find<Pos>(), nullptr);
    EXPECT_FALSE(empty.remove<Pos>());
    empty.destroy();
    empty.orphan();
    EXPECT_FALSE(empty.parent().alive());

    ecs::const_entity_filler cempty;
    EXPECT_FALSE(cempty.alive());
    EXPECT_FALSE(cempty.has<Pos>());
    EXPECT_EQ(cempty.find<Pos>(), nullptr);
    EXPECT_FALSE(cempty.parent().alive());
}

TEST(Core, HookConnectionForms)
{
    ecs::registry w;

    hookfree::free_hits = 0;
    const ecs::hook_token t1 = w.on_add<Pos, &hookfree::on_free_fn>();
    const ecs::hook_token t2 = w.on_add<Pos, &hookfree::on_free_short>();
    w.create(Pos{1});
    EXPECT_EQ(hookfree::free_hits, 2);
    EXPECT_TRUE(w.unhook(t1));
    EXPECT_TRUE(w.unhook(t2));

    GpuUploads gpu;
    const ecs::hook_token t3 = w.on_add<Tint, &GpuUploads::upload>(&gpu);
    const ecs::hook_token t4 = w.on_remove<Tint, &GpuUploads::free_one>(&gpu);
    const ecs::entity e = w.create();
    w.add<Tint>(e, Tint{3});
    w.remove<Tint>(e);
    EXPECT_EQ(gpu.uploaded, 1);
    EXPECT_EQ(gpu.freed, 1);
    EXPECT_TRUE(w.unhook(t3));
    EXPECT_TRUE(w.unhook(t4));

    {
        ecs::scoped_hook scoped(w, w.on_add<Tint, &GpuUploads::upload>(&gpu));
        EXPECT_TRUE(static_cast<bool>(scoped));
        EXPECT_TRUE(static_cast<bool>(scoped.token()));
        w.add<Tint>(e, Tint{4});
        EXPECT_EQ(gpu.uploaded, 2);
        ecs::scoped_hook moved(std::move(scoped));
        EXPECT_FALSE(static_cast<bool>(scoped));
        EXPECT_TRUE(static_cast<bool>(moved));
        moved.release();
        moved.release();
        EXPECT_FALSE(static_cast<bool>(moved));
    }
    w.replace<Tint>(e, Tint{5});
    w.remove<Tint>(e);
    EXPECT_EQ(gpu.uploaded, 2);

#if ECS_CHECKS
    {
        violation_scope guard;
        auto* leak = new ecs::scoped_hook(w, w.on_add<Pos, &hookfree::on_free_fn>());
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                delete leak;
                return false;
            });
        EXPECT_EQ(violations_seen, 1);
        hookfree::free_hits = 0;
        w.create(Pos{2});
        EXPECT_EQ(hookfree::free_hits, 1);
    }
#endif
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, Split)
{
    ecs::registry w;
    std::vector<ecs::entity> expected;
    for (int i = 0; i < 1000; ++i)
    {
        const ecs::entity e = w.create(Pos{i});
        if (i % 2 == 0)
        {
            w.add<Vel>(e, Vel{i});
        }
        if (i % 10 == 0)
        {
            w.add<TagB>(e);
        }
        if (i % 2 == 0 && i % 10 != 0)
        {
            expected.push_back(e);
        }
    }
    const auto sel = w.view<Pos, Vel>(!ecs::exists<TagB>{});

    for (const std::size_t n : {std::size_t{1}, std::size_t{3}, std::size_t{4}, std::size_t{7}})
    {
        std::unordered_set<ecs::entity> seen;
        std::size_t visits = 0;
        {
            const auto work = sel.split(n);
            EXPECT_EQ(work.parts(), n);
            for (std::size_t i = 0; i < work.parts(); ++i)
            {
                work.part(i).entities(
                    [&](ecs::entity e)
                    {
                        seen.insert(e);
                        ++visits;
                    });
            }
        }
        EXPECT_EQ(visits, expected.size());
        EXPECT_EQ(seen.size(), expected.size());
    }

    {
        const auto tiny = w.view<TagB>();
        const auto work = tiny.split(10000);
        std::size_t visits = 0;
        for (std::size_t i = 0; i < work.parts(); ++i)
        {
            work.part(i).entities([&](ecs::entity) { ++visits; });
        }
        EXPECT_EQ(visits, tiny.count());
        const auto zero = sel.split(0);
        EXPECT_EQ(zero.parts(), 1);
        const auto none = w.view<EmptyPacked>().split(4);
        none.part(0).entities([](ecs::entity) { EXPECT_TRUE(false); });
        none.part(9).entities([](ecs::entity) { EXPECT_TRUE(false); });
    }

    {
        const auto msel = w.view<Pos, ecs::maybe<Vel>>();
        const auto work = msel.split(3);
        std::size_t with = 0;
        std::size_t without = 0;
        for (std::size_t i = 0; i < work.parts(); ++i)
        {
            work.part(i).each([&](Pos&, Vel* v) { (v != nullptr ? with : without) += 1; });
        }
        EXPECT_EQ(with, 500);
        EXPECT_EQ(without, 500);
    }

#if ECS_CHECKS
    {
        violation_scope guard;
        const auto work = sel.split(2);
        w.add<Pos>(w.create(), Pos{1});
        EXPECT_EQ(violations_seen, 1);
    }
#endif

    {
        const auto par = w.view<Pos, const Vel>();
        const auto work = par.split(4);
        std::vector<std::thread> workers;
        workers.reserve(work.parts());
        for (std::size_t i = 0; i < work.parts(); ++i)
        {
            workers.emplace_back([part = work.part(i)]
                                 { part.each([](Pos& p, const Vel& v) { p.x += v.v; }); });
        }
        for (std::thread& t : workers)
        {
            t.join();
        }
    }
    EXPECT_TRUE(RegistryValid(w));

    {
        const auto work = sel.split(2);
        std::size_t first_visits = 0;
        work.part(0).entities(
            [&](ecs::entity)
            {
                ++first_visits;
                return false;
            });
        EXPECT_EQ(first_visits, 1);
        std::size_t second_visits = 0;
        work.part(1).entities([&](ecs::entity) { ++second_visits; });
        EXPECT_GT(second_visits, 0);
    }
}

TEST(Core, Archives)
{
    byte_writer out;
    std::vector<ecs::entity> survivors;
    {
        ecs::registry w;
        for (int i = 0; i < 10; ++i)
        {
            const ecs::entity e = w.create(Pos{i});
            if (i % 2 == 0)
            {
                w.add<Stable>(e, Stable{i * 10});
            }
            if (i % 3 == 0)
            {
                w.add<TagA>(e);
            }
        }
        w.each([&](ecs::entity e) { survivors.push_back(e); });
        w.destroy(survivors[2]);
        w.destroy(survivors[5]);
        w.create(Pos{100});
        survivors.clear();
        w.each([&](ecs::entity e) { survivors.push_back(e); });
        w.sort<Pos>([](const Pos& a, const Pos& b) { return a.x > b.x; });
        std::vector<int> order_before;
        w.each<const Pos>([&](const Pos& p) { order_before.push_back(p.x); });

        ecs::pack<Pos, Stable, TagA>(w, out);

        ecs::registry fresh;
        byte_reader in{&out.data};
        const auto r = ecs::unpack<Pos, Stable, TagA>(fresh, in);
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(fresh.live_count(), w.live_count());
        for (const ecs::entity e : survivors)
        {
            EXPECT_TRUE(fresh.alive(e));
            EXPECT_EQ(fresh.has<Pos>(e), w.has<Pos>(e));
            EXPECT_EQ(fresh.has<Stable>(e), w.has<Stable>(e));
            EXPECT_EQ(fresh.has<TagA>(e), w.has<TagA>(e));
            if (const Pos* p = fresh.find<Pos>(e))
            {
                EXPECT_EQ(p->x, w.get<Pos>(e).x);
            }
        }
        std::vector<int> order_after;
        fresh.each<const Pos>([&](const Pos& p) { order_after.push_back(p.x); });
        EXPECT_EQ(order_before, order_after);
        EXPECT_TRUE(RegistryValid(fresh));
    }

#if ECS_CHECKS
    {
        violation_scope guard;
        ecs::registry busy;
        busy.create(Pos{1});
        byte_reader in{&out.data};
        const auto r = ecs::unpack<Pos, Stable, TagA>(busy, in);
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code, ecs::fault_code::world_not_empty);
        EXPECT_EQ(violations_seen, 1);
    }
#endif

    {
        ecs::registry fresh;
        byte_reader in{&out.data};
        const auto r = ecs::unpack<Vel, Stable, TagA>(fresh, in);
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code, ecs::fault_code::archive_mismatch);
    }

    {
        ecs::registry w;
        const ecs::entity leader = w.create(Pos{1});
        const ecs::entity grunt = w.create(Pos{2});
        w.add<Squad>(grunt, Squad{leader, 5});
        w.add<Squad>(leader, Squad{leader, 9});
        w.add<ForeignLink>(grunt, ForeignLink{leader});
        byte_writer save;
        ecs::pack<Pos, Squad, ForeignLink>(w, save);

        ecs::registry host;
        host.create(Pos{777});
        byte_reader in{&save.data};
        const auto grafted = ecs::graft<Pos, Squad, ForeignLink>(host, in);
        EXPECT_TRUE(grafted.has_value());
        const ecs::graft_map& map = *grafted;
        EXPECT_EQ(map.size(), 2);
        const ecs::entity new_leader = map.resolve(leader);
        const ecs::entity new_grunt = map.resolve(grunt);
        EXPECT_TRUE(host.alive(new_leader) && host.alive(new_grunt));
        EXPECT_TRUE(new_leader != leader || new_grunt != grunt);
        EXPECT_EQ(host.get<Squad>(new_grunt).leader, new_leader);
        EXPECT_EQ(host.get<Squad>(new_leader).leader, new_leader);
        EXPECT_EQ(host.get<ForeignLink>(new_grunt).target, new_leader);
        EXPECT_EQ(host.get<Squad>(new_grunt).morale, 5);
        EXPECT_EQ(map.resolve(ecs::no_entity), ecs::no_entity);
        std::size_t mapped = 0;
        map.each(
            [&](ecs::entity, ecs::entity fresh_e)
            {
                EXPECT_TRUE(host.alive(fresh_e));
                ++mapped;
            });
        EXPECT_EQ(mapped, 2);
        EXPECT_TRUE(RegistryValid(host));
    }

    {
        ecs::registry w;
        const ecs::entity outsider = w.create();
        const ecs::entity insider = w.create(Pos{1});
        w.add<Squad>(insider, Squad{outsider, 1});
        w.destroy(outsider);
        byte_writer save;
        ecs::pack<Pos, Squad>(w, save);

        ecs::registry host;
        byte_reader in{&save.data};
        const auto grafted = ecs::graft<Pos, Squad>(host, in);
        EXPECT_TRUE(grafted.has_value());
        EXPECT_EQ(host.get<Squad>(grafted->resolve(insider)).leader, ecs::no_entity);
    }

    {
        ecs::registry w;
        w.create(Pos{1}, Vel{1});
        w.create(Pos{2});
        byte_writer save;
        ecs::pack<Pos, Vel>(w, save);

        ecs::registry fresh;
        auto view = fresh.view<Pos, Vel>();
        byte_reader in{&save.data};
        EXPECT_TRUE((ecs::unpack<Pos, Vel>(fresh, in).has_value()));
        EXPECT_EQ(view.count(), 1);
        EXPECT_TRUE(RegistryValid(fresh));
    }
}

TEST(Core, ArchiveSchemaFingerprint)
{
    EXPECT_NE(ecs::detail::schema_hash<Pos>(), ecs::hash_of<Pos>());

    byte_writer out;
    {
        ecs::registry w;
        ecs::pack<Pos>(w, out);
    }
    ASSERT_GE(out.data.size(), 24U);
    out.data[16] = static_cast<std::byte>(std::to_integer<unsigned>(out.data[16]) ^ 0xFFU);

    ecs::registry fresh;
    byte_reader in{&out.data};
    const auto r = ecs::unpack<Pos>(fresh, in);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ecs::fault_code::archive_mismatch);
}

TEST(Core, ArchiveLoadCap)
{
    byte_writer out;
    {
        ecs::registry w;
        for (int i = 0; i < 5; ++i)
        {
            w.create(Pos{i});
        }
        ecs::pack<Pos>(w, out);
    }

    {
        ecs::registry fresh;
        byte_reader in{&out.data};
        const auto r = ecs::unpack<Pos>(fresh, in, 1000);
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(fresh.live_count(), 5U);
    }
    {
        ecs::registry fresh;
        byte_reader in{&out.data};
        const auto r = ecs::unpack<Pos>(fresh, in, 2);
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code, ecs::fault_code::archive_too_large);
    }
}

TEST(Core, ArchiveGraftCap)
{
    byte_writer out;
    {
        ecs::registry w;
        for (int i = 0; i < 5; ++i)
        {
            w.create(Pos{i});
        }
        ecs::pack<Pos>(w, out);
    }

    {
        ecs::registry host;
        byte_reader in{&out.data};
        const auto m = ecs::graft<Pos>(host, in, std::pmr::get_default_resource(), 1000);
        ASSERT_TRUE(m.has_value());
        EXPECT_FALSE(m->empty());
        EXPECT_EQ(host.live_count(), 5U);
    }
    {
        ecs::registry host;
        byte_reader in{&out.data};
        const auto m = ecs::graft<Pos>(host, in, std::pmr::get_default_resource(), 2);
        ASSERT_FALSE(m.has_value());
        EXPECT_EQ(m.error().code, ecs::fault_code::archive_too_large);
        EXPECT_EQ(host.live_count(), 0U);
    }
}

TEST(Core, DuplicateSubtree)
{
    ecs::registry w;
    const ecs::entity root = w.create(Pos{1});
    const ecs::entity a = w.create(Pos{2});
    const ecs::entity b = w.create(Pos{3});
    const ecs::entity a1 = w.create(Pos{4});
    const ecs::entity a2 = w.create(Pos{5});
    w.adopt(root, a);
    w.adopt(root, b);
    w.adopt(a, a1);
    w.adopt(a, a2);

    const ecs::entity clone = w.duplicate_subtree(root);
    ASSERT_NE(clone, ecs::no_entity);
    EXPECT_NE(clone, root);
    EXPECT_EQ(w.parent_of(clone), ecs::no_entity);

    const auto walk = [&](ecs::entity start)
    {
        std::vector<int> vals;
        vals.push_back(w.get<Pos>(start).x);
        w.descendants_of(start, [&](ecs::entity e) { vals.push_back(w.get<Pos>(e).x); });
        return vals;
    };
    EXPECT_EQ(walk(clone), walk(root));

    EXPECT_TRUE(w.alive(a1));
    EXPECT_EQ(w.parent_of(a), root);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, LinkArchiveRoundTrip)
{
    ecs::registry w;
    const ecs::entity root = w.create(Pos{1});
    const ecs::entity a = w.create(Pos{2});
    const ecs::entity b = w.create(Pos{3});
    const ecs::entity a1 = w.create(Pos{4});
    w.adopt(root, a);
    w.adopt(root, b);
    w.adopt(a, a1);

    byte_writer out;
    ecs::pack<Pos>(w, out);
    ecs::pack_links(w, out);

    ecs::registry fresh;
    byte_reader in{&out.data};
    ASSERT_TRUE(ecs::unpack<Pos>(fresh, in).has_value());
    ASSERT_TRUE(ecs::unpack_links(fresh, in).has_value());

    EXPECT_EQ(fresh.parent_of(a), root);
    EXPECT_EQ(fresh.parent_of(a1), a);
    EXPECT_EQ(fresh.parent_of(root), ecs::no_entity);

    const auto kids = [](ecs::registry& reg, ecs::entity p)
    {
        std::vector<ecs::entity> v;
        reg.children_of(p, [&](ecs::entity c) { v.push_back(c); });
        return v;
    };
    EXPECT_EQ(kids(fresh, root), kids(w, root));
    EXPECT_TRUE(RegistryValid(fresh));
}

TEST(Core, LinkGraft)
{
    ecs::registry src;
    const ecs::entity root = src.create(Pos{1});
    const ecs::entity a = src.create(Pos{2});
    const ecs::entity a1 = src.create(Pos{3});
    src.adopt(root, a);
    src.adopt(a, a1);

    byte_writer out;
    ecs::pack<Pos>(src, out);
    ecs::pack_links(src, out);

    ecs::registry dst;
    dst.create(Pos{99});
    byte_reader in{&out.data};
    const auto m = ecs::graft<Pos>(dst, in);
    ASSERT_TRUE(m.has_value());
    ASSERT_TRUE(ecs::graft_links(dst, in, *m).has_value());

    const ecs::entity groot = m->resolve(root);
    const ecs::entity ga = m->resolve(a);
    const ecs::entity ga1 = m->resolve(a1);
    ASSERT_NE(groot, ecs::no_entity);
    EXPECT_EQ(dst.parent_of(ga), groot);
    EXPECT_EQ(dst.parent_of(ga1), ga);
    EXPECT_EQ(dst.parent_of(groot), ecs::no_entity);
    EXPECT_TRUE(RegistryValid(dst));
}

TEST(Core, Globals)
{
    ecs::registry w;
    const std::size_t before = w.live_count();

    ecs::entity_filler g = w.globals();
    EXPECT_TRUE(g.alive());
    EXPECT_EQ(w.live_count(), before + 1);
    g.obtain<Tint>(Tint{1}).color = 7;
    EXPECT_EQ(w.globals().get<Tint>().color, 7);
    EXPECT_EQ(w.globals().id(), g.id());

#if ECS_CHECKS
    {
        violation_scope guard;
        w.destroy(g.id());
        EXPECT_EQ(violations_seen, 1);
        EXPECT_TRUE(g.alive());
        const ecs::duplicate_result dup = w.duplicate(g.id());
        EXPECT_EQ(violations_seen, 2);
        EXPECT_EQ(dup.clone, ecs::no_entity);
    }
#endif

    {
        ecs::command_buffer cmd;
        cmd.destroy(g.id());
        violation_scope guard;
        w.apply(cmd);
        EXPECT_TRUE(g.alive());
    }

    const ecs::entity old_id = g.id();
    w.reset();
    EXPECT_FALSE(w.alive(old_id));
    ecs::entity_filler fresh = w.globals();
    EXPECT_TRUE(fresh.alive());
    EXPECT_FALSE(fresh.has<Tint>());

    fresh.obtain<Tint>(Tint{42});
    byte_writer save;
    ecs::pack<ecs::globals_mark, Tint>(w, save);
    ecs::registry loaded;
    byte_reader in{&save.data};
    EXPECT_TRUE((ecs::unpack<ecs::globals_mark, Tint>(loaded, in).has_value()));
    EXPECT_EQ(loaded.globals().get<Tint>().color, 42);
    EXPECT_EQ(loaded.live_count(), w.live_count());

    {
        const ecs::registry quiet;
        ecs::const_entity_filler cg = quiet.globals();
        EXPECT_FALSE(cg.alive());
        EXPECT_EQ(quiet.live_count(), 0);
    }

#if ECS_CHECKS
    {
        ecs::registry broken;
        static_cast<void>(broken.globals());
        broken.add<ecs::globals_mark>(broken.create());
        const auto r = broken.validate();
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code, ecs::fault_code::globals_broken);
    }
#endif
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, BracedInitMutators)
{
    ecs::registry w;
    const ecs::entity e = w.create();

    w.add<Pos>(e, {2});
    EXPECT_EQ(w.get<Pos>(e).x, 2);
    w.put<Pos>(e, {5});
    EXPECT_EQ(w.get<Pos>(e).x, 5);
    w.put<Vel>(e, {3});
    EXPECT_EQ(w.get<Vel>(e).v, 3);
    w.replace<Pos>(e, {7});
    EXPECT_EQ(w.get<Pos>(e).x, 7);
    EXPECT_EQ(w.obtain<Hp>(e, {40}).hp, 40);
    EXPECT_EQ(w.obtain<Hp>(e, {99}).hp, 40);

    const ecs::entity f = w.create().component<Pos>({1}).component<Hp>({10});
    EXPECT_EQ(w.get<Pos>(f).x, 1);
    EXPECT_EQ(w.get<Hp>(f).hp, 10);

    w.ref(f).add<Vel>({4});
    w.ref(f).put<Pos>({8});
    w.ref(f).replace<Hp>({20});
    EXPECT_EQ(w.get<Vel>(f).v, 4);
    EXPECT_EQ(w.get<Pos>(f).x, 8);
    EXPECT_EQ(w.get<Hp>(f).hp, 20);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, EntityFillerComponentChain)
{
    ecs::registry w;

    const ecs::entity e = w.create().component<Pos>(Pos{2}).component<Hp>(Hp{50});
    EXPECT_TRUE((w.has_all<Pos, Hp>(e)));
    EXPECT_EQ(w.get<Pos>(e).x, 2);
    EXPECT_EQ(w.get<Hp>(e).hp, 50);

    const ecs::entity f = w.create();
    w.ref(f).component<Pos>(Pos{7}).component<Vel>(Vel{1});
    EXPECT_TRUE((w.has_all<Pos, Vel>(f)));
    EXPECT_EQ(w.get<Pos>(f).x, 7);
    EXPECT_EQ(w.get<Vel>(f).v, 1);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, RefConversions)
{
    ecs::registry w;
    const ecs::entity e = w.create(Pos{3});
    ecs::entity_filler mut = w.ref(e);
    ecs::const_entity_filler view = mut;
    EXPECT_TRUE(view.alive());
    EXPECT_EQ(view.get<Pos>().x, 3);
    static_assert(std::is_same_v<decltype(view.get<Pos>()), const Pos&>);
    static_assert(std::is_same_v<decltype(mut.get<Pos>()), Pos&>);
    static_assert(std::is_same_v<decltype(view.find<Pos>()), const Pos*>);
    static_assert(std::is_same_v<decltype(mut.find<Pos>()), Pos*>);
    mut.get<Pos>().x = 4;
    EXPECT_EQ(view.get<Pos>().x, 4);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, ReviewRegressions)
{
    {
        ecs::registry w;
        ecs::command_buffer cmd;
        struct ctx_t
        {
            ecs::command_buffer* cmd = nullptr;
            int chained = 0;
        } ctx{&cmd, 0};
        const ecs::hook_token t = w.on_add<Pos>(
            [](ecs::registry&, ecs::entity e, void* user)
            {
                auto* c = static_cast<ctx_t*>(user);
                if (c->chained < 8)
                {
                    ++c->chained;
                    c->cmd->add<Vel>(e, Vel{c->chained});
                }
            },
            &ctx);
        std::vector<ecs::entity> spawned;
        for (int i = 0; i < 8; ++i)
        {
            const ecs::entity e = w.create();
            spawned.push_back(e);
            cmd.add<Pos>(e, Pos{i});
        }
        const ecs::apply_result r = w.apply(cmd);
        EXPECT_EQ(r.applied, 16);
        for (const ecs::entity e : spawned)
        {
            EXPECT_TRUE(w.has<Vel>(e));
        }
        EXPECT_TRUE(cmd.empty());
        EXPECT_TRUE(w.unhook(t));
        EXPECT_TRUE(RegistryValid(w));
    }

    {
        ecs::registry w;
        ecs::command_buffer cmd;
        const ecs::hook_token t = w.on_add<Hp>(
            [](ecs::registry& world, ecs::entity, void* user)
            {
                auto* c = static_cast<ecs::command_buffer*>(user);
                if (world.view<TagA>().count() == 0)
                {
                    const ecs::entity child = c->create();
                    c->add<TagA>(child);
                }
            },
            &cmd);
        cmd.add<Hp>(w.create(), Hp{1});
        w.apply(cmd);
        EXPECT_EQ(w.view<TagA>().count(), 1);
        EXPECT_TRUE(w.unhook(t));
        EXPECT_TRUE(RegistryValid(w));
    }

    {
        ecs::registry w;
        auto view = w.view<Pos, Vel>();
        w.create(Pos{1}, Vel{1});
        w.create(Pos{2}, Vel{2});
        struct probe_t
        {
            std::size_t seen = 999;
            bool valid_mid_reset = false;
        } probe;
        const ecs::hook_token t = w.on_remove<Vel>(
            [](ecs::registry& world, ecs::entity, void* user)
            {
                auto* p = static_cast<probe_t*>(user);
                p->seen = world.view<Pos, Vel>().count();
                p->valid_mid_reset = world.validate().has_value();
            },
            &probe);
        w.reset();
        EXPECT_EQ(probe.seen, 0);
        EXPECT_TRUE(probe.valid_mid_reset);
        EXPECT_EQ(view.count(), 0);
        EXPECT_TRUE(w.unhook(t));
        EXPECT_TRUE(RegistryValid(w));
    }

    {
        int fires = 0;
        ecs::registry a;
        ecs::scoped_hook guard(a,
                               a.on_add<Pos>([](ecs::registry&, ecs::entity, void* user)
                                             { ++*static_cast<int*>(user); },
                                             &fires));
        ecs::registry b = std::move(a);
        b.create(Pos{1});
        EXPECT_EQ(fires, 1);
        guard.release();
        b.create(Pos{2});
        EXPECT_EQ(fires, 1);
    }

#if ECS_CHECKS
    {
        ecs::registry w;
        w.globals().obtain<Tint>(Tint{5});
        byte_writer save;
        ecs::pack<ecs::globals_mark, Tint>(w, save);

        ecs::registry loaded;
        byte_reader in{&save.data};
        EXPECT_TRUE((ecs::unpack<ecs::globals_mark, Tint>(loaded, in).has_value()));
        const ecs::entity restored = loaded.view<ecs::globals_mark>().first();
        violation_scope guard;
        loaded.destroy(restored);
        EXPECT_EQ(violations_seen, 1);
        EXPECT_TRUE(loaded.alive(restored));
        const ecs::duplicate_result dup = loaded.duplicate(restored);
        EXPECT_EQ(violations_seen, 2);
        EXPECT_EQ(dup.clone, ecs::no_entity);
        EXPECT_TRUE(RegistryValid(loaded));
    }

    {
        violation_scope guard;
        ecs::registry w;
        ecs::entity carrier;
        {
            const auto marks = w.view<ecs::globals_mark>();
            const auto lock = marks.each();
            carrier = w.globals().id();
            EXPECT_EQ(violations_seen, 1);
            EXPECT_FALSE(w.has<ecs::globals_mark>(carrier));
        }
        const ecs::entity healed = w.globals().id();
        EXPECT_EQ(healed, carrier);
        EXPECT_TRUE(w.has<ecs::globals_mark>(carrier));
        EXPECT_TRUE(RegistryValid(w));
    }
#endif

    {
        ecs::registry w;
        auto view = w.view<Pos, const Vel>();
        w.create(Pos{1}, Vel{2});
        int sum = 0;
        view.each([&](Pos& p, const Vel& v) { sum = p.x + v.v; });
        EXPECT_EQ(sum, 3);
        EXPECT_EQ((w.view<const Pos, const Vel>().count()), 1);
        EXPECT_TRUE(RegistryValid(w));
    }
}

TEST(Core, GenericitySeams)
{
    static_assert(std::derived_from<ecs::pool_of_t<Telemetry>, ecs::packed_pool_of<Telemetry>>);

    {
        static_assert(std::same_as<ecs::pool_of_t<Telemetry>, telemetry_pool>);
        telemetry_pool::erases = 0;
        telemetry_pool::wipes = 0;
        ecs::registry w;
        int hook_fires = 0;
        const ecs::hook_token t = w.on_add<Telemetry>([](ecs::registry&, ecs::entity, void* user)
                                                      { ++*static_cast<int*>(user); },
                                                      &hook_fires);
        for (int i = 0; i < 6; ++i)
        {
            const ecs::entity e = w.create(Telemetry{i});
            if (i % 2 == 0)
            {
                w.add<TagA>(e);
            }
        }
        EXPECT_EQ(hook_fires, 6);

        int sum = 0;
        w.each<const Telemetry>([&](const Telemetry& tl) { sum += tl.v; });
        EXPECT_EQ(sum, 15);

        EXPECT_EQ((w.view<Telemetry, TagA>().count()), 3);

        w.sort<Telemetry>([](const Telemetry& a, const Telemetry& b) { return a.v > b.v; });
        int first_v = -1;
        w.view<const Telemetry>().each(
            [&](const Telemetry& tl)
            {
                first_v = tl.v;
                return false;
            });
        EXPECT_EQ(first_v, 5);

        byte_writer save;
        ecs::pack<Telemetry>(w, save);
        ecs::registry fresh;
        byte_reader in{&save.data};
        EXPECT_TRUE((ecs::unpack<Telemetry>(fresh, in).has_value()));
        EXPECT_EQ(fresh.view<const Telemetry>().count(), 6);

        w.remove<Telemetry>(w.view<const Telemetry>().first());
        EXPECT_GE(telemetry_pool::erases, 1);
        w.reset();
        EXPECT_GE(telemetry_pool::wipes, 1);
        EXPECT_TRUE(RegistryValid(w));
        EXPECT_TRUE(w.unhook(t));
    }

    {
        audit_pool::erases = 0;
        ecs::registry w;
        const ecs::entity a = w.create();
        Audit* pinned = &w.add<Audit>(a, Audit{1});
        for (int i = 0; i < 16; ++i)
        {
            const ecs::entity e = w.create();
            w.add<Audit>(e, Audit{i});
            if (i % 2 == 0)
            {
                w.add<TagB>(e);
            }
        }
        EXPECT_EQ(pinned, w.find<Audit>(a));
        EXPECT_EQ((w.view<Audit, TagB>().count()), 8);
        EXPECT_EQ(pinned, w.find<Audit>(a));

        byte_writer save;
        ecs::pack<Audit>(w, save);
        ecs::registry fresh;
        byte_reader in{&save.data};
        EXPECT_TRUE((ecs::unpack<Audit>(fresh, in).has_value()));
        EXPECT_EQ(fresh.view<const Audit>().count(), 17);

        w.remove<Audit>(a);
        EXPECT_GE(audit_pool::erases, 1);
        EXPECT_TRUE(RegistryValid(w));
    }

    {
        static_assert(ecs::name_of<LabeledA>() == "test.labeled_a");
        static_assert(ecs::name_of<ForeignVec>() == "test.foreign_vec");
        static_assert(ecs::hash_of<LabeledA>() != ecs::hash_of<ForeignVec>());
        ecs::registry w;
        w.create(LabeledA{7});
        EXPECT_EQ(w.find_pool<LabeledA>().name(), "test.labeled_a");
        EXPECT_TRUE(static_cast<bool>(w.find_pool_by_hash(ecs::hash_of<LabeledA>())));

        byte_writer save;
        ecs::pack<LabeledA>(w, save);
        ecs::registry fresh;
        byte_reader in{&save.data};
        EXPECT_TRUE((ecs::unpack<LabeledA>(fresh, in).has_value()));
        EXPECT_EQ(fresh.get<LabeledA>(fresh.view<const LabeledA>().first()).v, 7);
    }

#if ECS_CHECKS
    {
        violation_scope guard;
        ecs::registry w;
        w.create(DupLabel1{1});
        w.create(DupLabel2{2});
        EXPECT_EQ(violations_seen, 1);
    }
#endif

    {
        ecs::registry w;
        const ecs::entity e = w.create(Pos{41});
        w.add<TagA>(e);
        w.add<Stable>(e, Stable{5});

        const ecs::pool_ref positions = w.find_pool<Pos>();
        EXPECT_EQ(positions.info().bytes_per_item, sizeof(Pos));
        void* bytes = positions.raw(e);
        EXPECT_NE(bytes, nullptr);
        int v = 0;
        std::memcpy(&v, bytes, sizeof(int));
        EXPECT_EQ(v, 41);
        const int next = 42;
        std::memcpy(bytes, &next, sizeof(int));
        EXPECT_EQ(w.get<Pos>(e).x, 42);

        EXPECT_NE(w.find_pool<Stable>().raw(e), nullptr);
        EXPECT_EQ(w.find_pool<TagA>().raw(e), nullptr);
        EXPECT_EQ(positions.raw(ecs::no_entity), nullptr);
        const ecs::entity ghost = w.create();
        w.destroy(ghost);
        EXPECT_EQ(positions.raw(ghost), nullptr);

        std::size_t walked = 0;
        for (std::size_t pos = 0; pos < positions.size(); ++pos)
        {
            EXPECT_NE(positions.raw_at(pos), nullptr);
            EXPECT_NE(positions.entity_at(pos), ecs::no_entity);
            ++walked;
        }
        EXPECT_EQ(walked, positions.size());
        EXPECT_EQ(positions.raw_at(positions.size()), nullptr);
        EXPECT_TRUE(RegistryValid(w));
    }
}

TEST(Core, CompileTimeToolkit)
{
    using L = ecs::types<Pos, Vel, TagA>;
    static_assert(L::size == 3);
    static_assert(std::same_as<ecs::type_at_t<0, L>, Pos>);
    static_assert(std::same_as<ecs::type_at_t<2, L>, TagA>);
    static_assert(ecs::contains_type<Vel, L>);
    static_assert(!ecs::contains_type<Hp, L>);
    static_assert(ecs::index_of<Vel>(L{}) == 1);
    static_assert(std::same_as<ecs::joined_t<L, ecs::types<Hp>>, ecs::types<Pos, Vel, TagA, Hp>>);
    static_assert(std::same_as<ecs::without_t<L, Vel>, ecs::types<Pos, TagA>>);
    static_assert(std::same_as<ecs::distinct_t<ecs::types<Pos, Vel, Pos, TagA, Vel>>, L>);
    static_assert(std::same_as<ecs::mapped_t<std::add_const, ecs::types<Pos, Vel>>,
                               ecs::types<const Pos, const Vel>>);
    static_assert(std::same_as<ecs::constant<3>, std::integral_constant<int, 3>>);

    static_assert(std::same_as<ecs::front_t<L>, Pos>);
    static_assert(std::same_as<ecs::back_t<L>, TagA>);
    static_assert(ecs::type_list<L> && !ecs::type_list<int>);
    static_assert(ecs::is_types_v<L> && !ecs::is_types_v<Pos>);
    static_assert(ecs::all_unique_v<L> && !ecs::all_unique_v<ecs::types<Pos, Pos>>);
    static_assert(!ecs::all_of_v<std::is_empty, L>);
    static_assert(ecs::any_of_v<std::is_empty, L>);
    static_assert(ecs::none_of_v<std::is_empty, ecs::types<Pos, Vel>>);
    static_assert(ecs::count_if_v<std::is_empty, L> == 1);
    static_assert(std::same_as<ecs::filter_t<std::is_empty, L>, ecs::types<TagA>>);
    static_assert(ecs::find_if<std::is_empty>(L{}) == 2);
    static_assert(ecs::find_if<std::is_empty>(ecs::types<Pos, Vel>{}) == 2);

    static_assert(ecs::is_subset_v<ecs::types<Vel>, L> && !ecs::is_subset_v<ecs::types<Hp>, L>);
    static_assert(ecs::is_subset_v<ecs::types<>, L>);
    static_assert(
        std::same_as<ecs::intersection_t<L, ecs::types<Vel, Hp, Pos>>, ecs::types<Pos, Vel>>);
    static_assert(std::same_as<ecs::difference_t<L, ecs::types<Vel>>, ecs::types<Pos, TagA>>);

    using V = ecs::values<1, 2U, std::size_t{7}>;
    static_assert(V::size == 3);
    static_assert(ecs::value_at<0>(V{}) == 1);
    static_assert(std::same_as<decltype(ecs::value_at<1>(V{})), unsigned>);
    static_assert(ecs::contains_value<2U, V>);
    static_assert(!ecs::contains_value<9, V>);
    static_assert(ecs::index_of_value<std::size_t{7}>(V{}) == 2);

    {
        std::vector<std::string_view> names;
        ecs::for_each(L{}, [&]<class T>() { names.push_back(ecs::name_of<T>()); });
        EXPECT_EQ(names.size(), 3);
        EXPECT_EQ(names[0], ecs::name_of<Pos>());
        EXPECT_EQ(names[2], ecs::name_of<TagA>());
    }

    {
        ecs::registry w;
        for (int i = 0; i < 6; ++i)
        {
            const ecs::entity e = w.create(Pos{i});
            if (i % 2 == 0)
            {
                w.add<Vel>(e, Vel{i});
            }
            if (i % 3 == 0)
            {
                w.add<TagB>(e);
            }
        }
        using Movers = ecs::types<Pos, const Vel>;
        const auto sel = w.view(Movers{}, !ecs::exists<TagB>{});
        static_assert(std::same_as<decltype(sel)::included, Movers>);
        static_assert(std::same_as<decltype(sel)::filter_type, decltype(!ecs::exists<TagB>{})>);
        std::size_t manifest_count = 0;
        sel.each([&](Pos&, const Vel&) { ++manifest_count; });
        EXPECT_EQ(manifest_count, sel.count());
        EXPECT_EQ((sel.count()), (w.view<Pos, const Vel>(!ecs::exists<TagB>{}).count()));

        std::size_t each_count = 0;
        w.each(ecs::types<Pos>{}, [&](Pos&) { ++each_count; });
        EXPECT_EQ(each_count, 6);
        w.each(Movers{}, [&](Pos&, const Vel&) { --each_count; }, !ecs::exists<TagB>{});
        EXPECT_EQ(each_count, 6 - manifest_count);

        constexpr std::size_t arity = decltype(sel)::included::size;
        static_assert(arity == 2);
    }

    {
        using Saved = ecs::types<Pos, Stable, TagA>;
        ecs::registry w;
        const ecs::entity e = w.create(Pos{3});
        w.add<Stable>(e, Stable{4});
        w.add<TagA>(e);

        byte_writer save;
        ecs::pack(w, save, Saved{});
        ecs::registry fresh;
        byte_reader in{&save.data};
        EXPECT_TRUE(ecs::unpack(fresh, in, Saved{}).has_value());
        EXPECT_EQ(fresh.get<Pos>(e).x, 3);
        EXPECT_TRUE(fresh.has<TagA>(e));

        ecs::registry host;
        byte_reader in2{&save.data};
        const auto grafted = ecs::graft(host, in2, Saved{});
        EXPECT_TRUE(grafted.has_value());
        EXPECT_EQ(host.get<Pos>(grafted->resolve(e)).x, 3);
        EXPECT_TRUE(RegistryValid(host));
    }
}

TEST(Core, WorldOpsAndInspection)
{
    ecs::registry w;
    for (int i = 0; i < 32; ++i)
    {
        w.create(Pos{i}, Vel{i});
    }
    w.purge<Vel>();
    EXPECT_EQ(w.view<Vel>().count(), 0);
    EXPECT_EQ(w.view<Pos>().count(), 32);
    EXPECT_EQ(w.live_count(), 32);

    for (int i = 0; i < 16; ++i)
    {
        w.destroy(w.current_handle(static_cast<std::uint32_t>(i)));
    }
    EXPECT_EQ(w.live_count(), 16);
    EXPECT_EQ(w.slot_count(), 32);
    w.shrink();
    EXPECT_EQ(w.live_count(), 16);
    EXPECT_EQ(w.view<Pos>().count(), 16);
    EXPECT_TRUE(RegistryValid(w));

    bool saw_pos = false;
    w.each_pool(
        [&](const ecs::pool_info& info)
        {
            if (info.name == ecs::name_of<Pos>())
            {
                saw_pos = true;
                EXPECT_EQ(info.size, 16);
                EXPECT_EQ(info.bytes_per_item, sizeof(Pos));
                EXPECT_EQ(info.kind, ecs::storage::packed);
            }
        });
    EXPECT_TRUE(saw_pos);
    EXPECT_EQ(ecs::name_of<Pos>(), "Pos");

    ecs::registry first;
    ecs::registry second;
    first.create(OnlyHere1{1});
    second.create(OnlyHere2{2});
    second.create(OnlyHere1{3});
    first.create(OnlyHere2{4});
    EXPECT_EQ(first.view<OnlyHere1>().count(), 1);
    EXPECT_EQ(first.view<OnlyHere2>().count(), 1);
    EXPECT_EQ(second.view<OnlyHere1>().count(), 1);
    EXPECT_EQ(second.view<OnlyHere2>().count(), 1);
    EXPECT_TRUE(RegistryValid(first));
    EXPECT_TRUE(RegistryValid(second));
}

TEST(Core, ViewSingleAndCollect)
{
    ecs::registry w;
    const ecs::entity a = w.create(Pos{1});
    const ecs::entity b = w.create(Pos{2});
    const ecs::entity c = w.create(Pos{3});
    w.add<TagA>(b);

    EXPECT_EQ((w.view<Pos, TagA>().single()), b);

    const std::vector<ecs::entity> all = w.view<Pos>().collect();
    EXPECT_EQ(all.size(), 3U);
    EXPECT_EQ((std::vector<ecs::entity>{a, b, c}), all);

#if ECS_CHECKS
    {
        const violation_scope guard;
        EXPECT_EQ((w.view<Pos, TagB>().single()), ecs::entity{});
        EXPECT_GE(violations_seen, 1);
    }
    {
        const violation_scope guard;
        EXPECT_EQ(w.view<Pos>().single(), ecs::entity{});
        EXPECT_GE(violations_seen, 1);
    }
#endif
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, EntityFillerForwarders)
{
    ecs::registry w;
    const ecs::entity_filler e = w.create(Pos{1}, Vel{2});

    EXPECT_TRUE((e.has_all<Pos, Vel>()));
    EXPECT_FALSE((e.has_all<Pos, Hp>()));
    EXPECT_TRUE((e.has_any<Hp, Vel>()));
    EXPECT_FALSE((e.has_any<Hp, TagA>()));

    auto [pp, vp, hp] = e.find_all<Pos, Vel, Hp>();
    EXPECT_NE(pp, nullptr);
    EXPECT_NE(vp, nullptr);
    EXPECT_EQ(hp, nullptr);
    EXPECT_EQ(pp->x, 1);

    const Pos& after = e.amend<Pos>([](Pos& p) { p.x = 9; });
    EXPECT_EQ(after.x, 9);
    EXPECT_EQ(e.get<Pos>().x, 9);

    const ecs::entity_filler empty;
    EXPECT_FALSE((empty.has_all<Pos, Vel>()));
    EXPECT_FALSE((empty.has_any<Pos, Vel>()));
    const auto [ep, ev] = empty.find_all<Pos, Vel>();
    EXPECT_EQ(ep, nullptr);
    EXPECT_EQ(ev, nullptr);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Core, StringId)
{
    EXPECT_EQ(ecs::string_id(ecs::name_of<Pos>()), ecs::hash_of<Pos>());

    constexpr std::uint64_t camera = ecs::string_id("camera");
    static_assert(camera == ecs::string_id("camera"));
    EXPECT_NE(ecs::string_id("camera"), ecs::string_id("player"));

    EXPECT_EQ(ecs::string_id("meta.labeled"), ecs::string_id(std::string_view{"meta.labeled"}));
}
