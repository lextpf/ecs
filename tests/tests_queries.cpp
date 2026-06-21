#include "test_harness.hpp"

#include <unordered_set>

struct Cat
{
    int meow = 0;
};

struct Dog
{
    int woof = 0;
};

struct Muzzled
{
};

TEST(Queries, ViewGet)
{
    ecs::registry w;
    const ecs::entity a = w.create(Pos{1}, Vel{2});
    const ecs::entity b = w.create(Pos{3}, Vel{4});

    auto v = w.view<Pos, const Vel>();
    EXPECT_EQ(v.get<Pos>(a).x, 1);
    EXPECT_EQ(v.get<Pos>(b).x, 3);
    EXPECT_EQ(v.get<Vel>(a).v, 2);
    static_assert(std::is_same_v<decltype(v.get<Pos>(a)), Pos&>);
    static_assert(std::is_same_v<decltype(v.get<Vel>(a)), const Vel&>);

    v.get<Pos>(a).x = 9;
    EXPECT_EQ(w.get<Pos>(a).x, 9);

    auto [p, vel] = v.get<Pos, Vel>(b);
    EXPECT_EQ(p.x, 3);
    EXPECT_EQ(vel.v, 4);
}

TEST(Queries, AnyOfBasics)
{
    ecs::registry w;

    const ecs::entity cat = w.create(Pos{1});
    w.add<Cat>(cat, Cat{10});
    const ecs::entity dog = w.create(Pos{2});
    w.add<Dog>(dog, Dog{20});
    const ecs::entity both = w.create(Pos{3});
    w.add<Cat>(both, Cat{30});
    w.add<Dog>(both, Dog{40});
    w.create(Pos{4});

    int cats = 0;
    int dogs = 0;
    int boths = 0;
    auto sel =
        w.view<Pos, ecs::maybe<Cat>, ecs::maybe<Dog>>(ecs::exists<Cat>{} || ecs::exists<Dog>{});
    sel.each(
        [&](ecs::entity, Pos&, Cat* c, Dog* d)
        {
            EXPECT_TRUE(c != nullptr || d != nullptr);
            if (c != nullptr && d != nullptr)
            {
                ++boths;
            }
            else if (c != nullptr)
            {
                ++cats;
            }
            else
            {
                ++dogs;
            }
        });
    EXPECT_EQ(cats, 1);
    EXPECT_EQ(dogs, 1);
    EXPECT_EQ(boths, 1);

    const ecs::entity tagged = w.create(Pos{5});
    w.add<TagA>(tagged);
    std::size_t flagged = 0;
    w.view<Pos, ecs::maybe<Cat>>(ecs::exists<TagA>{} || ecs::exists<Cat>{})
        .each([&](Pos&, Cat*) { ++flagged; });
    EXPECT_EQ(flagged, 3);

    const ecs::registry& cw = w;
    int seen = 0;
    cw.view<Pos, ecs::maybe<Cat>, ecs::maybe<Dog>>(ecs::exists<Cat>{} || ecs::exists<Dog>{})
        .each([&](const Pos&, const Cat* c, const Dog* d)
              { seen += (c != nullptr || d != nullptr) ? 1 : 0; });
    EXPECT_EQ(seen, 3);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Queries, AnyOfDriving)
{
    ecs::registry w;

    std::vector<ecs::entity> all;
    for (int i = 0; i < 12; ++i)
    {
        const ecs::entity e = w.create(Pos{i});
        if (i % 2 == 0)
        {
            w.add<Cat>(e, Cat{i});
        }
        if (i % 3 == 0)
        {
            w.add<Dog>(e, Dog{i});
        }
        all.push_back(e);
    }
    std::unordered_set<ecs::entity, ecs::entity_hash> visited;
    std::size_t visits = 0;
    w.view<Pos, ecs::maybe<Cat>, ecs::maybe<Dog>>(ecs::exists<Cat>{} || ecs::exists<Dog>{})
        .each(
            [&](ecs::entity e, Pos&, Cat*, Dog*)
            {
                visited.insert(e);
                ++visits;
            });
    EXPECT_EQ(visits, 8);
    EXPECT_EQ(visited.size(), 8);

    std::size_t oracle = 0;
    for (const ecs::entity e : all)
    {
        oracle += (w.has<Cat>(e) || w.has<Dog>(e)) ? 1 : 0;
    }
    EXPECT_EQ(oracle, 8);

    const ecs::entity special = all[0];
    w.add<Hp>(special, Hp{1});
    std::size_t narrowed = 0;
    w.view<Hp, ecs::maybe<Cat>, ecs::maybe<Dog>>(ecs::exists<Cat>{} || ecs::exists<Dog>{})
        .each([&](Hp&, Cat*, Dog*) { ++narrowed; });
    EXPECT_EQ(narrowed, 1);

    ecs::registry w2;
    for (int i = 0; i < 100; ++i)
    {
        w2.create(Pos{i});
    }
    const ecs::entity rare = w2.create(Pos{1000});
    w2.add<Cat>(rare, Cat{1});
    std::size_t found = 0;
    w2.view<Pos, ecs::maybe<Cat>, ecs::maybe<Dog>>(ecs::exists<Cat>{} || ecs::exists<Dog>{})
        .each([&](Pos&, Cat*, Dog*) { ++found; });
    EXPECT_EQ(found, 1);

    std::size_t forced = 0;
    w2.view<Pos, ecs::maybe<Cat>, ecs::maybe<Dog>>(ecs::exists<Cat>{} || ecs::exists<Dog>{})
        .driven_by<Pos>()
        .each([&](Pos&, Cat*, Dog*) { ++forced; });
    EXPECT_EQ(forced, 1);

    EXPECT_TRUE(RegistryValid(w));
    EXPECT_TRUE(RegistryValid(w2));
}

TEST(Queries, AnyOfComposition)
{
    ecs::registry w;

    ecs::entity muzzled_cat = ecs::no_entity;
    for (int i = 0; i < 10; ++i)
    {
        const ecs::entity e = w.create(Pos{i});
        if (i % 2 == 0)
        {
            w.add<Cat>(e, Cat{i});
        }
        else
        {
            w.add<Dog>(e, Dog{i});
        }
        if (i % 3 == 0)
        {
            w.add<Vel>(e, Vel{i * 10});
        }
        if (i == 4)
        {
            w.add<Muzzled>(e);
            muzzled_cat = e;
        }
    }

    auto sel = w.view<Pos, ecs::maybe<Cat>, ecs::maybe<Dog>, ecs::maybe<Vel>>(
        (ecs::exists<Cat>{} || ecs::exists<Dog>{}) && !ecs::exists<Muzzled>{});
    std::size_t rows = 0;
    std::size_t with_vel = 0;
    sel.each(
        [&](ecs::entity e, Pos&, Cat* c, Dog* d, Vel* v)
        {
            EXPECT_NE(e, muzzled_cat);
            EXPECT_TRUE(c != nullptr || d != nullptr);
            rows += 1;
            with_vel += (v != nullptr) ? 1 : 0;
        });
    EXPECT_EQ(rows, 9);
    EXPECT_EQ(with_vel, 4);

    EXPECT_EQ(sel.count(), 9);
    EXPECT_FALSE(sel.contains(muzzled_cat));
    EXPECT_TRUE(sel.contains(sel.first()));

    const ecs::entity cross = w.create(Pos{100});
    w.add<Cat>(cross, Cat{100});
    w.add<Vel>(cross, Vel{100});
    std::size_t crossed = 0;
    w.view<Pos, ecs::maybe<Cat>, ecs::maybe<Dog>, ecs::maybe<Vel>, ecs::maybe<Hp>>(
         (ecs::exists<Cat>{} || ecs::exists<Dog>{}) && (ecs::exists<Vel>{} || ecs::exists<Hp>{}))
        .each([&](ecs::entity, Pos&, Cat*, Dog*, Vel*, Hp*) { ++crossed; });
    EXPECT_EQ(crossed, 5);

    using Pets = ecs::types<Pos, ecs::maybe<Cat>, ecs::maybe<Dog>>;
    std::size_t via_manifest = 0;
    w.view(Pets{}, ecs::exists<Cat>{} || ecs::exists<Dog>{})
        .each([&](Pos&, Cat*, Dog*) { ++via_manifest; });
    EXPECT_EQ(via_manifest, 11);

    std::size_t range_rows = 0;
    for (auto [e, p, c, d, v] : sel.each())
    {
        range_rows += (c != nullptr || d != nullptr) ? 1 : 0;
    }
    EXPECT_EQ(range_rows, 10);

    {
        std::unordered_set<ecs::entity, ecs::entity_hash> seen;
        std::size_t visits = 0;
        const auto work = sel.split(3);
        for (std::size_t i = 0; i < work.parts(); ++i)
        {
            work.part(i).entities(
                [&](ecs::entity e)
                {
                    seen.insert(e);
                    ++visits;
                });
        }
        EXPECT_EQ(visits, 10);
        EXPECT_EQ(seen.size(), 10);
    }

    EXPECT_TRUE(RegistryValid(w));
}

#if ECS_CHECKS
TEST(Queries, AnyOfViolations)
{
    ecs::registry w;
    const ecs::entity e = w.create(Pos{1});
    w.add<Cat>(e, Cat{1});

    violation_scope guard;
    w.view<Pos, ecs::maybe<Cat>, ecs::maybe<Dog>>(ecs::exists<Cat>{} || ecs::exists<Dog>{})
        .each(
            [&](ecs::entity who, Pos&, Cat*, Dog*)
            {
                w.remove<Cat>(who);
                return false;
            });
    EXPECT_EQ(violations_seen, 1);
    EXPECT_TRUE(w.has<Cat>(e));
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Queries, ViewGetStaleHandleAborts)
{
    ecs::registry w;
    const ecs::entity gone = w.create(Pos{1});
    w.destroy(gone);
    const ecs::entity fresh = w.create(Pos{2});
    ASSERT_EQ(fresh.index(), gone.index());
    EXPECT_DEATH({ (void)w.view<Pos>().get<Pos>(gone); }, "");
    EXPECT_TRUE(RegistryValid(w));
}
#endif

TEST(Queries, ViewBackAndReversed)
{
    ecs::registry w;
    std::vector<ecs::entity> order;
    for (int i = 0; i < 5; ++i)
    {
        order.push_back(w.create(Pos{i}));
    }

    const auto v = w.view<Pos>();

    EXPECT_EQ(v.first(), order.front());
    EXPECT_EQ(v.back(), order.back());

    std::vector<int> seen;
    for (auto&& [e, p] : v.reversed())
    {
        (void)e;
        seen.push_back(p.x);
    }
    EXPECT_TRUE((seen == std::vector<int>{4, 3, 2, 1, 0}));

    w.add<Cat>(order[1]);
    w.add<Cat>(order[3]);
    const auto cats = w.view<Pos>(ecs::exists<Cat>{});
    EXPECT_EQ(cats.back(), order[3]);
    std::vector<int> cat_x;
    for (auto&& [e, p] : cats.reversed())
    {
        (void)e;
        cat_x.push_back(p.x);
    }
    EXPECT_TRUE((cat_x == std::vector<int>{3, 1}));

    const auto none = w.view<Hp>();
    EXPECT_EQ(none.back(), ecs::no_entity);
    int count = 0;
    for (auto&& [e, h] : none.reversed())
    {
        (void)e;
        (void)h;
        ++count;
    }
    EXPECT_EQ(count, 0);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Queries, WhereValuePredicate)
{
    ecs::registry w;
    const ecs::entity a = w.create(Pos{1}, Hp{5});
    w.create(Pos{2}, Hp{50});
    w.create(Pos{3}, Hp{8});
    w.create(Hp{1});

    const auto hurt = [](const Hp& h) { return h.hp < 10; };

    EXPECT_EQ((w.view<Pos>(ecs::where<Hp>(hurt)).count()), 2);

    std::vector<int> xs;
    w.view<Pos>(ecs::where<Hp>(hurt)).each([&](Pos& p) { xs.push_back(p.x); });
    std::sort(xs.begin(), xs.end());
    EXPECT_EQ(xs, (std::vector<int>{1, 3}));

    w.add<TagA>(a);
    EXPECT_EQ((w.view<Pos>(ecs::where<Hp>(hurt) && ecs::exists<TagA>{}).count()), 1);
    EXPECT_EQ((w.view<Pos>(!ecs::where<Hp>(hurt)).count()), 1);

    const ecs::entity d = w.create(Pos{4});
    EXPECT_FALSE(w.view<Pos>(ecs::where<Hp>(hurt)).contains(d));

    EXPECT_TRUE(RegistryValid(w));
}
