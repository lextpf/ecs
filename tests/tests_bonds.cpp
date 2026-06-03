// ============================================================================
// tests_bonds.cpp — N-ary bonds (M2): bond<A, B, C, ...> mirrored partitions
// across up to 8 owners, churn maintenance, violation matrix, unbond
// semantics. The 2-ary regression gate is test_bonded_pairs in tests.cpp,
// which must keep passing unchanged through the same machinery.
// ============================================================================

#include "test_harness.hpp"

void test_bond_n_ary()
{
    section("bond<A, B, C>: N-way mirrored partitions");
    ecs::world w;

    // Pre-populated pools in scrambled membership, then a one-time build.
    for (int i = 0; i < 12; ++i)
    {
        const ecs::entity e = w.spawn(Pos{i});
        if (i % 2 == 0)
        {
            w.add<Vel>(e, Vel{i * 10});
        }
        if (i % 3 == 0)
        {
            w.add<Hp>(e, Hp{i * 100});
        }
    }

    // Pos ∩ Vel ∩ Hp = entities with i % 6 == 0 → i ∈ {0, 6}.
    auto trio = w.bond<Pos, Vel, Hp>();
    CHECK(trio.count() == 2);
    CHECK_VALID(w);

    // The mirror invariant, verified directly: the same entity at the same
    // dense position in EVERY owner.
    auto mirrored = [&](std::size_t expect)
    {
        auto pa = w.find_pool<Pos>();
        auto pb = w.find_pool<Vel>();
        auto pc = w.find_pool<Hp>();
        const auto view = w.bonded<Pos, Vel, Hp>();
        if (view.count() != expect)
        {
            return false;
        }
        for (std::size_t pos = 0; pos < expect; ++pos)
        {
            const ecs::entity e = pa.entity_at(pos);
            if (pb.entity_at(pos) != e || pc.entity_at(pos) != e || view.entity_at(pos) != e)
            {
                return false;
            }
        }
        return true;
    };
    CHECK(mirrored(2));

    // Rows come from all three pools in lockstep, zero probes.
    int rows = 0;
    trio.each(
        [&](ecs::entity, Pos& p, Vel& v, Hp& h)
        {
            ++rows;
            CHECK(p.x % 6 == 0);
            CHECK(v.v == p.x * 10);
            CHECK(h.hp == p.x * 100);
        });
    CHECK(rows == 2);

    // Churn through EVERY side maintains the partition.
    const ecs::entity joiner = w.spawn(Pos{100});
    CHECK(trio.count() == 2);
    w.add<Vel>(joiner, Vel{1000});  // still missing Hp: not in the partition
    CHECK(trio.count() == 2);
    w.add<Hp>(joiner, Hp{10000});  // completes the intersection
    CHECK(trio.count() == 3);
    CHECK(mirrored(3));
    CHECK(trio.contains(joiner));
    CHECK_VALID(w);

    w.remove<Vel>(joiner);  // leaves through a middle pool
    CHECK(trio.count() == 2);
    CHECK(mirrored(2));
    CHECK(!trio.contains(joiner));
    w.add<Vel>(joiner, Vel{1000});  // re-enters
    CHECK(trio.count() == 3);
    w.kill(joiner);  // kill exits through every pool
    CHECK(trio.count() == 2);
    CHECK(mirrored(2));
    CHECK_VALID(w);

    // entities() walks the intersection ids.
    int seen = 0;
    trio.entities([&](ecs::entity e) { seen += w.has_all<Pos, Vel, Hp>(e) ? 1 : 0; });
    CHECK(seen == 2);

    // range() yields lockstep rows.
    int range_rows = 0;
    for (auto [e, p, v, h] : trio.range())
    {
        range_rows += (v.v == p.x * 10 && h.hp == p.x * 100) ? 1 : 0;
    }
    CHECK(range_rows == 2);

    // 4-way bond with a tag owner (tags are membership-only, no argument).
    ecs::world w4;
    for (int i = 0; i < 8; ++i)
    {
        const ecs::entity e = w4.spawn(Pos{i});
        if (i < 6)
        {
            w4.add<Vel>(e, Vel{i});
        }
        if (i < 4)
        {
            w4.add<Hp>(e, Hp{i});
        }
        if (i < 2)
        {
            w4.add<TagA>(e);
        }
    }
    auto quad = w4.bond<Pos, Vel, Hp, TagA>();
    CHECK(quad.count() == 2);
    int qrows = 0;
    quad.each([&](Pos&, Vel&, Hp&) { ++qrows; });
    CHECK(qrows == 2);
    w4.remove<TagA>(w4.select<TagA>().first());
    CHECK(quad.count() == 1);
    CHECK_VALID(w4);

    // The 2-ary spelling rides the same machinery (exhaustive regression
    // lives in test_bonded_pairs, unchanged).
    ecs::world w2;
    const ecs::entity x = w2.spawn(Pos{1}, Vel{2});
    auto duo = w2.bond<Pos, Vel>();
    CHECK(duo.count() == 1);
    CHECK(duo.contains(x));
    CHECK_VALID(w2);
}

void test_bond_n_ary_paths()
{
    section("bond<A, B, C>: every membership path re-pairs");
    ecs::world w;
    w.bond<Pos, Vel, Stable>();  // bond BEFORE any members; one stable owner
    const auto view = w.bonded<Pos, Vel, Stable>();

    // Spawn packs.
    w.spawn(Pos{1}, Vel{10}, Stable{100});
    CHECK(view.count() == 1);

    // Blueprints.
    ecs::blueprint recipe;
    recipe.add<Pos>(Pos{2});
    recipe.add<Vel>(Vel{20});
    recipe.add<Stable>(Stable{200});
    const ecs::entity b = w.spawn(recipe);
    CHECK(view.count() == 2);

    // duplicate.
    const auto dup = w.duplicate(b);
    CHECK(w.alive(dup.clone));
    CHECK(view.count() == 3);

    // Command-buffer replay with a provisional spawn.
    ecs::command_buffer cmd;
    const ecs::entity prov = cmd.spawn();
    cmd.add<Pos>(prov, Pos{4});
    cmd.add<Vel>(prov, Vel{40});
    cmd.add<Stable>(prov, Stable{400});
    w.apply(cmd);
    CHECK(view.count() == 4);
    CHECK_VALID(w);

    // Archive loads re-pair against a standing bond in the target world.
    byte_writer out;
    ecs::pack<Pos, Vel, Stable>(w, out);
    ecs::world fresh;
    fresh.bond<Pos, Vel, Stable>();
    byte_reader in{&out.data};
    CHECK((ecs::unpack<Pos, Vel, Stable>(fresh, in).has_value()));
    CHECK((fresh.bonded<Pos, Vel, Stable>().count() == 4));
    CHECK_VALID(fresh);

    // Lifetime balance under partition churn (mirror swaps move packed
    // payloads; stable owners never move theirs).
    ecs::world w2;
    w2.bond<Hp, StableCounted, TagB>();
    const int live_before = Counted::live;
    for (int i = 0; i < 16; ++i)
    {
        const ecs::entity e = w2.spawn(Hp{i});
        w2.add<StableCounted>(e, StableCounted{i});
        w2.add<TagB>(e);  // completes the intersection
        if (i % 2 == 0)
        {
            w2.remove<TagB>(e);  // leaves through the tag side
        }
        if (i % 3 == 0)
        {
            w2.kill(e);  // or through kill
        }
    }
    CHECK(Counted::live - live_before == w2.select<StableCounted>().count());
    CHECK_VALID(w2);
}

#if QUIVER_CHECKS
void test_bond_n_ary_violations()
{
    section("bond<A, B, C>: violation matrix");
    ecs::world w;
    const ecs::entity trio_member = w.spawn(Pos{1}, Vel{2}, Hp{3});
    w.spawn(Pos{4}, Vel{5}, Hp{6});  // sort's <2-element no-op must not mask refusals
    w.add<TagA>(trio_member);        // registers TagA so sort_along reaches its checks
    w.bond<Pos, Vel, Hp>();

    {
        // Overlapping owned sets are refused outright — subset, superset,
        // and partial overlap all spell "already bonded".
        violation_scope guard;
        auto v = w.bond<Pos, TagA>();
        CHECK(violations_seen == 1);
        CHECK(v.count() == 0);
        auto v2 = w.bond<Pos, Vel>();
        CHECK(violations_seen == 2);
        CHECK(v2.count() == 0);
    }
    {
        // sort / sort_along on an owned pool: refused.
        violation_scope guard;
        w.sort<Pos>([](const Pos& l, const Pos& r) { return l.x < r.x; });
        CHECK(violations_seen == 1);
        w.sort_along<Vel, TagA>();
        CHECK(violations_seen == 2);
    }
    {
        // Structural change on an owner refused while a CO-OWNER iterates.
        violation_scope guard;
        w.select<Hp>().each(
            [&](ecs::entity e, Hp&)
            {
                w.remove<Pos>(e);  // Pos is bond-locked through Hp's lock
                return false;
            });
        CHECK(violations_seen == 1);
        CHECK(w.has<Pos>(trio_member));  // refused: still present
    }
    {
        // A hook on one owner may not structurally touch a co-owner.
        violation_scope guard;
        const ecs::hook_token t =
            w.on_add<Pos>([](ecs::world& world, ecs::entity e, void*) { world.remove<Vel>(e); });
        const ecs::entity h = w.spawn();
        w.add<Vel>(h, Vel{1});
        w.add<Pos>(h, Pos{1});  // fires under Pos's lock; remove<Vel> refused
        CHECK(violations_seen == 1);
        CHECK(w.has<Vel>(h));
        w.unhook(t);
    }
    {
        // bonded<> naming a subset of the standing owned set: violation,
        // empty view.
        violation_scope guard;
        auto v = w.bonded<Pos, Vel>();
        CHECK(violations_seen == 1);
        CHECK(v.count() == 0);
    }
    {
        // bond while a prospective owner iterates: refused.
        ecs::world w2;
        w2.spawn(Pos{1}, Vel{2}, Hp{3});
        violation_scope guard;
        w2.select<Pos>().each(
            [&](ecs::entity, Pos&)
            {
                auto v = w2.bond<Pos, Vel, Hp>();
                CHECK(v.count() == 0);
                return false;
            });
        CHECK(violations_seen == 1);
    }
    {
        // validate() detects an injected N-ary mirror break in ONE owner.
        ecs::world w3;
        w3.spawn(Pos{1}, Vel{1}, Hp{1});
        w3.spawn(Pos{2}, Vel{2}, Hp{2});
        w3.bond<Pos, Vel, Hp>();
        CHECK_VALID(w3);
        ecs::test_access::corrupt_bond<Vel>(w3);
        CHECK(!w3.validate().has_value());
    }
    CHECK_VALID(w);
    // Compile-time walls, for the record (each is a static_assert):
    //   w.bond<Pos, Pos, Vel>();              // duplicate component type
    //   w.bond<Pos>();                        // needs at least two types
    //   w.bond<T1, T2, T3, T4, T5, T6, T7, T8, T9>();  // at most 8 owners
}
#endif

void test_bond_n_ary_unbond()
{
    section("bond<A, B, C>: unbond tombstones; exact-set naming");
    ecs::world w;
    w.spawn(Pos{1}, Vel{2}, Hp{3});
    auto view = w.bond<Pos, Vel, Hp>();
    CHECK(view.count() == 1);

    CHECK(!(w.unbond<Pos, Vel>()));      // a subset does not name the group
    CHECK((w.unbond<Hp, Pos, Vel>()));   // any ORDER of the full set does
    CHECK(view.count() == 0);            // stored views degrade to empty
    CHECK(!(w.unbond<Pos, Vel, Hp>()));  // nothing standing anymore

    // Owners are free again: a different partition over one of them builds.
    auto pair = w.bond<Pos, TagA>();
    CHECK(pair.count() == 0);
    w.add<TagA>(w.select<Pos>().first());
    CHECK(pair.count() == 1);

    // Re-bonding the trio builds a fresh partition; the OLD view stays empty.
    CHECK((w.unbond<Pos, TagA>()));
    auto fresh = w.bond<Pos, Vel, Hp>();
    CHECK(fresh.count() == 1);
    CHECK(view.count() == 0);
    CHECK_VALID(w);
}
