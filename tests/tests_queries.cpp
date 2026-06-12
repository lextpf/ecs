// ============================================================================
// tests_queries.cpp -- any_of query combinators: OR-alternatives in select,
// union driving with dedup, and composition with except/maybe/manifests.
// ============================================================================

#include "test_harness.hpp"

#include <unordered_set>

// Alternative components for the OR tests.
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

void test_any_of_basics()
{
    section("any_of: pointer parts, >=1 non-null per row");
    ecs::world w;

    const ecs::entity cat = w.spawn(Pos{1});
    w.add<Cat>(cat, Cat{10});
    const ecs::entity dog = w.spawn(Pos{2});
    w.add<Dog>(dog, Dog{20});
    const ecs::entity both = w.spawn(Pos{3});
    w.add<Cat>(both, Cat{30});
    w.add<Dog>(both, Dog{40});
    w.spawn(Pos{4});  // neither: filtered out

    // The nullity matrix: cat-only, dog-only, both -- never neither.
    int cats = 0;
    int dogs = 0;
    int boths = 0;
    auto sel = w.select<Pos, ecs::any_of<Cat, Dog>>();
    sel.each(
        [&](ecs::entity, Pos&, Cat* c, Dog* d)
        {
            CHECK(c != nullptr || d != nullptr);  // the strengthened invariant
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
    CHECK(cats == 1);
    CHECK(dogs == 1);
    CHECK(boths == 1);

    // Tag alternatives are legal and contribute no callback argument.
    const ecs::entity tagged = w.spawn(Pos{5});
    w.add<TagA>(tagged);
    std::size_t flagged = 0;
    w.select<Pos, ecs::any_of<TagA, Cat>>().each([&](Pos&, Cat*) { ++flagged; });
    CHECK(flagged == 3);  // cat, both, tagged

    // A const world serves the same shapes, const-propagated.
    const ecs::world& cw = w;
    int seen = 0;
    cw.select<Pos, ecs::any_of<Cat, Dog>>().each(
        [&](const Pos&, const Cat* c, const Dog* d)
        { seen += (c != nullptr || d != nullptr) ? 1 : 0; });
    CHECK(seen == 3);

    CHECK_VALID(w);
}

void test_any_of_driving()
{
    section("any_of: union driving with exact dedup");
    ecs::world w;

    // No plain include: the union drives; overlap is visited exactly once.
    std::vector<ecs::entity> all;
    for (int i = 0; i < 12; ++i)
    {
        const ecs::entity e = w.spawn();
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
    // Cats: 6, dogs: 4, overlap (i % 6 == 0): 2 -> union = 8 distinct.
    std::unordered_set<ecs::entity, ecs::entity_hash> visited;
    std::size_t visits = 0;
    w.select<ecs::any_of<Cat, Dog>>().each(
        [&](ecs::entity e, Cat*, Dog*)
        {
            visited.insert(e);
            ++visits;
        });
    CHECK(visits == 8);          // exactly once each
    CHECK(visited.size() == 8);  // the exact union

    // Brute-force oracle agreement.
    std::size_t oracle = 0;
    for (const ecs::entity e : all)
    {
        oracle += (w.has<Cat>(e) || w.has<Dog>(e)) ? 1 : 0;
    }
    CHECK(oracle == 8);

    // With a small plain include, the plain pool drives; the group becomes an
    // OR-probe.
    const ecs::entity special = all[0];  // has Cat (i=0) and Dog
    w.add<Hp>(special, Hp{1});
    std::size_t narrowed = 0;
    w.select<Hp, ecs::any_of<Cat, Dog>>().each([&](Hp&, Cat*, Dog*) { ++narrowed; });
    CHECK(narrowed == 1);

    // A union smaller than the plain pool drives; the match set never changes.
    ecs::world w2;
    for (int i = 0; i < 100; ++i)
    {
        w2.spawn(Pos{i});  // big plain pool
    }
    const ecs::entity rare = w2.spawn(Pos{1000});
    w2.add<Cat>(rare, Cat{1});
    std::size_t found = 0;
    w2.select<Pos, ecs::any_of<Cat, Dog>>().each([&](Pos&, Cat*, Dog*) { ++found; });
    CHECK(found == 1);

    // driven_by still forces a PLAIN include.
    std::size_t forced = 0;
    w2.select<Pos, ecs::any_of<Cat, Dog>>().driven_by<Pos>().each([&](Pos&, Cat*, Dog*)
                                                                  { ++forced; });
    CHECK(forced == 1);

    CHECK_VALID(w);
    CHECK_VALID(w2);
}

void test_any_of_composition()
{
    section("any_of: composition with except, maybe, manifests, range, split");
    ecs::world w;

    ecs::entity muzzled_cat = ecs::no_entity;
    for (int i = 0; i < 10; ++i)
    {
        const ecs::entity e = w.spawn(Pos{i});
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

    // except + maybe + any_of in one select.
    auto sel = w.select<Pos, ecs::any_of<Cat, Dog>, ecs::maybe<Vel>>(ecs::except<Muzzled>{});
    std::size_t rows = 0;
    std::size_t with_vel = 0;
    sel.each(
        [&](ecs::entity e, Pos&, Cat* c, Dog* d, Vel* v)
        {
            CHECK(e != muzzled_cat);
            CHECK(c != nullptr || d != nullptr);
            rows += 1;
            with_vel += (v != nullptr) ? 1 : 0;
        });
    CHECK(rows == 9);
    CHECK(with_vel == 4);  // i in {0, 3, 6, 9}

    // first / count / contains agree with the walk.
    CHECK(sel.count() == 9);
    CHECK(!sel.contains(muzzled_cat));
    CHECK(sel.contains(sel.first()));

    // Two independent groups: both must be satisfied.
    const ecs::entity cross = w.spawn(Pos{100});
    w.add<Cat>(cross, Cat{100});
    w.add<Vel>(cross, Vel{100});
    std::size_t crossed = 0;
    w.select<ecs::any_of<Cat, Dog>, ecs::any_of<Vel, Hp>>().each(
        [&](ecs::entity, Cat*, Dog*, Vel*, Hp*) { ++crossed; });
    // Entities with (Cat|Dog) AND (Vel|Hp): i in {0,3,6,9} plus cross.
    CHECK(crossed == 5);

    // Manifests carry any_of like any other element.
    using Pets = ecs::types<Pos, ecs::any_of<Cat, Dog>>;
    std::size_t via_manifest = 0;
    w.select(Pets{}).each([&](Pos&, Cat*, Dog*) { ++via_manifest; });
    CHECK(via_manifest == 11);

    // range() carries the pointer parts. Selections are live: `cross` now matches.
    std::size_t range_rows = 0;
    for (auto [e, p, c, d, v] : sel.range())
    {
        range_rows += (c != nullptr || d != nullptr) ? 1 : 0;
    }
    CHECK(range_rows == 10);

    // split() covers every row exactly once (plain include drives parts).
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
        CHECK(visits == 10);
        CHECK(seen.size() == 10);
    }

    CHECK_VALID(w);

    // Compile-time walls (each is a static_assert):
    //   select<any_of<Cat>>()                  // a group needs >= 2 alternatives
    //   select<any_of<Cat, Cat>>()             // duplicate alternative
    //   select<Cat, any_of<Cat, Dog>>()        // type appears twice across the select
    //   select<any_of<Cat, Dog>>(except<Cat>{})  // alternative also excluded
    //   select<ecs::maybe<ecs::any_of<Cat, Dog>>>()  // maybe<any_of<>> is meaningless
    //   bond<ecs::any_of<Cat, Dog>, Pos>()     // bonds own pools, not alternatives
}

#if ECS_CHECKS
void test_any_of_violations()
{
    section("any_of: lock discipline covers every alternative pool");
    ecs::world w;
    const ecs::entity e = w.spawn(Pos{1});
    w.add<Cat>(e, Cat{1});

    // Every alternative pool is locked during iteration; structural changes to
    // any of them are refused.
    violation_scope guard;
    w.select<Pos, ecs::any_of<Cat, Dog>>().each(
        [&](ecs::entity who, Pos&, Cat*, Dog*)
        {
            w.remove<Cat>(who);  // Cat pool is locked as an alternative
            return false;
        });
    CHECK(violations_seen == 1);
    CHECK(w.has<Cat>(e));  // refused: still present
    CHECK_VALID(w);
}
#endif
