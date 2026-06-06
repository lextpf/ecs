// ============================================================================
// tests_reactive.cpp — watcher condition-set monitors (M5): enter edges,
// eviction (live truth), changed<> triggers, lifetime, and lock violations.
// ============================================================================

#include "test_harness.hpp"

struct Burning
{
    int heat = 0;
};

struct Shielded
{
};

void test_watcher_entered()
{
    section("watcher: enter edges and live-truth eviction");
    ecs::world w;

    ecs::watcher<ecs::types<Burning, Hp>> entered(w);

    // Gaining the LAST missing include inserts; partial sets do not.
    const ecs::entity e = w.spawn(Hp{10});
    CHECK(entered.count() == 0);
    w.add<Burning>(e, Burning{5});
    CHECK(entered.count() == 1);
    CHECK(entered.contains(e));

    // Losing any include evicts — matched() is live truth.
    w.remove<Hp>(e);
    CHECK(entered.count() == 0);
    CHECK(!entered.contains(e));
    w.add<Hp>(e, Hp{20});  // re-enters through the other include
    CHECK(entered.count() == 1);

    // Dedup: repeated edges keep one entry.
    w.remove<Burning>(e);
    w.add<Burning>(e, Burning{6});
    CHECK(entered.count() == 1);

    // Drain empties; a STILL-matching entity is not re-collected without a
    // fresh edge.
    entered.clear();
    CHECK(entered.count() == 0);
    CHECK((w.has_all<Burning, Hp>(e)));
    w.remove<Burning>(e);
    w.add<Burning>(e, Burning{7});  // a fresh edge re-collects
    CHECK(entered.count() == 1);

    // kill evicts naturally (include hooks fire on the way out).
    w.kill(e);
    CHECK(entered.count() == 0);

    // Pre-existing members do NOT enter at construction: watchers see edges.
    const ecs::entity old = w.spawn(Hp{1});
    w.add<Burning>(old, Burning{1});
    ecs::watcher<ecs::types<Burning, Hp>> late(w);
    CHECK(late.count() == 0);

    // except<> conditions: adding the exclude evicts, removing it (while the
    // includes hold) inserts.
    ecs::watcher<ecs::types<Burning>, ecs::except<Shielded>> guarded(w);
    CHECK(guarded.count() == 0);  // `old` predates the watcher
    w.add<Shielded>(old);
    CHECK(guarded.count() == 0);
    w.remove<Shielded>(old);  // un-shielding IS the enter edge
    CHECK(guarded.count() == 1);
    CHECK(guarded.contains(old));
    w.add<Shielded>(old);  // shielding evicts
    CHECK(guarded.count() == 0);

    const ecs::entity f = w.spawn();
    w.add<Burning>(f, Burning{2});
    CHECK(guarded.count() == 1);

    CHECK_VALID(w);
}

void test_watcher_changed()
{
    section("watcher: changed<> triggers fire only while matching");
    ecs::world w;

    ecs::watcher<ecs::types<Burning>, ecs::changed<Hp>> hurt_while_burning(w);

    const ecs::entity e = w.spawn(Burning{1}, Hp{100});
    CHECK(hurt_while_burning.count() == 1);  // entered via the Burning edge
    hurt_while_burning.clear();

    // replace / put / amend all trigger; a plain reference write does not.
    w.replace<Hp>(e, Hp{90});
    CHECK(hurt_while_burning.count() == 1);
    hurt_while_burning.clear();
    w.put<Hp>(e, Hp{80});
    CHECK(hurt_while_burning.count() == 1);
    hurt_while_burning.clear();
    w.amend<Hp>(e, [](Hp& hp) { hp.hp -= 10; });
    CHECK(hurt_while_burning.count() == 1);
    hurt_while_burning.clear();
    w.get<Hp>(e).hp = 5;  // invisible by design
    CHECK(hurt_while_burning.count() == 0);

    // A non-matching entity's change is ignored.
    const ecs::entity cold = w.spawn(Hp{50});
    w.amend<Hp>(cold, [](Hp& hp) { hp.hp = 1; });
    CHECK(hurt_while_burning.count() == 0);

    // Breaking the condition evicts even a trigger-collected entry.
    w.amend<Hp>(e, [](Hp& hp) { hp.hp = 4; });
    CHECK(hurt_while_burning.count() == 1);
    w.remove<Burning>(e);
    CHECK(hurt_while_burning.count() == 0);

    // A changed<> type may also be a condition.
    ecs::watcher<ecs::types<Hp>, ecs::changed<Hp>> hp_watch(w);
    const ecs::entity g = w.spawn(Hp{7});
    CHECK(hp_watch.count() == 1);
    hp_watch.clear();
    w.amend<Hp>(g, [](Hp& hp) { ++hp.hp; });
    CHECK(hp_watch.count() == 1);

    CHECK_VALID(w);
}

void test_watcher_lifetime()
{
    section("watcher: reset, teardown, and span stability");
    ecs::world w;

    auto* watcher_box = new ecs::watcher<ecs::types<Burning>>(w);
    const ecs::entity e = w.spawn(Burning{1});
    CHECK(watcher_box->count() == 1);

    // reset() fires on_remove per live component: everything evicts, the
    // connections survive (empty-but-connected, like tracker hooks).
    w.reset();
    CHECK(watcher_box->count() == 0);
    const ecs::entity f = w.spawn(Burning{2});
    CHECK(watcher_box->count() == 1);
    CHECK(watcher_box->contains(f));
    CHECK(!watcher_box->contains(e));  // dead handle: not collected

    // matched() spans read the live set directly.
    int total = 0;
    for (const ecs::entity m : watcher_box->matched())
    {
        total += w.get<Burning>(m).heat;
    }
    CHECK(total == 2);

    // Destruction disconnects: later edges touch nothing.
    delete watcher_box;
    w.add<Hp>(f, Hp{1});
    w.remove<Burning>(f);
    w.add<Burning>(f, Burning{3});
    CHECK_VALID(w);
}

#if QUIVER_CHECKS
void test_watcher_violations()
{
    section("watcher: hook connect/disconnect rules apply");
    ecs::world w;
    w.spawn(Burning{1});

    // Constructing a watcher connects hooks — refused while a named pool
    // iterates, leaving the watcher inert but safe to destroy.
    violation_scope guard;
    w.select<Burning>().each(
        [&](ecs::entity, Burning&)
        {
            const ecs::watcher<ecs::types<Burning>> mid_loop(w);
            CHECK(mid_loop.count() == 0);
            return false;
        });
    CHECK(violations_seen >= 1);
    CHECK_VALID(w);
}
#endif
