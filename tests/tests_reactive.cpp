#include "test_harness.hpp"

namespace
{
struct Burning
{
    int heat = 0;
};

struct Shielded
{
};
}

TEST(Reactive, Tracker)
{
    ecs::registry w;
    ecs::tracker<Hp> hurt(w);

    const ecs::entity a = w.create();
    const ecs::entity b = w.create();
    w.add<Hp>(a, Hp{10});
    w.add<Hp>(b, Hp{20});
    EXPECT_EQ(hurt.added().size(), 2U);

    w.replace<Hp>(a, Hp{9});
    w.replace<Hp>(a, Hp{8});
    w.put<Hp>(a, Hp{7});
    EXPECT_EQ(hurt.replaced().size(), 1U);
    EXPECT_EQ(hurt.replaced()[0], a);

    w.destroy(b);
    EXPECT_EQ(hurt.removed().size(), 1U);
    EXPECT_EQ(hurt.removed()[0], b);
    EXPECT_FALSE(w.alive(hurt.removed()[0]));

    hurt.clear();
    EXPECT_TRUE(hurt.added().empty());
    EXPECT_TRUE(hurt.replaced().empty());
    EXPECT_TRUE(hurt.removed().empty());
    w.replace<Hp>(a, Hp{6});
    EXPECT_EQ(hurt.replaced().size(), 1U);
    hurt.clear();

    w.remove<Hp>(a);
    hurt.clear();
    w.destroy(a);
    const ecs::entity recycled = w.create();
    EXPECT_EQ(recycled.index(), a.index());
    w.add<Hp>(recycled, Hp{1});
    EXPECT_EQ(hurt.added().size(), 1U);
    EXPECT_EQ(hurt.added()[0], recycled);

    ecs::tracker<Pos> moves(w, ecs::track::added | ecs::track::removed);
    const ecs::entity c = w.create(Pos{1});
    w.replace<Pos>(c, Pos{2});
    w.destroy(c);
    EXPECT_EQ(moves.added().size(), 1U);
    EXPECT_TRUE(moves.replaced().empty());
    EXPECT_EQ(moves.removed().size(), 1U);

    hurt.clear();
    w.create(Hp{1});
    w.create(Hp{2});
    hurt.clear();
    w.reset();
    EXPECT_EQ(hurt.removed().size(), 3U);

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
        ecs::tracker<Hp> counted(w, ecs::track::all, &arena);
        w.create(Hp{1});
        EXPECT_GT(arena.allocations, 0U);
    }
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Reactive, WatcherEntered)
{
    ecs::registry w;
    ecs::watcher<ecs::types<Burning, Hp>> entered(w);

    const ecs::entity e = w.create(Hp{10});
    EXPECT_EQ(entered.count(), 0U);
    EXPECT_TRUE(entered.empty());
    w.add<Burning>(e, Burning{5});
    EXPECT_EQ(entered.count(), 1U);
    EXPECT_FALSE(entered.empty());
    EXPECT_TRUE(entered.contains(e));

    w.remove<Hp>(e);
    EXPECT_EQ(entered.count(), 0U);
    EXPECT_FALSE(entered.contains(e));
    w.add<Hp>(e, Hp{20});
    EXPECT_EQ(entered.count(), 1U);

    w.remove<Burning>(e);
    w.add<Burning>(e, Burning{6});
    EXPECT_EQ(entered.count(), 1U);

    entered.clear();
    EXPECT_EQ(entered.count(), 0U);
    EXPECT_TRUE((w.has_all<Burning, Hp>(e)));
    w.remove<Burning>(e);
    w.add<Burning>(e, Burning{7});
    EXPECT_EQ(entered.count(), 1U);

    w.destroy(e);
    EXPECT_EQ(entered.count(), 0U);

    const ecs::entity old = w.create(Hp{1});
    w.add<Burning>(old, Burning{1});
    ecs::watcher<ecs::types<Burning, Hp>> late(w);
    EXPECT_EQ(late.count(), 0U);

    ecs::watcher<ecs::types<Burning>, ecs::except<Shielded>> guarded(w);
    EXPECT_EQ(guarded.count(), 0U);
    w.add<Shielded>(old);
    EXPECT_EQ(guarded.count(), 0U);
    w.remove<Shielded>(old);
    EXPECT_EQ(guarded.count(), 1U);
    EXPECT_TRUE(guarded.contains(old));
    w.add<Shielded>(old);
    EXPECT_EQ(guarded.count(), 0U);

    const ecs::entity f = w.create();
    w.add<Burning>(f, Burning{2});
    EXPECT_EQ(guarded.count(), 1U);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Reactive, WatcherChanged)
{
    ecs::registry w;
    ecs::watcher<ecs::types<Burning>, ecs::changed<Hp>> hurt_while_burning(w);

    const ecs::entity e = w.create(Burning{1}, Hp{100});
    EXPECT_EQ(hurt_while_burning.count(), 1U);
    hurt_while_burning.clear();

    w.replace<Hp>(e, Hp{90});
    EXPECT_EQ(hurt_while_burning.count(), 1U);
    hurt_while_burning.clear();
    w.put<Hp>(e, Hp{80});
    EXPECT_EQ(hurt_while_burning.count(), 1U);
    hurt_while_burning.clear();
    w.amend<Hp>(e, [](Hp& hp) { hp.hp -= 10; });
    EXPECT_EQ(hurt_while_burning.count(), 1U);
    hurt_while_burning.clear();
    w.get<Hp>(e).hp = 5;
    EXPECT_EQ(hurt_while_burning.count(), 0U);

    const ecs::entity cold = w.create(Hp{50});
    w.amend<Hp>(cold, [](Hp& hp) { hp.hp = 1; });
    EXPECT_EQ(hurt_while_burning.count(), 0U);

    w.amend<Hp>(e, [](Hp& hp) { hp.hp = 4; });
    EXPECT_EQ(hurt_while_burning.count(), 1U);
    w.remove<Burning>(e);
    EXPECT_EQ(hurt_while_burning.count(), 0U);

    ecs::watcher<ecs::types<Hp>, ecs::changed<Hp>> hp_watch(w);
    const ecs::entity g = w.create(Hp{7});
    EXPECT_EQ(hp_watch.count(), 1U);
    hp_watch.clear();
    w.amend<Hp>(g, [](Hp& hp) { ++hp.hp; });
    EXPECT_EQ(hp_watch.count(), 1U);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Reactive, WatcherLifetime)
{
    ecs::registry w;
    auto watcher_box = std::make_unique<ecs::watcher<ecs::types<Burning>>>(w);
    const ecs::entity e = w.create(Burning{1});
    EXPECT_EQ(watcher_box->count(), 1U);

    w.reset();
    EXPECT_EQ(watcher_box->count(), 0U);
    const ecs::entity f = w.create(Burning{2});
    EXPECT_EQ(watcher_box->count(), 1U);
    EXPECT_TRUE(watcher_box->contains(f));
    EXPECT_FALSE(watcher_box->contains(e));

    int total = 0;
    for (const ecs::entity m : watcher_box->matched())
    {
        total += w.get<Burning>(m).heat;
    }
    EXPECT_EQ(total, 2);

    watcher_box.reset();
    w.add<Hp>(f, Hp{1});
    w.remove<Burning>(f);
    w.add<Burning>(f, Burning{3});
    EXPECT_TRUE(RegistryValid(w));
}

#if ECS_CHECKS
TEST(Reactive, WatcherViolations)
{
    ecs::registry w;
    w.create(Burning{1});

    violation_scope guard;
    w.view<Burning>().each(
        [&](Burning&)
        {
            const ecs::watcher<ecs::types<Burning>> mid_loop(w);
            EXPECT_EQ(mid_loop.count(), 0U);
        });
    EXPECT_GE(violations_seen, 1);
    EXPECT_TRUE(RegistryValid(w));
}
#endif

TEST(Reactive, ViewEachOf)
{
    ecs::registry w;
    ecs::tracker<Hp> hurt(w, ecs::track::replaced);

    const ecs::entity a = w.create(Pos{1}, Hp{10});
    const ecs::entity b = w.create(Hp{20});
    const ecs::entity c = w.create(Pos{3}, Hp{30});
    w.replace<Hp>(a, Hp{9});
    w.replace<Hp>(b, Hp{19});
    w.replace<Hp>(c, Hp{29});

    std::vector<int> xs;
    w.view<Pos>().each_of(hurt.replaced(), [&](Pos& p) { xs.push_back(p.x); });
    std::sort(xs.begin(), xs.end());
    EXPECT_EQ(xs, (std::vector<int>{1, 3}));

    w.add<TagA>(a);
    std::vector<ecs::entity> tagged;
    w.view<Pos>(ecs::exists<TagA>{})
        .each_of(hurt.replaced(), [&](ecs::entity e, Pos&) { tagged.push_back(e); });
    EXPECT_EQ(tagged, (std::vector<ecs::entity>{a}));

    w.destroy(c);
    int live = 0;
    w.view<Pos>().each_of(hurt.replaced(), [&](Pos&) { ++live; });
    EXPECT_EQ(live, 1);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Reactive, MutatorsReturnLiveRefAfterReallocatingHook)
{
    const ecs::component_hook grow_pos =
        +[](ecs::registry& reg, ecs::entity, void*) { reg.reserve<Pos>(1u << 16); };

    {
        ecs::registry w;
        const ecs::entity e = w.create(Pos{1});
        const ecs::scoped_hook hook(w, w.on_replace<Pos>(grow_pos));
        Pos& r = w.replace<Pos>(e, Pos{2});
        EXPECT_EQ(&r, w.find<Pos>(e));
    }
    {
        ecs::registry w;
        const ecs::entity e = w.create(Pos{1});
        const ecs::scoped_hook hook(w, w.on_replace<Pos>(grow_pos));
        Pos& r = w.amend<Pos>(e, [](Pos& p) { p.x = 3; });
        EXPECT_EQ(&r, w.find<Pos>(e));
    }
    {
        ecs::registry w;
        const ecs::entity e = w.create(Pos{1});
        const ecs::scoped_hook hook(w, w.on_replace<Pos>(grow_pos));
        Pos& r = w.put<Pos>(e, Pos{4});
        EXPECT_EQ(&r, w.find<Pos>(e));
    }
}

TEST(Reactive, TouchFiresReplaceForBareWrite)
{
    ecs::registry w;
    ecs::tracker<Hp> hurt(w, ecs::track::replaced);
    const ecs::entity e = w.create(Hp{10});

    w.get<Hp>(e).hp = 5;
    EXPECT_TRUE(hurt.replaced().empty());

    w.touch<Hp>(e);
    EXPECT_EQ(hurt.replaced().size(), 1U);
    EXPECT_EQ(hurt.replaced()[0], e);
    EXPECT_EQ(w.get<Hp>(e).hp, 5);

    EXPECT_TRUE(RegistryValid(w));
}
