// ============================================================================
// tests.cpp — quiver's test suite. No framework: a CHECK macro, a counter, and
// an exit code. Build with QUIVER_CHECKS on (the default without NDEBUG); the
// violation-handler tests are skipped when checks are compiled out.
// ============================================================================

#include "test_harness.hpp"

#include <thread>
#include <unordered_set>

// ---------------------------------------------------------- test components

// Empty but explicitly packed: storage_policy beats the empty-type auto-tag.
struct EmptyPacked
{
    static constexpr auto quiver_storage = ecs::storage::packed;
};

struct MoveOnly
{
    explicit MoveOnly(int v)
        : box(std::make_unique<int>(v))
    {
    }

    std::unique_ptr<int> box;
};

// Knows its own address; its move constructor records whether the source was
// where it claimed to be. If the command buffer's arena ever relocated a
// payload behind its back, sources would lie.
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

// Non-movable component (std::atomic member): legal in stable storage, which
// constructs and destroys in place and never moves payloads.
struct Pinned
{
    std::atomic<int> charge{0};
    static constexpr auto quiver_storage = ecs::storage::stable;
};
static_assert(!std::is_move_constructible_v<Pinned>);

// Payload aligned beyond the command buffer's bump-arena alignment: must get
// a dedicated, exactly-aligned chunk.
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

// Heap-owning, copyable payload for aliasing/duplication regressions.
struct Label
{
    std::string value;
};

// Archive round-trip component with an entity member, relinked via the
// member hook.
struct Squad
{
    ecs::entity leader;
    int morale = 0;

    void quiver_relink(const ecs::graft_map& map) { leader = map.resolve(leader); }
};

// A "foreign" type that opts into relinking via the trait instead.
struct ForeignLink
{
    ecs::entity target;
};

template <>
struct quiver::relink_traits<ForeignLink>
{
    static constexpr bool links = true;

    static void relink(ForeignLink& value, const quiver::graft_map& map)
    {
        value.target = map.resolve(value.target);
    }
};

// System object for member-function hooks.
struct GpuUploads
{
    int uploaded = 0;
    int freed = 0;

    void upload(ecs::world&, ecs::entity) { ++uploaded; }
    void free_one(ecs::entity) { ++freed; }
};

// --- genericity-wave fixtures ---

// A component with a CUSTOM storage backend: derives the built-in packed
// pool and instruments the cold virtuals.
struct Telemetry
{
    int v = 0;
};

struct telemetry_pool : ecs::packed_pool_of<Telemetry>
{
    using base = ecs::packed_pool_of<Telemetry>;
    using base::base;  // (std::pmr::memory_resource*)

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

// Pinned identity via member label.
struct LabeledA
{
    int v = 0;
    static constexpr std::string_view quiver_label = "test.labeled_a";
};

// Pinned identity for a "foreign" type via the variable template.
struct ForeignVec
{
    float x = 0;
};

// Two distinct types sharing a label: must trip the collision check.
struct DupLabel1
{
    int v = 0;
    static constexpr std::string_view quiver_label = "test.duplicate";
};

struct DupLabel2
{
    int v = 0;
    static constexpr std::string_view quiver_label = "test.duplicate";
};

template <>
struct quiver::pool_of<Telemetry>
{
    using type = telemetry_pool;
};

// A custom STABLE-derived backend (the seam's other families work too).
struct Audit
{
    int v = 0;
    static constexpr auto quiver_storage = ecs::storage::stable;
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
struct quiver::pool_of<Audit>
{
    using type = audit_pool;
};

template <>
inline constexpr std::string_view quiver::component_label<ForeignVec> = "test.foreign_vec";

namespace hookfree
{
inline int free_hits = 0;
inline void on_free_fn(ecs::world&, ecs::entity)
{
    ++free_hits;
}
inline void on_free_short(ecs::entity)
{
    ++free_hits;
}
}  // namespace hookfree

// Stable storage with a tuned chunk size (local classes cannot host the
// static members).
struct TunedStable
{
    int v = 0;
    static constexpr auto quiver_storage = ecs::storage::stable;
    static constexpr std::size_t quiver_chunk_items = 8;
};

// Distinct types for the multi-world registration-order test.
struct OnlyHere1
{
    int a = 0;
};

struct OnlyHere2
{
    int b = 0;
};

// ------------------------------------------------------------------- tests

static void test_entity_basics()
{
    section("entity basics");
    CHECK(ecs::entity{} == ecs::no_entity);
    CHECK(!ecs::no_entity);
    CHECK(ecs::no_entity.bits() == (std::uint64_t{0xFFFFFFFFu} << 32));

    ecs::world w;
    const ecs::entity first = w.spawn();
    CHECK(static_cast<bool>(first));
    CHECK(first != ecs::no_entity);
    CHECK(!w.alive(ecs::no_entity));
    CHECK(w.alive(first));
    CHECK(first.index() == 0 && first.generation() == 0);  // slot 0 must not alias null

    std::unordered_set<ecs::entity> set;
    set.insert(first);
    set.insert(ecs::no_entity);
    CHECK(set.size() == 2);
    CHECK_VALID(w);
}

static void test_lifecycle_and_reuse()
{
    section("lifecycle, recycling, stale handles");
    ecs::world w;
    const ecs::entity a = w.spawn();
    const ecs::entity b = w.spawn();
    CHECK(w.live_count() == 2);
    CHECK(w.slot_count() == 2);

    w.kill(a);
    CHECK(!w.alive(a));
    CHECK(w.alive(b));
    CHECK(w.live_count() == 1);

    const ecs::entity reused = w.spawn();
    CHECK(reused.index() == a.index());                // slot recycled...
    CHECK(reused.generation() == a.generation() + 1);  // ...at a new generation
    CHECK(!w.alive(a));                                // the stale handle stays dead
    CHECK(w.alive(reused));

    CHECK(w.current_handle(reused.index()) == reused);
    CHECK(w.current_handle(9999) == ecs::no_entity);

    // Components attached through a stale handle must not leak onto the
    // recycled entity. (has/find are the safe, non-violating queries.)
    CHECK(!w.has<Pos>(a));
    CHECK(w.find<Pos>(a) == nullptr);

    w.reserve_entities(100);
    CHECK(w.capacity() >= 100);
    CHECK_VALID(w);
}

static void test_component_basics()
{
    section("add / get / find / remove / put / replace");
    ecs::world w;
    const ecs::entity e = w.spawn();

    Pos& p = w.add<Pos>(e, Pos{7});
    CHECK(p.x == 7);
    CHECK(w.has<Pos>(e));
    CHECK(w.get<Pos>(e).x == 7);
    CHECK(w.find<Pos>(e) == &p);

    // replace: in place, pointer-stable, requires presence
    Pos* before = w.find<Pos>(e);
    Pos& replaced = w.replace<Pos>(e, Pos{8});
    CHECK(&replaced == before);
    CHECK(w.get<Pos>(e).x == 8);

    // put: replace when present...
    w.put<Pos>(e, Pos{9});
    CHECK(w.get<Pos>(e).x == 9);
    // ...add when absent
    w.put<Vel>(e, Vel{3});
    CHECK(w.get<Vel>(e).v == 3);

    CHECK(w.remove<Pos>(e));
    CHECK(!w.has<Pos>(e));
    CHECK(!w.remove<Pos>(e));  // removing what is not there reports false

    const ecs::world& cw = w;
    CHECK(cw.get<Vel>(e).v == 3);
    CHECK(cw.find<Vel>(e) != nullptr);
    CHECK_VALID(w);
}

static void test_destructor_balance_packed()
{
    section("destructor balance (packed)");
    Counted::total_ctors = 0;
    Counted::total_dtors = 0;
    {
        ecs::world w;
        std::vector<ecs::entity> es;
        for (int i = 0; i < 64; ++i)
        {
            es.push_back(w.spawn(Counted{i}));
        }
        for (int i = 0; i < 64; i += 2)
        {
            w.remove<Counted>(es[i]);  // swap-remove path
        }
        for (int i = 1; i < 64; i += 4)
        {
            w.kill(es[i]);  // kill path
        }
        w.put<Counted>(es[3], Counted{99});  // replace path
        w.purge<Counted>();                  // pool wipe path
        for (int i = 0; i < 8; ++i)
        {
            w.spawn(Counted{i});
        }
        w.reset();  // world reset path
        for (int i = 0; i < 8; ++i)
        {
            w.spawn(Counted{i});
        }
        CHECK_VALID(w);
    }  // world destruction path
    CHECK(Counted::live == 0);
    CHECK(Counted::total_ctors == Counted::total_dtors);
}

static void test_tags()
{
    section("tag components");
    static_assert(ecs::storage_policy<TagA> == ecs::storage::tag);
    static_assert(ecs::storage_policy<EmptyPacked> == ecs::storage::packed);

    ecs::world w;
    const ecs::entity a = w.spawn(Pos{1});
    const ecs::entity b = w.spawn(Pos{2});
    w.add<TagA>(a);

    CHECK(w.has<TagA>(a));
    CHECK(!w.has<TagA>(b));

    // Tags filter but are not passed to callbacks.
    int visited = 0;
    w.each<Pos, TagA>(
        [&](ecs::entity e, Pos& pos)
        {
            ++visited;
            CHECK(e == a);
            CHECK(pos.x == 1);
        });
    CHECK(visited == 1);

    // A selection of only a tag passes just the entity.
    visited = 0;
    w.each<TagA>(
        [&](ecs::entity e)
        {
            ++visited;
            CHECK(e == a);
        });
    CHECK(visited == 1);

    CHECK(w.remove<TagA>(a));
    CHECK(!w.has<TagA>(a));

    // Explicit policy beats the empty-type auto-tag: EmptyPacked has a real
    // (1-byte) pool and value access works.
    w.add<EmptyPacked>(a);
    CHECK(w.find<EmptyPacked>(a) != nullptr);
    CHECK_VALID(w);
}

static void test_move_only()
{
    section("move-only components");
    ecs::world w;
    const ecs::entity e = w.spawn();
    w.add<MoveOnly>(e, MoveOnly{41});
    CHECK(*w.get<MoveOnly>(e).box == 41);

    // swap-remove with a move-only payload
    const ecs::entity e2 = w.spawn();
    w.add<MoveOnly>(e2, MoveOnly{42});
    CHECK(w.remove<MoveOnly>(e));
    CHECK(*w.get<MoveOnly>(e2).box == 42);

    // through the command buffer
    ecs::command_buffer cmd;
    cmd.add<MoveOnly>(e, MoveOnly{43});
    const ecs::apply_result r = w.apply(cmd);
    CHECK(r.applied == 1);
    CHECK(*w.get<MoveOnly>(e).box == 43);
    CHECK_VALID(w);
}

static void test_stable_storage()
{
    section("stable storage: pointer stability and slot reuse");
    static_assert(ecs::storage_policy<Stable> == ecs::storage::stable);

    ecs::world w;
    std::vector<ecs::entity> es;
    std::vector<const Stable*> pointers;
    for (int i = 0; i < 1000; ++i)
    {
        const ecs::entity e = w.spawn();
        pointers.push_back(&w.add<Stable>(e, Stable{i}));
        es.push_back(e);
    }
    // Heavy churn: remove every other one, then add a fresh thousand.
    for (int i = 0; i < 1000; i += 2)
    {
        w.remove<Stable>(es[i]);
    }
    for (int i = 0; i < 1000; ++i)
    {
        w.add<Stable>(w.spawn(), Stable{10000 + i});
    }
    // Survivors must still be exactly where they were, with their values.
    for (int i = 1; i < 1000; i += 2)
    {
        CHECK(w.find<Stable>(es[i]) == pointers[i]);
        CHECK(pointers[i]->value == i);
    }
    CHECK_VALID(w);

    // Destructor balance through every stable-pool teardown path.
    Counted::total_ctors = 0;
    Counted::total_dtors = 0;
    {
        ecs::world sw;
        std::vector<ecs::entity> ses;
        for (int i = 0; i < 100; ++i)
        {
            const ecs::entity e = sw.spawn();
            sw.add<StableCounted>(e, StableCounted{i});
            ses.push_back(e);
        }
        for (int i = 0; i < 50; ++i)
        {
            sw.remove<StableCounted>(ses[i]);  // slot freed
        }
        for (int i = 0; i < 25; ++i)
        {
            sw.add<StableCounted>(sw.spawn(), StableCounted{i});  // slot reused
        }
        sw.kill(ses[60]);
        sw.purge<StableCounted>();
        sw.add<StableCounted>(sw.spawn(), StableCounted{1});
        sw.reset();
        sw.add<StableCounted>(sw.spawn(), StableCounted{2});
        CHECK_VALID(sw);
    }  // world destruction
    CHECK(Counted::live == 0);
    CHECK(Counted::total_ctors == Counted::total_dtors);
}

static void test_iteration()
{
    section("iteration: arity, constness, early exit");
    ecs::world w;
    const ecs::entity a = w.spawn(Pos{1}, Vel{10});
    const ecs::entity b = w.spawn(Pos{2}, Vel{20});
    const ecs::entity c = w.spawn(Pos{3});
    (void)c;

    // one / two / three component iteration, with and without the entity
    int sum = 0;
    w.each<Pos>([&](Pos& p) { sum += p.x; });
    CHECK(sum == 6);

    sum = 0;
    w.each<Pos, Vel>([&](ecs::entity, Pos& p, Vel& v) { sum += p.x * v.v; });
    CHECK(sum == 50);

    w.add<Hp>(a, Hp{5});
    w.add<Hp>(b, Hp{6});
    sum = 0;
    w.each<Pos, Vel, Hp>([&](Pos& p, Vel& v, Hp& h) { sum += p.x + v.v + h.hp; });
    CHECK(sum == 44);

    // Entity-first wins when a callback could bind both ways.
    std::size_t generic_arity = 0;
    w.each<Pos, Vel>(
        [&](auto&&... args)
        {
            (..., static_cast<void>(args));
            generic_arity = sizeof...(args);
        });
    CHECK(generic_arity == 3);

    // Mixed constness: write Pos, read Vel.
    w.each<Pos, const Vel>([](Pos& p, const Vel& v) { p.x += v.v; });
    CHECK(w.get<Pos>(a).x == 11);
    CHECK(w.get<Pos>(b).x == 22);

    // A const world iterates const.
    const ecs::world& cw = w;
    sum = 0;
    cw.each<Pos>([&](const Pos& p) { sum += p.x; });
    CHECK(sum == 36);

    // Early exit: returning false stops the loop.
    int visits = 0;
    w.each<Pos>(
        [&](ecs::entity, const Pos&)
        {
            ++visits;
            return false;
        });
    CHECK(visits == 1);

    // entities(): id-only iteration with the same rules.
    visits = 0;
    auto sel = w.select<Pos, Vel>();
    sel.entities(
        [&](ecs::entity e)
        {
            ++visits;
            CHECK((e == a || e == b));
        });
    CHECK(visits == 2);
    CHECK_VALID(w);
}

static void test_filters_and_driver()
{
    section("include/exclude filters, smallest-pool driving");
    ecs::world w;

    // A misleading population: Pos is the big pool, Vel is smallest, and some
    // Vel holders are disqualified (no Pos, or excluded by TagB).
    std::vector<ecs::entity> expected;
    for (int i = 0; i < 50; ++i)
    {
        w.spawn(Pos{i});  // Pos only
    }
    const ecs::entity v_only = w.spawn();
    w.add<Vel>(v_only, Vel{1});  // Vel but no Pos -> never matches
    for (int i = 0; i < 5; ++i)
    {
        const ecs::entity e = w.spawn(Pos{100 + i}, Vel{i});
        if (i % 2 == 0)
        {
            w.add<TagB>(e);  // excluded
        }
        else
        {
            expected.push_back(e);
        }
    }

    auto sel = w.select<Pos, Vel>(ecs::except<TagB>{});

    // each / entities / count / empty / contains must agree on the exact set.
    std::vector<ecs::entity> from_each;
    sel.each([&](ecs::entity e, Pos&, Vel&) { from_each.push_back(e); });
    std::vector<ecs::entity> from_entities;
    sel.entities([&](ecs::entity e) { from_entities.push_back(e); });

    CHECK(from_each == from_entities);
    CHECK(from_each.size() == expected.size());
    CHECK(sel.count() == expected.size());
    CHECK(!sel.empty());
    for (const ecs::entity e : expected)
    {
        CHECK(sel.contains(e));
    }
    CHECK(!sel.contains(v_only));

    // Driving from the smallest pool means iteration order follows Vel's dense
    // order, not Pos's.
    std::vector<ecs::entity> vel_order;
    w.select<Vel>().entities(
        [&](ecs::entity e)
        {
            if (sel.contains(e))
            {
                vel_order.push_back(e);
            }
        });
    CHECK(from_each == vel_order);

    // count() on the single-pool selection is O(1) pool size.
    CHECK(w.select<Vel>().count() == 6);

    // Exclude-only-difference: without the filter the tagged ones reappear.
    CHECK((w.select<Pos, Vel>().count() == 5));
    CHECK_VALID(w);
}

static void test_selection_reuse_and_const_world()
{
    section("stored selections, const-world select, reset survival");
    ecs::world w;
    auto movers = w.select<Pos, Vel>();  // stored "compiled query"
    CHECK(movers.count() == 0);

    const ecs::entity e = w.spawn(Pos{1}, Vel{2});
    CHECK(movers.count() == 1);  // same selection sees later additions

    // A const world never registers pools: unseen component => empty selection.
    const ecs::world cold;
    auto cold_sel = cold.select<Pos>();
    CHECK(cold_sel.count() == 0);
    CHECK(cold_sel.empty());
    int cold_visits = 0;
    cold_sel.each([&](const Pos&) { ++cold_visits; });
    CHECK(cold_visits == 0);

    // reset() empties pools but never destroys them: stored selections stay
    // usable and see the rebuilt world.
    w.reset();
    CHECK(movers.count() == 0);
    CHECK(!w.alive(e));
    w.spawn(Pos{1}, Vel{2});
    CHECK(movers.count() == 1);
    CHECK_VALID(w);
}

static void test_command_buffer_basics()
{
    section("command buffer: order, dead-target skipping, reuse");
    ecs::world w;
    const ecs::entity a = w.spawn(Pos{1});
    const ecs::entity b = w.spawn(Pos{2}, Hp{10});

    ecs::command_buffer cmd;

    // Strict recording order: add then remove leaves the component absent;
    // remove then add leaves it present.
    cmd.add<Vel>(a, Vel{5});
    cmd.remove<Vel>(a);
    cmd.remove<Hp>(b);
    cmd.add<Hp>(b, Hp{20});
    ecs::apply_result r = w.apply(cmd);
    CHECK(r.applied == 4 && r.skipped == 0);
    CHECK(!w.has<Vel>(a));
    CHECK(w.get<Hp>(b).hp == 20);

    // Buffer is reusable after apply.
    cmd.put<Pos>(a, Pos{7});
    cmd.kill(b);
    r = w.apply(cmd);
    CHECK(r.applied == 2);
    CHECK(w.get<Pos>(a).x == 7);
    CHECK(!w.alive(b));

    // Ops aimed at an entity killed outside the buffer are skipped even when
    // the slot has been recycled: the newcomer must not be touched.
    cmd.add<Vel>(a, Vel{9});
    cmd.kill(a);
    w.kill(a);  // killed externally between record and apply
    const ecs::entity recycled = w.spawn();
    CHECK(recycled.index() == a.index());  // proves the slot really was reused
    r = w.apply(cmd);
    CHECK(r.applied == 0 && r.skipped == 2);
    CHECK(!w.has<Vel>(recycled));
    CHECK(w.alive(recycled));

    // kill recorded twice: second one is a skip, not an error.
    const ecs::entity c = w.spawn();
    cmd.kill(c);
    cmd.kill(c);
    r = w.apply(cmd);
    CHECK(r.applied == 1 && r.skipped == 1);
    CHECK_VALID(w);
}

static void test_command_buffer_spawn()
{
    section("command buffer: deferred spawn with provisional handles");
    ecs::world w;
    ecs::command_buffer cmd;

    const ecs::entity ghost = cmd.spawn();
    CHECK(!w.alive(ghost));  // provisional handles are not live entities
    cmd.add<Pos>(ghost, Pos{77});
    cmd.add<TagA>(ghost);
    const ecs::entity ghost2 = cmd.spawn();
    cmd.add<Pos>(ghost2, Pos{78});
    cmd.kill(ghost2);  // even a deferred spawn can be deferred-killed

    const ecs::apply_result r = w.apply(cmd);
    // Order is spawn, add, add, spawn, add, kill: ghost2 is alive when its add
    // and its kill run, so every op applies and nothing is skipped.
    CHECK(r.applied == 6 && r.skipped == 0);

    CHECK(w.live_count() == 1);
    int found = 0;
    w.each<Pos, TagA>(
        [&](ecs::entity, Pos& p)
        {
            ++found;
            CHECK(p.x == 77);
        });
    CHECK(found == 1);
    CHECK_VALID(w);
}

static void test_command_buffer_payloads()
{
    section("command buffer: arena growth, alignment, payload destruction");
    anchored_betrayals = 0;
    misaligned_payloads = 0;
    Counted::total_ctors = 0;
    Counted::total_dtors = 0;

    ecs::world w;
    std::vector<ecs::entity> targets;
    for (int i = 0; i < 400; ++i)
    {
        targets.push_back(w.spawn());
    }

    {
        ecs::command_buffer cmd;
        // Hundreds of mixed-size payloads force several arena chunks. Anchored
        // proves no payload was ever relocated; Aligned proves alignment.
        for (int i = 0; i < 400; ++i)
        {
            cmd.add<Anchored>(targets[i]);
            if (i % 3 == 0)
            {
                cmd.add<Aligned>(targets[i]);
            }
            cmd.add<Counted>(targets[i], Counted{i});
        }
        // Kill a slice of the targets so some payload ops get skipped: their
        // payloads must still be destroyed exactly once.
        for (int i = 100; i < 200; ++i)
        {
            w.kill(targets[i]);
        }
        const ecs::apply_result r = w.apply(cmd);
        CHECK(r.skipped > 0);
        CHECK(r.applied > 0);
    }

    {
        // A buffer that records but is never applied must destroy its payloads
        // on destruction.
        ecs::command_buffer never_applied;
        never_applied.add<Counted>(targets[0], Counted{1});
        never_applied.add<Counted>(targets[1], Counted{2});
    }

    CHECK(anchored_betrayals == 0);
    CHECK(misaligned_payloads == 0);
    w.reset();
    CHECK(Counted::live == 0);
    CHECK(Counted::total_ctors == Counted::total_dtors);
    CHECK_VALID(w);
}

#if QUIVER_CHECKS
static void test_violations()
{
    section("violation handler: structural changes during iteration");
    ecs::world w;
    const ecs::entity a = w.spawn(Pos{1});
    const ecs::entity b = w.spawn(Pos{2});
    w.reserve<Pos>(64);

    // kill of an iterated-pool member: refused, entity survives.
    {
        violation_scope guard;
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                w.kill(a);
                return false;
            });
        CHECK(violations_seen == 1);
        CHECK(w.alive(a));
    }

    // remove from the iterated pool: refused.
    {
        violation_scope guard;
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                CHECK(!w.remove<Pos>(b));
                return false;
            });
        CHECK(violations_seen == 1);
        CHECK(w.has<Pos>(b));
    }

    // add to the iterated pool: reported, then proceeds (it must return a
    // reference). We stop iterating immediately and touch nothing stale.
    {
        violation_scope guard;
        const ecs::entity fresh = w.spawn();
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                w.add<Pos>(fresh, Pos{50});
                return false;
            });
        CHECK(violations_seen == 1);
        CHECK(w.get<Pos>(fresh).x == 50);
        w.remove<Pos>(fresh);
    }

    // Variadic spawn writes pools: reported when one of them is iterated.
    {
        violation_scope guard;
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                w.spawn(Pos{60});
                return false;
            });
        CHECK(violations_seen == 1);
    }

    // Bare spawn is the documented exception: always legal, no violation.
    {
        violation_scope guard;
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                const ecs::entity quiet = w.spawn();
                CHECK(w.alive(quiet));
                return false;
            });
        CHECK(violations_seen == 0);
    }

    // apply during iteration: refused before any mutation, buffer intact.
    {
        violation_scope guard;
        ecs::command_buffer cmd;
        cmd.add<Vel>(a, Vel{1});
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                const ecs::apply_result r = w.apply(cmd);
                CHECK(r.applied == 0 && r.skipped == 0);
                return false;
            });
        CHECK(violations_seen == 1);
        CHECK(!w.has<Vel>(a));
        const ecs::apply_result r = w.apply(cmd);  // works once iteration ended
        CHECK(r.applied == 1);
        CHECK(w.has<Vel>(a));
    }

    // purge / reset / shrink during iteration: refused.
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
        CHECK(violations_seen == 3);
        CHECK(w.alive(a));
        CHECK(w.has<Pos>(a));
    }

    // Structural changes to pools OUTSIDE the running iteration are legal.
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
        CHECK(violations_seen == 0);
    }

    // Stale-handle misuse is reported (and refused).
    {
        violation_scope guard;
        const ecs::entity victim = w.spawn();
        w.kill(victim);
        w.kill(victim);  // stale kill
        CHECK(violations_seen == 1);
        w.remove<Pos>(victim);  // stale remove
        CHECK(violations_seen == 2);
        CHECK(!w.has<Pos>(victim));  // safe queries stay silent
        CHECK(w.find<Pos>(victim) == nullptr);
        CHECK(violations_seen == 2);
    }

    // Iteration locks unwind even on early exit.
    w.each<Pos>([](ecs::entity, Pos&) { return false; });
    CHECK(ecs::test_access::lock_count<Pos>(w) == 0);
    CHECK_VALID(w);
}

static void test_negative_validation()
{
    section("validate() detects manufactured corruption");
    {
        ecs::world w;
        const ecs::entity e = w.spawn(Pos{1});
        CHECK_VALID(w);
        ecs::test_access::corrupt_sparse<Pos>(w, e);
        const auto r = w.validate();
        CHECK(!r.has_value());
        if (!r)
        {
            CHECK(r.error().code == ecs::fault_code::sparse_dense_desync);
            CHECK(r.error().pool == ecs::name_of<Pos>());
        }
    }
    {
        ecs::world w;
        const ecs::entity e = w.spawn(Pos{1});
        ecs::test_access::corrupt_generation(w, e);
        const auto r = w.validate();
        CHECK(!r.has_value());
        if (!r)
        {
            CHECK(r.error().code == ecs::fault_code::dense_entity_dead);
        }
    }
}
#endif  // QUIVER_CHECKS

static void test_relationships()
{
    section("relationships: adopt, orphan, kill-time link surgery");
    ecs::world w;
    const ecs::entity parent = w.spawn();
    const ecs::entity c1 = w.spawn();
    const ecs::entity c2 = w.spawn();
    const ecs::entity c3 = w.spawn();

    w.adopt(parent, c1);
    w.adopt(parent, c2);
    w.adopt(parent, c3);
    CHECK(w.parent_of(c1) == parent);
    CHECK(w.parent_of(parent) == ecs::no_entity);

    // Enumeration is newest-first.
    std::vector<ecs::entity> kids;
    w.children_of(parent, [&](ecs::entity c) { kids.push_back(c); });
    CHECK((kids == std::vector<ecs::entity>{c3, c2, c1}));

    // Early exit works here too.
    kids.clear();
    w.children_of(parent,
                  [&](ecs::entity c)
                  {
                      kids.push_back(c);
                      return false;
                  });
    CHECK(kids.size() == 1);

    // Killing the middle sibling keeps the chain intact.
    w.kill(c2);
    kids.clear();
    w.children_of(parent, [&](ecs::entity c) { kids.push_back(c); });
    CHECK((kids == std::vector<ecs::entity>{c3, c1}));
    CHECK_VALID(w);

    // Re-parenting moves the child between lists.
    const ecs::entity foster = w.spawn();
    w.adopt(foster, c1);
    CHECK(w.parent_of(c1) == foster);
    kids.clear();
    w.children_of(parent, [&](ecs::entity c) { kids.push_back(c); });
    CHECK((kids == std::vector<ecs::entity>{c3}));

    // orphan() detaches without destroying.
    w.orphan(c3);
    CHECK(w.parent_of(c3) == ecs::no_entity);
    CHECK(w.alive(c3));

    // Killing a parent orphans the children; nothing cascades.
    w.adopt(parent, c3);
    w.kill(parent);
    CHECK(w.alive(c3));
    CHECK(w.parent_of(c3) == ecs::no_entity);
    CHECK_VALID(w);

#if QUIVER_CHECKS
    // Self-adoption and cycles are refused.
    {
        violation_scope guard;
        w.adopt(c3, c3);
        CHECK(violations_seen == 1);
        const ecs::entity x = w.spawn();
        const ecs::entity y = w.spawn();
        w.adopt(x, y);
        w.adopt(y, x);  // would make x its own ancestor
        CHECK(violations_seen == 2);
        CHECK(w.parent_of(x) == ecs::no_entity);

        // Link edits while children_of is running are refused.
        w.children_of(x,
                      [&](ecs::entity child)
                      {
                          w.orphan(child);
                          return false;
                      });
        CHECK(violations_seen == 3);
        CHECK(w.parent_of(y) == x);
    }
    CHECK_VALID(w);
#endif
}

static void test_snapshot()
{
    section("snapshot hooks: ordered walk, restore, round trip");
    ecs::world source;

    // Build a world whose handles have history (non-zero generations).
    std::vector<ecs::entity> es;
    for (int i = 0; i < 10; ++i)
    {
        es.push_back(source.spawn(Pos{i}));
    }
    source.kill(es[3]);
    source.kill(es[6]);
    es[3] = source.spawn(Pos{33});  // recycled slots, generation 1
    es[6] = source.spawn(Pos{66});
    source.add<TagA>(es[6]);

    // live_entities walks in slot order.
    std::vector<ecs::entity> order;
    source.live_entities([&](ecs::entity e) { order.push_back(e); });
    CHECK(order.size() == source.live_count());
    for (std::size_t i = 1; i < order.size(); ++i)
    {
        CHECK(order[i - 1].index() < order[i].index());
    }

    // Early exit.
    int seen = 0;
    source.live_entities(
        [&](ecs::entity)
        {
            ++seen;
            return false;
        });
    CHECK(seen == 1);

    // "Serialize": handles plus their components, via the public API only.
    struct Row
    {
        ecs::entity e;
        Pos pos;
        bool tagged;
    };
    std::vector<Row> saved;
    source.live_entities([&](ecs::entity e)
                         { saved.push_back(Row{e, *source.find<Pos>(e), source.has<TagA>(e)}); });

    // "Load" into a fresh world: identical handles, identical data.
    ecs::world loaded;
    for (const Row& row : saved)
    {
        const auto restored = loaded.restore_entity(row.e);
        CHECK(restored.has_value());
        CHECK(*restored == row.e);
        loaded.add<Pos>(row.e, row.pos);
        if (row.tagged)
        {
            loaded.add<TagA>(row.e);
        }
    }
    CHECK(loaded.live_count() == source.live_count());
    for (const Row& row : saved)
    {
        CHECK(loaded.alive(row.e));
        CHECK(loaded.get<Pos>(row.e).x == row.pos.x);
        CHECK(loaded.has<TagA>(row.e) == row.tagged);
    }
    CHECK_VALID(loaded);

    // The loaded world keeps spawning safely: recycled and fresh slots never
    // collide with restored ones.
    std::unordered_set<std::uint32_t> indices;
    loaded.live_entities([&](ecs::entity e) { indices.insert(e.index()); });
    for (int i = 0; i < 20; ++i)
    {
        const ecs::entity fresh = loaded.spawn();
        CHECK(indices.insert(fresh.index()).second);  // unique slot
    }
    CHECK_VALID(loaded);

    // Restoring into the middle of a live free list.
    ecs::world mid;
    std::vector<ecs::entity> ms;
    for (int i = 0; i < 10; ++i)
    {
        ms.push_back(mid.spawn());
    }
    mid.kill(ms[3]);
    mid.kill(ms[5]);
    mid.kill(ms[7]);  // free stack now 3,5,7 with 5 mid-stack
    const ecs::entity back = ecs::entity{ms[5].index(), ms[5].generation()};
    const auto restored = mid.restore_entity(back);
    CHECK(restored.has_value());
    CHECK(mid.alive(back));
    std::unordered_set<std::uint32_t> live_indices;
    mid.live_entities([&](ecs::entity e) { CHECK(live_indices.insert(e.index()).second); });
    for (int i = 0; i < 5; ++i)
    {
        const ecs::entity fresh = mid.spawn();  // drains the free stack past the stale entry
        CHECK(live_indices.insert(fresh.index()).second);
    }
    CHECK_VALID(mid);

    // Restore beyond the current table size grows the table.
    ecs::world tall;
    const auto high = tall.restore_entity(ecs::entity{100, 5});
    CHECK(high.has_value());
    CHECK(tall.alive(ecs::entity{100, 5}));
    CHECK(tall.slot_count() == 101);
    const ecs::entity filler = tall.spawn();  // grew slots are recyclable
    CHECK(filler.index() < 100);
    CHECK_VALID(tall);

    // Error paths report instead of asserting: this is the load path.
    const auto occupied = tall.restore_entity(ecs::entity{100, 9});
    CHECK(!occupied.has_value());
    CHECK(occupied.error().code == ecs::fault_code::slot_occupied);
    const auto null_restore = tall.restore_entity(ecs::no_entity);
    CHECK(!null_restore.has_value());
    CHECK(null_restore.error().code == ecs::fault_code::bad_handle);
}

static void test_move_semantics()
{
    section("move semantics: buffers, worlds, arenas");

    // A moved-from command buffer behaves like a fresh one — including its
    // provisional ticket counter (regression: defaulted move copied it).
    {
        ecs::world w;
        ecs::command_buffer a;
        const ecs::entity g1 = a.spawn();
        a.add<Pos>(g1, Pos{1});
        a.spawn();
        a.spawn();

        ecs::command_buffer b{std::move(a)};
        // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        // Moved-from reuse is exactly the contract under test here.
        CHECK(a.empty());

        const ecs::entity g2 = a.spawn();  // reuse the moved-from buffer
        a.add<Pos>(g2, Pos{2});
        ecs::apply_result r = w.apply(a);
        CHECK(r.applied == 2 && r.skipped == 0);
        int with_two = 0;
        w.each<Pos>([&](Pos& p) { with_two += p.x == 2 ? 1 : 0; });
        CHECK(with_two == 1);

        r = w.apply(b);  // the moved-to buffer carries the original ops
        // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        CHECK(r.applied == 4 && r.skipped == 0);
        int with_one = 0;
        w.each<Pos>([&](Pos& p) { with_one += p.x == 1 ? 1 : 0; });
        CHECK(with_one == 1);

        // Move assignment with pending payloads on both sides stays balanced.
        Counted::total_ctors = 0;
        Counted::total_dtors = 0;
        {
            ecs::command_buffer lhs;
            ecs::command_buffer rhs;
            lhs.add<Counted>(w.spawn(), Counted{1});
            rhs.add<Counted>(w.spawn(), Counted{2});
            lhs = std::move(rhs);
            const ecs::entity g3 = lhs.spawn();
            lhs.add<Counted>(g3, Counted{3});
            r = w.apply(lhs);
            CHECK(r.applied == 3 && r.skipped == 0);
        }
        w.reset();
        CHECK(Counted::live == 0);
        CHECK(Counted::total_ctors == Counted::total_dtors);
        CHECK_VALID(w);
    }

#if QUIVER_CHECKS
    // Provisional handles from another buffer (or from before a clear) are
    // refused, even when the ticket number happens to be in range.
    {
        violation_scope guard;
        ecs::world w;
        ecs::command_buffer a;
        ecs::command_buffer b;
        const ecs::entity foreign = a.spawn();
        b.kill(foreign);  // wrong buffer
        b.spawn();
        const ecs::apply_result r = w.apply(b);
        CHECK(violations_seen == 1);
        CHECK(r.applied == 1 && r.skipped == 1);
        CHECK(w.live_count() == 1);  // b's own spawn survived

        ecs::command_buffer c;
        const ecs::entity stale = c.spawn();
        c.clear();
        c.add<Pos>(stale, Pos{9});  // recorded after the clear made it stale
        const ecs::apply_result r2 = w.apply(c);
        CHECK(violations_seen == 2);
        CHECK(r2.applied == 0 && r2.skipped == 1);
    }
#endif

    // The payload arena is retained and reused across applies.
    {
        ecs::world w;
        ecs::command_buffer cmd;
        anchored_betrayals = 0;
        for (int round = 0; round < 3; ++round)
        {
            for (int i = 0; i < 200; ++i)
            {
                const ecs::entity e = cmd.spawn();
                cmd.add<Anchored>(e);
            }
            const ecs::apply_result r = w.apply(cmd);
            CHECK(r.skipped == 0);
        }
        CHECK(anchored_betrayals == 0);
        CHECK_VALID(w);
    }

    // Moved-from worlds are empty and reusable; the moved-to world owns the
    // data, and selections built before a move-construction stay valid.
    {
        ecs::world w1;
        w1.spawn(Pos{1}, Vel{1});
        w1.spawn(Pos{2});
        auto sel = w1.select<Pos>();

        ecs::world w2{std::move(w1)};
        CHECK(w2.live_count() == 2);
        CHECK(sel.count() == 2);  // pools traveled with the world
        // NOLINTBEGIN(bugprone-use-after-move) — moved-from reuse is the contract under test
        CHECK(w1.live_count() == 0);
        CHECK(w1.slot_count() == 0);
        w1.spawn(Pos{3});  // reuse after move
        CHECK(w1.live_count() == 1);
        CHECK_VALID(w1);
        CHECK_VALID(w2);

        ecs::world w3;
        w3.spawn(Pos{7});
        w3 = std::move(w2);
        CHECK(w3.live_count() == 2);
        CHECK(w2.live_count() == 0);
        w2.spawn();
        CHECK_VALID(w2);
        CHECK_VALID(w3);
        // NOLINTEND(bugprone-use-after-move)
    }
}

static void test_iteration_lock_unwinding()
{
    section("iteration locks unwind on exceptions");
    ecs::world w;
    const ecs::entity parent = w.spawn();
    const ecs::entity child = w.spawn();
    w.adopt(parent, child);

    struct probe_exception
    {
    };

    // The empty catch blocks below are the point: the probe exception only
    // exists to unwind the iteration mid-loop.
    // NOLINTBEGIN(bugprone-empty-catch)

    // each(): the RAII lock releases when the callback throws.
    w.spawn(Pos{1});
    try
    {
        w.each<Pos>([](ecs::entity, Pos&) -> bool { throw probe_exception{}; });
    }
    catch (const probe_exception&)
    {
    }

    // children_of(): same guarantee (regression: manual lock leaked here).
    try
    {
        w.children_of(parent, [](ecs::entity) -> bool { throw probe_exception{}; });
    }
    catch (const probe_exception&)
    {
    }
    // NOLINTEND(bugprone-empty-catch)

#if QUIVER_CHECKS
    CHECK(ecs::test_access::lock_count<Pos>(w) == 0);
    {
        violation_scope guard;
        w.orphan(child);  // would be refused if the kin lock had leaked
        CHECK(violations_seen == 0);
        CHECK(w.parent_of(child) == ecs::no_entity);
    }
#else
    w.orphan(child);
    CHECK(w.parent_of(child) == ecs::no_entity);
#endif
    CHECK_VALID(w);
}

#if QUIVER_CHECKS
static void test_tag_add_refused_during_iteration()
{
    section("tag add during its pool's iteration is refused");
    violation_scope guard;
    ecs::world w;
    const ecs::entity a = w.spawn();
    const ecs::entity outsider = w.spawn();
    w.add<TagA>(a);
    w.each<TagA>(
        [&](ecs::entity)
        {
            w.add<TagA>(outsider);  // tag adds can refuse: nothing to return
            return false;
        });
    CHECK(violations_seen == 1);
    CHECK(!w.has<TagA>(outsider));
    CHECK_VALID(w);
}
#endif

static void test_non_movable_components()
{
    section("non-movable components in stable storage");
    ecs::world w;
    const ecs::entity e = w.spawn();
    Pinned& p = w.add<Pinned>(e);  // constructed in place; never moved
    p.charge.store(41);
    p.charge.fetch_add(1);
    CHECK(w.get<Pinned>(e).charge.load() == 42);
    CHECK(w.find<Pinned>(e) == &p);  // stable storage: pointer holds

    w.replace<Pinned>(e);  // destroy + reconstruct in place
    CHECK(w.get<Pinned>(e).charge.load() == 0);
    CHECK(w.find<Pinned>(e) == &p);

    int visited = 0;
    w.each<Pinned>([&](ecs::entity, Pinned& got) { visited += got.charge.load() == 0 ? 1 : 0; });
    CHECK(visited == 1);

    CHECK(w.remove<Pinned>(e));
    w.add<Pinned>(e);
    w.purge<Pinned>();
    w.add<Pinned>(w.spawn());
    w.reset();  // destruction paths run without requiring movability
    CHECK_VALID(w);
}

static void test_overaligned_command_payloads()
{
    section("command buffer carries over-aligned payloads");
    misaligned_big = 0;
    ecs::world w;
    ecs::command_buffer cmd;
    for (int round = 0; round < 2; ++round)  // second round reuses the arena
    {
        std::vector<ecs::entity> es;
        for (int i = 0; i < 8; ++i)
        {
            const ecs::entity e = w.spawn();
            cmd.add<Pos>(e, Pos{i});  // interleave small payloads
            cmd.add<Big128>(e, Big128{static_cast<float>(i)});
            es.push_back(e);
        }
        const ecs::apply_result r = w.apply(cmd);
        CHECK(r.skipped == 0);
        for (const ecs::entity e : es)
        {
            CHECK(reinterpret_cast<std::uintptr_t>(w.find<Big128>(e)) % 128 == 0);
        }
    }
    CHECK(misaligned_big == 0);  // the arena handed out 128-aligned sources
    CHECK_VALID(w);
}

static void test_apply_spawn_callback()
{
    section("apply(buffer, on_spawn) reports provisional -> real");
    ecs::world w;
    ecs::command_buffer cmd;
    const ecs::entity g1 = cmd.spawn();
    cmd.add<Pos>(g1, Pos{1});
    const ecs::entity g2 = cmd.spawn();
    cmd.kill(g2);  // even a killed deferred spawn is reported

    std::vector<std::pair<ecs::entity, ecs::entity>> mapping;
    const ecs::apply_result r = w.apply(cmd,
                                        [&](ecs::entity provisional, ecs::entity real)
                                        { mapping.emplace_back(provisional, real); });
    CHECK(r.applied == 4 && r.skipped == 0);
    CHECK(mapping.size() == 2);
    CHECK(mapping[0].first == g1);
    CHECK(w.alive(mapping[0].second));
    CHECK(w.get<Pos>(mapping[0].second).x == 1);
    CHECK(mapping[1].first == g2);
    CHECK(!w.alive(mapping[1].second));  // its deferred kill already ran
    CHECK_VALID(w);
}

static void test_hash_identity()
{
    section("hash_of: stable name-hash identity");
    static_assert(ecs::hash_of<Pos>() != 0);
    static_assert(ecs::hash_of<Pos>() != ecs::hash_of<Vel>());
    static_assert(ecs::hash_of<Pos>() == ecs::hash_of<Pos>());

    ecs::world w;
    w.spawn(Pos{1});
    bool saw = false;
    w.each_pool(
        [&](const ecs::pool_info& info)
        {
            if (info.name == ecs::name_of<Pos>())
            {
                saw = true;
                CHECK(info.name_hash == ecs::hash_of<Pos>());
            }
        });
    CHECK(saw);
}

static void test_range_iteration()
{
    section("range(): range-for with structured bindings");
    ecs::world w;
    const ecs::entity a = w.spawn(Pos{1}, Vel{10});
    const ecs::entity b = w.spawn(Pos{2}, Vel{20});
    const ecs::entity c = w.spawn(Pos{3}, Vel{30});
    w.add<TagB>(b);

    // Parity with each(): same set, same order, same filters.
    auto sel = w.select<Pos, const Vel>(ecs::except<TagB>{});
    std::vector<ecs::entity> from_each;
    sel.each([&](ecs::entity e, Pos&, const Vel&) { from_each.push_back(e); });
    std::vector<ecs::entity> from_range;
    int sum = 0;
    for (auto&& [e, p, v] : sel.range())
    {
        from_range.push_back(e);
        p.x += 100;  // mutable ref flows through
        sum += v.v;  // const ref flows through
    }
    CHECK(from_each == from_range);
    CHECK((from_range == std::vector<ecs::entity>{a, c}));
    CHECK(sum == 40);
    CHECK(w.get<Pos>(a).x == 101);

    // Early exit unwinds the lock like each() does.
    for (auto&& [e, p, v] : sel.range())
    {
        static_cast<void>(p);
        static_cast<void>(v);
        if (e == a)
        {
            break;
        }
    }
#if QUIVER_CHECKS
    CHECK(ecs::test_access::lock_count<Pos>(w) == 0);
    CHECK(ecs::test_access::lock_count<Vel>(w) == 0);

    // A live range holds the lock: structural ops report violations.
    {
        violation_scope guard;
        auto r = w.select<Pos>().range();
        static_cast<void>(r.begin());
        CHECK(ecs::test_access::lock_count<Pos>(w) == 1);
        CHECK(!w.remove<Pos>(c));
        CHECK(violations_seen == 1);
    }
    CHECK(ecs::test_access::lock_count<Pos>(w) == 0);
#endif

    // Const world: const refs, and direct range-for over a temporary
    // selection (C++23 extends range-init temporaries).
    const ecs::world& cw = w;
    int const_sum = 0;
    for (auto&& [e, p] : cw.select<Pos>().range())
    {
        static_cast<void>(e);
        const_sum += p.x;
    }
    CHECK(const_sum == (101 + 2 + 103));
    CHECK_VALID(w);
}

static void test_maybe_components()
{
    section("maybe<T>: optional components in selections");
    ecs::world w;
    const ecs::entity plain = w.spawn(Pos{1});
    const ecs::entity tinted = w.spawn(Pos{2});
    w.add<Tint>(tinted, Tint{7});
    const ecs::entity excluded = w.spawn(Pos{3});
    w.add<TagB>(excluded);

    // maybe never filters: both plain and tinted match; except still applies.
    auto sel = w.select<Pos, ecs::maybe<Tint>>(ecs::except<TagB>{});
    CHECK(sel.count() == 2);
    int with = 0;
    int without = 0;
    sel.each(
        [&](ecs::entity e, Pos&, Tint* tint)
        {
            if (tint != nullptr)
            {
                ++with;
                CHECK(e == tinted);
                CHECK(tint->color == 7);
                tint->color = 8;  // writable through the pointer
            }
            else
            {
                ++without;
                CHECK(e == plain);
            }
        });
    CHECK(with == 1 && without == 1);
    CHECK(w.get<Tint>(tinted).color == 8);
    CHECK(sel.contains(plain));
    CHECK(!sel.contains(excluded));

    // maybe pools never drive: Tint's tiny pool must not shrink the result.
    CHECK((w.select<Pos, ecs::maybe<Tint>>().count() == 3));

    // Range rows carry the pointer too.
    for (auto&& [e, p, tint] : sel.range())
    {
        static_cast<void>(p);
        CHECK((tint != nullptr) == (e == tinted));
    }

    // Const world: const pointers; unregistered maybe pool stays a null
    // pointer without disqualifying anyone.
    const ecs::world& cw = w;
    int rows = 0;
    cw.each<Pos, ecs::maybe<Tint>>(ecs::except<TagB>{},
                                   [&](const Pos&, const Tint* tint)
                                   {
                                       ++rows;
                                       if (tint != nullptr)
                                       {
                                           CHECK(tint->color == 8);
                                       }
                                   });
    CHECK(rows == 2);

    ecs::world fresh;
    fresh.spawn(Pos{1});
    const ecs::world& cfresh = fresh;
    int fresh_rows = 0;
    cfresh.each<Pos, ecs::maybe<Tint>>(
        [&](const Pos&, const Tint* tint)
        {
            ++fresh_rows;
            CHECK(tint == nullptr);
        });
    CHECK(fresh_rows == 1);
    CHECK_VALID(w);
}

static void test_sort()
{
    section("sort<T>: value and entity comparators");
    ecs::world w;
    for (int i = 0; i < 32; ++i)
    {
        w.spawn(Pos{31 - i}, Vel{i});  // Pos descending on purpose
    }

    w.sort<Pos>([](const Pos& a, const Pos& b) { return a.x < b.x; });
    int expected = 0;
    bool ordered = true;
    w.each<Pos>([&](const Pos& p) { ordered = ordered && p.x == expected++; });
    CHECK(ordered);
    CHECK_VALID(w);  // sparse <-> dense survived the permutation

    // Stable pools sort bookkeeping only: payload pointers survive.
    ecs::world sw;
    std::vector<const Stable*> ptrs;
    std::vector<ecs::entity> ses;
    for (int i = 0; i < 16; ++i)
    {
        const ecs::entity e = sw.spawn();
        ptrs.push_back(&sw.add<Stable>(e, Stable{100 - i}));
        ses.push_back(e);
    }
    sw.sort<Stable>([](const Stable& a, const Stable& b) { return a.value < b.value; });
    for (std::size_t i = 0; i < ses.size(); ++i)
    {
        CHECK(sw.find<Stable>(ses[i]) == ptrs[i]);  // pointers unmoved
    }
    int last = -1;
    bool stable_ordered = true;
    sw.each<Stable>(
        [&](const Stable& s)
        {
            stable_ordered = stable_ordered && s.value > last;
            last = s.value;
        });
    CHECK(stable_ordered);
    CHECK_VALID(sw);

    // Entity comparator works on tag pools (no values to compare).
    ecs::world tw;
    const ecs::entity t1 = tw.spawn();
    const ecs::entity t2 = tw.spawn();
    tw.add<TagA>(t2);
    tw.add<TagA>(t1);
    tw.sort<TagA>([](ecs::entity a, ecs::entity b) { return a.index() < b.index(); });
    std::vector<ecs::entity> order;
    tw.select<TagA>().entities([&](ecs::entity e) { order.push_back(e); });
    CHECK((order == std::vector<ecs::entity>{t1, t2}));
    CHECK_VALID(tw);

#if QUIVER_CHECKS
    {
        violation_scope guard;
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                w.sort<Pos>([](const Pos& a, const Pos& b) { return a.x > b.x; });
                return false;
            });
        CHECK(violations_seen == 1);  // refused while locked
    }
#endif
}

static void test_query_helpers()
{
    section("first(), components_of(), child_count(), multi-get");
    ecs::world w;
    CHECK(w.select<Pos>().first() == ecs::no_entity);

    const ecs::entity a = w.spawn(Pos{1}, Vel{2});
    w.add<TagA>(a);
    CHECK(w.select<TagA>().first() == a);
    CHECK(w.select<Pos>(ecs::except<TagA>{}).first() == ecs::no_entity);

    // components_of: the per-entity inspector axis.
    std::vector<std::string_view> names;
    w.components_of(a, [&](const ecs::pool_info& info) { names.push_back(info.name); });
    CHECK(names.size() == 3);
    w.kill(a);
    names.clear();
    w.components_of(a, [&](const ecs::pool_info& info) { names.push_back(info.name); });
    CHECK(names.empty());  // dead entities report nothing

    // multi-get: one call, structured bindings, mutation through the tuple.
    const ecs::entity m = w.spawn(Pos{5}, Vel{6});
    auto [mp, mv] = w.get<Pos, Vel>(m);
    CHECK(mp.x == 5 && mv.v == 6);
    mp.x = 50;
    CHECK(w.get<Pos>(m).x == 50);
    const ecs::world& cw = w;
    auto [cp, cv] = cw.get<Pos, Vel>(m);
    static_assert(std::is_same_v<decltype(cp), const Pos&>);
    CHECK(cp.x == 50 && cv.v == 6);

    // child_count mirrors children_of.
    const ecs::entity parent = w.spawn();
    CHECK(w.child_count(parent) == 0);
    const ecs::entity c1 = w.spawn();
    const ecs::entity c2 = w.spawn();
    w.adopt(parent, c1);
    w.adopt(parent, c2);
    CHECK(w.child_count(parent) == 2);
    w.kill(c1);
    CHECK(w.child_count(parent) == 1);

    // kill is noexcept end-to-end now (free-stack slack invariant).
    static_assert(noexcept(w.kill(parent)));
    CHECK_VALID(w);
}

static void test_hooks()
{
    section("reactive hooks: on_add / on_remove / on_replace");
    ecs::world w;
    struct counters
    {
        int added = 0;
        int removed = 0;
        int replaced = 0;
        int last_removed_value = -1;
        ecs::world* expected_world = nullptr;
        bool world_ok = true;
    } c;
    c.expected_world = &w;

    const ecs::hook_token t_add = w.on_add<Pos>(
        [](ecs::world& world, ecs::entity, void* user)
        {
            auto* k = static_cast<counters*>(user);
            k->world_ok = k->world_ok && (&world == k->expected_world);
            ++k->added;
        },
        &c);
    const ecs::hook_token t_rem = w.on_remove<Pos>(
        [](ecs::world& world, ecs::entity e, void* user)
        {
            auto* k = static_cast<counters*>(user);
            // on_remove fires before destruction: the component is readable.
            k->last_removed_value = world.get<Pos>(e).x;
            ++k->removed;
        },
        &c);
    const ecs::hook_token t_rep = w.on_replace<Pos>(
        [](ecs::world&, ecs::entity, void* user) { ++static_cast<counters*>(user)->replaced; }, &c);
    CHECK(static_cast<bool>(t_add) && static_cast<bool>(t_rem) && static_cast<bool>(t_rep));

    const ecs::entity a = w.spawn(Pos{1});  // spawn-with-components fires too
    w.add<Pos>(w.spawn());
    CHECK(c.added == 2);
    CHECK(c.world_ok);

    w.replace<Pos>(a, Pos{5});
    w.put<Pos>(a, Pos{6});  // replacing half of put
    CHECK(c.replaced == 2);

    w.remove<Pos>(a);
    CHECK(c.removed == 1);
    CHECK(c.last_removed_value == 6);

    // Command-buffer replay funnels through the same verbs.
    ecs::command_buffer cmd;
    const ecs::entity g = cmd.spawn();
    cmd.add<Pos>(g, Pos{9});
    w.apply(cmd);
    CHECK(c.added == 3);

    // purge and reset fire on_remove per live component (two entities still
    // held Pos at purge time).
    w.purge<Pos>();
    CHECK(c.removed == 3);
    w.spawn(Pos{1});  // added -> 4
    w.reset();
    CHECK(c.removed == 4);

    // unhook stops delivery; stale tokens answer false.
    CHECK(c.added == 4);
    CHECK(w.unhook(t_add));
    CHECK(!w.unhook(t_add));
    w.spawn(Pos{1});
    CHECK(c.added == 4);
    CHECK(w.unhook(t_rem));
    CHECK(w.unhook(t_rep));

#if QUIVER_CHECKS
    // A hook that structurally mutates its own pool is a reported violation.
    {
        violation_scope guard;
        static int fired = 0;
        fired = 0;
        const ecs::hook_token t = w.on_add<Vel>(
            [](ecs::world& world, ecs::entity e, void*)
            {
                ++fired;
                world.remove<Vel>(e);  // own pool, mid-dispatch: refused
            },
            nullptr);
        const ecs::entity v = w.spawn();
        w.add<Vel>(v, Vel{1});
        CHECK(fired == 1);
        CHECK(violations_seen == 1);
        CHECK(w.has<Vel>(v));  // the removal was refused
        CHECK(w.unhook(t));
    }
#endif
    CHECK_VALID(w);
}

static void test_blueprint()
{
    section("blueprint: reusable spawn recipes");
    ecs::world w;
    ecs::blueprint goblin;
    goblin.add<Pos>(7);
    goblin.add<Hp>(30);
    goblin.add<TagA>();
    CHECK(goblin.size() == 3);

    const ecs::entity g1 = w.spawn(goblin);
    const ecs::entity g2 = w.spawn(goblin);
    CHECK(w.get<Pos>(g1).x == 7 && w.get<Pos>(g2).x == 7);
    CHECK(w.has<TagA>(g1) && w.has<TagA>(g2));

    w.get<Pos>(g1).x = 99;  // stamped values are independent copies
    CHECK(w.get<Pos>(g2).x == 7);
    const ecs::entity g3 = w.spawn(goblin);
    CHECK(w.get<Pos>(g3).x == 7);

#if QUIVER_CHECKS
    {
        violation_scope guard;
        goblin.add<Pos>(1);  // duplicate component in the recipe
        CHECK(violations_seen == 1);
        CHECK(goblin.size() == 3);
    }
#endif

    // clear + rebuild reuses the arena; move semantics keep payloads alive.
    goblin.clear();
    CHECK(goblin.empty());
    goblin.add<Pos>(1);
    ecs::blueprint moved{std::move(goblin)};
    const ecs::entity g4 = w.spawn(moved);
    CHECK(w.get<Pos>(g4).x == 1);

    // Payload destruction balances even for never-stamped recipes.
    Counted::total_ctors = 0;
    Counted::total_dtors = 0;
    {
        ecs::blueprint never_used;
        never_used.add<Counted>(5);
    }
    CHECK(Counted::total_ctors == Counted::total_dtors);
    CHECK_VALID(w);
}

static void test_duplicate()
{
    section("duplicate: whole-entity cloning");
    ecs::world w;
    const ecs::entity src = w.spawn(Pos{4}, Vel{5});
    w.add<TagA>(src);
    w.add<Stable>(src, Stable{6});
    w.add<MoveOnly>(src, MoveOnly{7});  // not copy-constructible -> skipped

    const ecs::entity parent = w.spawn();
    w.adopt(parent, src);

    const ecs::duplicate_result r = w.duplicate(src);
    CHECK(w.alive(r.clone));
    CHECK(r.copied == 4);   // Pos, Vel, TagA, Stable
    CHECK(r.skipped == 1);  // MoveOnly
    CHECK(w.get<Pos>(r.clone).x == 4);
    CHECK(w.get<Stable>(r.clone).value == 6);
    CHECK(w.has<TagA>(r.clone));
    CHECK(!w.has<MoveOnly>(r.clone));
    CHECK(w.parent_of(r.clone) == ecs::no_entity);  // clones start unlinked
    CHECK(w.parent_of(src) == parent);              // the original keeps its links

    w.get<Pos>(r.clone).x = 40;  // independent copy
    CHECK(w.get<Pos>(src).x == 4);

#if QUIVER_CHECKS
    {
        violation_scope guard;
        const ecs::duplicate_result dead = w.duplicate(ecs::no_entity);
        CHECK(violations_seen == 1);
        CHECK(dead.clone == ecs::no_entity);
    }
#endif
    CHECK_VALID(w);
}

static void test_entity_ref()
{
    section("entity_ref: forwarding handles");
    ecs::world w;
    ecs::entity_ref hero = w.ref(w.spawn());
    CHECK(hero.alive());
    hero.add<Pos>(1);
    hero.obtain<Vel>(2);
    CHECK(hero.has<Pos>());
    CHECK(hero.get<Pos>().x == 1);
    CHECK(hero.find<Vel>() != nullptr);
    hero.put<Pos>(3);
    CHECK(hero.get<Pos>().x == 3);
    CHECK(hero.remove<Vel>());

    const ecs::entity parent = w.spawn();
    w.adopt(parent, hero.id());
    CHECK(hero.parent().id() == parent);

    const ecs::world& cw = w;
    ecs::const_entity_ref seen = cw.ref(hero);  // implicit entity conversion
    CHECK(seen.alive());
    CHECK(seen.get<Pos>().x == 3);
    CHECK(seen.find<Pos>() != nullptr);

    hero.kill();
    CHECK(!hero.alive());
    CHECK(!seen.alive());
    CHECK(!static_cast<bool>(hero));
    CHECK_VALID(w);
}

static void test_obtain_and_find_all()
{
    section("obtain (get-or-add) and find_all");
    ecs::world w;
    const ecs::entity e = w.spawn();

    Pos& fresh = w.obtain<Pos>(e, 5);
    CHECK(fresh.x == 5);
    Pos& again = w.obtain<Pos>(e, 99);
    CHECK(&again == &fresh);
    CHECK(again.x == 5);  // existing value wins; no overwrite

    w.obtain<TagA>(e);
    CHECK(w.has<TagA>(e));
    w.obtain<TagA>(e);  // idempotent

    w.add<Vel>(e, Vel{2});
    auto [p, v, h] = w.find_all<Pos, Vel, Hp>(e);
    CHECK(p != nullptr && p->x == 5);
    CHECK(v != nullptr && v->v == 2);
    CHECK(h == nullptr);

    w.kill(e);
    auto [dp, dv, dh] = w.find_all<Pos, Vel, Hp>(e);
    CHECK(dp == nullptr && dv == nullptr && dh == nullptr);

    const ecs::world& cw = w;
    const ecs::entity e2 = w.spawn(Pos{1});
    auto [cp2, cv2] = cw.find_all<Pos, Vel>(e2);
    static_assert(std::is_same_v<decltype(cp2), const Pos*>);
    CHECK(cp2 != nullptr && cv2 == nullptr);
    CHECK_VALID(w);
}

static void test_runtime_queries()
{
    section("runtime queries: pool_ref + runtime_selection");
    ecs::world w;
    std::vector<ecs::entity> expected;
    for (int i = 0; i < 10; ++i)
    {
        const ecs::entity e = w.spawn(Pos{i});
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
            expected.push_back(e);  // Pos && Vel && !TagB
        }
    }

    // The three addressing forms agree.
    const ecs::pool_ref by_type = w.find_pool<Pos>();
    CHECK(static_cast<bool>(by_type));
    CHECK(by_type.name() == ecs::name_of<Pos>());
    CHECK(w.find_pool(by_type.info().id).name() == by_type.name());
    CHECK(w.find_pool_by_hash(ecs::hash_of<Pos>()).name() == by_type.name());
    CHECK(!w.find_pool_by_hash(0xDEAD));
    CHECK(by_type.size() == 10);

    ecs::runtime_selection q;
    q.include(w.find_pool<Pos>()).include(w.find_pool<Vel>()).exclude(w.find_pool<TagB>());
    std::vector<ecs::entity> from_runtime;
    q.entities([&](ecs::entity e) { from_runtime.push_back(e); });
    CHECK(from_runtime == expected);
    CHECK(q.count() == expected.size());
    CHECK(q.contains(expected.front()));

    // Parity with the typed layer.
    std::vector<ecs::entity> from_typed;
    w.select<Pos, Vel>(ecs::except<TagB>{})
        .entities([&](ecs::entity e) { from_typed.push_back(e); });
    CHECK(from_runtime == from_typed);

    // Including a pool that does not exist makes the selection impossible.
    ecs::runtime_selection impossible;
    impossible.include(w.find_pool_by_hash(0xDEAD));
    impossible.include(w.find_pool<Pos>());
    CHECK(impossible.count() == 0);

    // Excluding an absent pool excludes nothing.
    ecs::runtime_selection lax;
    lax.include(w.find_pool<Pos>()).exclude(ecs::pool_ref{});
    CHECK(lax.count() == 10);
    CHECK_VALID(w);
}

static void test_sort_along()
{
    section("sort_along: paired ordering for locality");
    ecs::world w;
    std::vector<ecs::entity> lead_order;
    for (int i = 0; i < 12; ++i)
    {
        const ecs::entity e = w.spawn(Pos{i});
        if (i % 3 != 0)
        {
            w.add<Vel>(e, Vel{i});
        }
    }
    // Scramble Pos, then order it by Vel's dense order.
    w.sort<Pos>([](const Pos& a, const Pos& b) { return a.x > b.x; });
    w.select<Vel>().entities([&](ecs::entity e) { lead_order.push_back(e); });

    std::vector<const Stable*> stable_ptrs;
    std::vector<ecs::entity> stable_es;
    for (const ecs::entity e : lead_order)
    {
        stable_es.push_back(e);
        stable_ptrs.push_back(&w.add<Stable>(e, Stable{w.get<Pos>(e).x}));
    }

    w.sort_along<Pos, Vel>();
    std::vector<ecs::entity> pos_order;
    w.select<Pos>().entities([&](ecs::entity e) { pos_order.push_back(e); });
    CHECK(pos_order.size() == 12);
    for (std::size_t i = 0; i < lead_order.size(); ++i)
    {
        CHECK(pos_order[i] == lead_order[i]);  // shared prefix follows the lead
    }

    // Stable pools pair up without moving payloads.
    w.sort_along<Stable, Vel>();
    for (std::size_t i = 0; i < stable_es.size(); ++i)
    {
        CHECK(w.find<Stable>(stable_es[i]) == stable_ptrs[i]);
    }
    CHECK_VALID(w);
}

static void test_chunk_capacity_and_footprint()
{
    section("chunk_capacity<T> and memory footprint");
    static_assert(ecs::chunk_capacity<TunedStable> == 8);
    static_assert(ecs::chunk_capacity<Stable> == 1024);  // default formula for int payloads

    ecs::world w;
    w.add<TunedStable>(w.spawn(), TunedStable{1});
    bool saw = false;
    w.each_pool(
        [&](const ecs::pool_info& info)
        {
            if (info.name == ecs::name_of<TunedStable>())
            {
                saw = true;
                CHECK(info.capacity == 8);  // exactly one tuned chunk
                CHECK(info.index_bytes > 0);
                CHECK(info.bookkeeping_bytes > 0);
            }
        });
    CHECK(saw);

    const ecs::memory_footprint f = w.footprint();
    CHECK(f.entity_table_bytes > 0);
    CHECK(f.component_bytes >= 8 * sizeof(TunedStable));
    CHECK(f.index_bytes > 0);
    CHECK(f.total() ==
          f.entity_table_bytes + f.component_bytes + f.index_bytes + f.bookkeeping_bytes);
    CHECK_VALID(w);
}

static void test_pmr_support()
{
    section("std::pmr: every scaling allocation routes through the resource");
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
        ecs::world w(&arena);
        ecs::command_buffer cmd(&arena);
        ecs::blueprint bp(&arena);
        bp.add<Pos>(1);
        bp.add<Stable>(Stable{2});
        for (int i = 0; i < 200; ++i)
        {
            const ecs::entity e = w.spawn(bp);
            w.add<TagA>(e);
            cmd.add<Vel>(e, Vel{i});
        }
        w.apply(cmd);
        for (int i = 0; i < 100; ++i)
        {
            w.kill(w.select<Pos>().first());
        }
        w.shrink();
        CHECK(w.select<Pos>().count() == 100);
        CHECK(arena.allocations > 0);
        CHECK_VALID(w);
    }
    // Everything the world/buffer/blueprint took from the resource came back.
    CHECK(arena.live_bytes == 0);
}

static void test_sort_non_involution()
{
    section("sort<T>: non-involution permutations (regression)");
    // The original cycle-walk applied the INVERSE permutation; reversals are
    // involutions and masked it. {30,10,20} has a 3-cycle: the honest test.
    ecs::world w;
    w.spawn(Pos{30});
    w.spawn(Pos{10});
    w.spawn(Pos{20});
    w.sort<Pos>([](const Pos& a, const Pos& b) { return a.x < b.x; });
    std::vector<int> order;
    w.each<Pos>([&](const Pos& p) { order.push_back(p.x); });
    CHECK((order == std::vector<int>{10, 20, 30}));
    CHECK_VALID(w);

    // Seeded pseudo-shuffle, packed and stable.
    ecs::world big;
    std::uint32_t state = 0x9E3779B9u;
    for (int i = 0; i < 64; ++i)
    {
        state = (state * 1664525u) + 1013904223u;  // LCG: deterministic "random"
        const int v = static_cast<int>(state % 1000);
        const ecs::entity e = big.spawn(Pos{v});
        big.add<Stable>(e, Stable{v});
    }
    big.sort<Pos>([](const Pos& a, const Pos& b) { return a.x < b.x; });
    big.sort<Stable>([](const Stable& a, const Stable& b) { return a.value > b.value; });
    int prev = -1;
    bool ascending = true;
    big.each<Pos>([&](const Pos& p) { ascending = ascending && p.x >= std::exchange(prev, p.x); });
    CHECK(ascending);
    prev = 1001;
    bool descending = true;
    big.each<Stable>([&](const Stable& s)
                     { descending = descending && s.value <= std::exchange(prev, s.value); });
    CHECK(descending);
    CHECK_VALID(big);
}

static void test_hook_edge_cases()
{
    section("hooks: new pools mid-sweep, dying-entity adds, clone double-grant");

    // A hook registering a brand-new component type during kill/reset/
    // duplicate used to reallocate active_ under the running sweep (ASan UAF).
    struct LateA
    {
        int v = 0;
    };
    struct LateB
    {
        int v = 0;
    };
    {
        ecs::world w;
        ecs::entity grave = w.spawn();  // the hook targets another entity
        const ecs::hook_token t =
            w.on_remove<Pos>([](ecs::world& world, ecs::entity, void* user)
                             { world.put<LateA>(*static_cast<ecs::entity*>(user), LateA{1}); },
                             &grave);
        const ecs::entity victim = w.spawn(Pos{1});
        w.kill(victim);  // first LateA registration happens mid-kill
        CHECK(w.has<LateA>(grave));
        CHECK_VALID(w);
        CHECK(w.unhook(t));

        const ecs::hook_token t2 =
            w.on_remove<Pos>([](ecs::world& world, ecs::entity, void*)
                             { static_cast<void>(world.find_pool<LateB>()); },  // registers nothing
                             nullptr);
        w.spawn(Pos{2});
        w.reset();
        CHECK_VALID(w);
        CHECK(w.unhook(t2));
    }

    // A hook adding a component to the DYING entity is refused (it would
    // leave a pool holding a dead entity; the recycled slot must stay clean).
#if QUIVER_CHECKS
    {
        violation_scope guard;
        ecs::world w;
        const ecs::hook_token t = w.on_remove<Pos>(
            [](ecs::world& world, ecs::entity e, void*) { world.add<TagA>(e); }, nullptr);
        const ecs::entity victim = w.spawn(Pos{1});
        w.kill(victim);
        CHECK(violations_seen == 1);
        CHECK_VALID(w);
        const ecs::entity recycled = w.spawn();
        CHECK(recycled.index() == victim.index());
        CHECK(!w.has<TagA>(recycled));  // nothing leaked onto the new tenant
        CHECK(w.unhook(t));
    }
#endif

    // duplicate: an on_add hook granting the clone a component the source
    // also has must not double-attach it.
    {
        ecs::world w;
        const ecs::hook_token t = w.on_add<Pos>(
            [](ecs::world& world, ecs::entity e, void*)
            {
                if (!world.has<Vel>(e))
                {
                    world.add<Vel>(e, Vel{99});
                }
            },
            nullptr);
        const ecs::entity src = w.spawn();
        w.add<Pos>(src, Pos{1});  // hook grants Vel{99} to src too
        w.replace<Vel>(src, Vel{5});
        const ecs::duplicate_result r = w.duplicate(src);
        CHECK(w.alive(r.clone));
        CHECK(w.get<Vel>(r.clone).v == 99);  // the hook's grant won; no double entry
        CHECK_VALID(w);
        CHECK(w.unhook(t));
    }

    // duplicate at items_ capacity: the copied-from reference must survive
    // the growth (heap-payload component; the ASan config proves it).
    {
        ecs::world w;
        ecs::entity last;
        for (int i = 0; i < 8; ++i)
        {
            const ecs::entity e = w.spawn();
            w.add<Label>(e, Label{std::string(64, static_cast<char>('a' + i))});
            last = e;
        }
        const ecs::duplicate_result r = w.duplicate(last);
        CHECK(w.get<Label>(r.clone).value == w.get<Label>(last).value);
        CHECK_VALID(w);
    }

#if QUIVER_CHECKS
    // unhook is structural: refused mid-iteration like connect.
    {
        violation_scope guard;
        ecs::world w;
        const ecs::hook_token t = w.on_add<Pos>([](ecs::world&, ecs::entity, void*) {}, nullptr);
        w.spawn(Pos{1});
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                CHECK(!w.unhook(t));
                return false;
            });
        CHECK(violations_seen == 1);
        CHECK(w.unhook(t));  // fine once the iteration ended
    }
#endif
}

static void test_arena_warm_overaligned()
{
    section("payload arena: warm reuse of over-aligned chunks (regression)");
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
    ecs::world w;
    ecs::command_buffer cmd(&arena);
    const ecs::entity e = w.spawn();
    // Warm-up frame, then the steady state must stop allocating entirely.
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
    CHECK(arena.allocations == after_warmup);
    CHECK_VALID(w);
}

static void test_runtime_selection_self_mutation()
{
    section("runtime_selection: editing the selection mid-iteration is safe");
    ecs::world w;
    for (int i = 0; i < 4; ++i)
    {
        w.spawn(Pos{i});
    }
    w.add<Vel>(w.select<Pos>().first(), Vel{1});

    ecs::runtime_selection q;
    q.include(w.find_pool<Pos>());
    std::size_t visited = 0;
    q.entities(
        [&](ecs::entity)
        {
            if (visited++ == 0)
            {
                q.include(w.find_pool<Vel>());  // affects future walks only
            }
            return true;
        });
    CHECK(visited == 4);    // the in-flight walk kept its snapshot
    CHECK(q.count() == 1);  // the next walk sees the narrowed selection
#if QUIVER_CHECKS
    CHECK(ecs::test_access::lock_count<Pos>(w) == 0);  // no underflow
    CHECK(ecs::test_access::lock_count<Vel>(w) == 0);
    {
        violation_scope guard;
        w.add<Pos>(w.spawn(), Pos{9});  // would report if a lock leaked
        CHECK(violations_seen == 0);
    }
#endif
    CHECK_VALID(w);
}

static void test_empty_entity_ref()
{
    section("entity_ref: empty (default-constructed) refs are inert");
    ecs::entity_ref empty;
    CHECK(!empty.alive());
    CHECK(!static_cast<bool>(empty));
    CHECK(!empty.has<Pos>());
    CHECK(empty.find<Pos>() == nullptr);
    CHECK(!empty.remove<Pos>());
    empty.kill();  // no-ops, no crash
    empty.orphan();
    CHECK(!empty.parent().alive());

    ecs::const_entity_ref cempty;
    CHECK(!cempty.alive());
    CHECK(!cempty.has<Pos>());
    CHECK(cempty.find<Pos>() == nullptr);
    CHECK(!cempty.parent().alive());
}

static void test_hook_connection_forms()
{
    section("hooks: template<auto> connection forms and scoped_hook");
    ecs::world w;

    // Free functions, both signatures.
    hookfree::free_hits = 0;
    const ecs::hook_token t1 = w.on_add<Pos, &hookfree::on_free_fn>();
    const ecs::hook_token t2 = w.on_add<Pos, &hookfree::on_free_short>();
    w.spawn(Pos{1});
    CHECK(hookfree::free_hits == 2);
    CHECK(w.unhook(t1));
    CHECK(w.unhook(t2));

    // Member functions on an instance.
    GpuUploads gpu;
    const ecs::hook_token t3 = w.on_add<Tint, &GpuUploads::upload>(&gpu);
    const ecs::hook_token t4 = w.on_remove<Tint, &GpuUploads::free_one>(&gpu);
    const ecs::entity e = w.spawn();
    w.add<Tint>(e, Tint{3});
    w.remove<Tint>(e);
    CHECK(gpu.uploaded == 1);
    CHECK(gpu.freed == 1);
    CHECK(w.unhook(t3));
    CHECK(w.unhook(t4));

    // scoped_hook: RAII disconnect, move semantics, idempotent release.
    {
        ecs::scoped_hook scoped(w, w.on_add<Tint, &GpuUploads::upload>(&gpu));
        CHECK(static_cast<bool>(scoped));
        w.add<Tint>(e, Tint{4});
        CHECK(gpu.uploaded == 2);
        ecs::scoped_hook moved(std::move(scoped));
        CHECK(!static_cast<bool>(scoped));  // NOLINT(bugprone-use-after-move)
        CHECK(static_cast<bool>(moved));
        moved.release();
        moved.release();  // idempotent
        CHECK(!static_cast<bool>(moved));
    }
    w.replace<Tint>(e, Tint{5});
    w.remove<Tint>(e);
    CHECK(gpu.uploaded == 2);  // disconnected: no further uploads

#if QUIVER_CHECKS
    // Destroying a scoped_hook mid-iteration hits unhook's refusal: the hook
    // stays connected and a violation is reported.
    {
        violation_scope guard;
        auto* leak = new ecs::scoped_hook(w, w.on_add<Pos, &hookfree::on_free_fn>());
        w.each<Pos>(
            [&](ecs::entity, Pos&)
            {
                delete leak;  // unhook refused under the lock
                return false;
            });
        CHECK(violations_seen == 1);
        hookfree::free_hits = 0;
        w.spawn(Pos{2});
        CHECK(hookfree::free_hits == 1);  // still connected
    }
#endif
    CHECK_VALID(w);
}

static void test_tracker()
{
    section("tracker<T>: deduped drain-pattern collections");
    ecs::world w;
    ecs::tracker<Hp> hurt(w);

    const ecs::entity a = w.spawn();
    const ecs::entity b = w.spawn();
    w.add<Hp>(a, Hp{10});
    w.add<Hp>(b, Hp{20});
    CHECK(hurt.added().size() == 2);

    // Dedupe: three replaces, one entry.
    w.replace<Hp>(a, Hp{9});
    w.replace<Hp>(a, Hp{8});
    w.put<Hp>(a, Hp{7});
    CHECK(hurt.replaced().size() == 1);
    CHECK(hurt.replaced()[0] == a);

    // removed() may hold dead handles — that is the information.
    w.kill(b);
    CHECK(hurt.removed().size() == 1);
    CHECK(hurt.removed()[0] == b);
    CHECK(!w.alive(hurt.removed()[0]));

    // Drain, then a second batch dedupes independently.
    hurt.clear();
    CHECK(hurt.added().empty() && hurt.replaced().empty() && hurt.removed().empty());
    w.replace<Hp>(a, Hp{6});
    CHECK(hurt.replaced().size() == 1);
    hurt.clear();

    // Slot recycling before a drain: the live handle wins, exactly once.
    w.remove<Hp>(a);
    hurt.clear();
    w.kill(a);
    const ecs::entity recycled = w.spawn();
    CHECK(recycled.index() == a.index());
    w.add<Hp>(recycled, Hp{1});
    CHECK(hurt.added().size() == 1);
    CHECK(hurt.added()[0] == recycled);

    // Mask subsets connect only those events.
    ecs::tracker<Pos> moves(w, ecs::track::added | ecs::track::removed);
    const ecs::entity c = w.spawn(Pos{1});
    w.replace<Pos>(c, Pos{2});
    w.kill(c);
    CHECK(moves.added().size() == 1);
    CHECK(moves.replaced().empty());
    CHECK(moves.removed().size() == 1);

    // reset() funnels through on_remove: the tracker sees every death (the
    // recycled entity still carries its Hp, so three in total).
    hurt.clear();
    w.spawn(Hp{1});
    w.spawn(Hp{2});
    hurt.clear();
    w.reset();
    CHECK(hurt.removed().size() == 3);

    // pmr: tracker storage routes through the supplied resource.
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
        w.spawn(Hp{1});
        CHECK(arena.allocations > 0);
    }
    CHECK_VALID(w);
}

static void test_bonded_pairs()
{
    section("bonded pairs: mirrored partitions, O(1) maintenance");

    // Interleaved adds, both orders; mirrored positions at every step.
    {
        ecs::world w;
        auto view = w.bond<Pos, Vel>();
        CHECK(view.count() == 0);
        std::vector<ecs::entity> both;
        for (int i = 0; i < 12; ++i)
        {
            const ecs::entity e = w.spawn();
            if (i % 3 == 0)  // A then B
            {
                w.add<Pos>(e, Pos{i});
                w.add<Vel>(e, Vel{i});
                both.push_back(e);
            }
            else if (i % 3 == 1)  // B then A
            {
                w.add<Vel>(e, Vel{i});
                w.add<Pos>(e, Pos{i});
                both.push_back(e);
            }
            else  // A only
            {
                w.add<Pos>(e, Pos{i});
            }
        }
        CHECK(view.count() == both.size());
        // Mirror invariant: same entity at the same position in both pools.
        bool mirrored = true;
        std::size_t seen = 0;
        view.each(
            [&](ecs::entity e, Pos& p, Vel& v)
            {
                mirrored = mirrored && p.x == v.v;  // built with equal values
                mirrored = mirrored && w.get<Pos>(e).x == p.x;
                ++seen;
                return true;
            });
        CHECK(seen == both.size());
        CHECK(mirrored);
        CHECK_VALID(w);

        // Removing a paired entity shrinks the partition; the survivor stays.
        const ecs::entity victim = both[1];
        w.remove<Vel>(victim);
        CHECK(view.count() == both.size() - 1);
        CHECK(w.has<Pos>(victim));  // remains, unpaired
        w.kill(both[2]);
        CHECK(view.count() == both.size() - 2);
        CHECK_VALID(w);
    }

    // bond() over prepopulated pools builds the partition in place.
    {
        ecs::world w;
        std::size_t expected = 0;
        for (int i = 0; i < 20; ++i)
        {
            const ecs::entity e = w.spawn(Pos{i});
            if (i % 2 == 0)
            {
                w.add<Stable>(e, Stable{i});
                ++expected;
            }
        }
        auto view = w.bond<Pos, Stable>();  // packed + stable
        CHECK(view.count() == expected);
        int sum = 0;
        view.each([&](Pos& p, const Stable& s) { sum += p.x - s.value; });
        CHECK(sum == 0);
        CHECK_VALID(w);

        // range() with structured bindings; early exit unwinds locks.
        std::size_t rows = 0;
        for (auto&& [e, p, s] : view.range())
        {
            static_cast<void>(e);
            static_cast<void>(s);
            p.x += 1;
            if (++rows == 3)
            {
                break;
            }
        }
        CHECK(rows == 3);
        w.add<Pos>(w.spawn(), Pos{99});  // would report if a lock leaked

        // const world -> all-const view.
        const ecs::world& cw = w;
        auto cview = cw.bonded<Pos, Stable>();
        CHECK(cview.count() == view.count());
        cview.each([](const Pos&, const Stable&) {});
    }

    // Tag-side bond: "fast filtered iteration".
    {
        ecs::world w;
        auto burning = w.bond<Hp, TagA>();
        const ecs::entity e1 = w.spawn(Hp{10});
        const ecs::entity e2 = w.spawn(Hp{20});
        w.add<TagA>(e2);
        CHECK(burning.count() == 1);
        burning.each([](ecs::entity, Hp& h) { h.hp -= 5; });
        CHECK(w.get<Hp>(e2).hp == 15);
        CHECK(w.get<Hp>(e1).hp == 10);
        CHECK(burning.contains(e2));
        CHECK(!burning.contains(e1));
        CHECK(burning.entity_at(0) == e2);
    }

    // Replay paths re-pair automatically: buffer, blueprint, duplicate.
    {
        ecs::world w;
        auto view = w.bond<Pos, Vel>();

        ecs::command_buffer cmd;
        const ecs::entity p1 = cmd.spawn();
        cmd.add<Pos>(p1, Pos{1});
        cmd.add<Vel>(p1, Vel{1});
        w.apply(cmd);
        CHECK(view.count() == 1);

        ecs::blueprint pair;
        pair.add<Pos>(2);
        pair.add<Vel>(2);
        w.spawn(pair);
        w.spawn(pair);
        CHECK(view.count() == 3);

        const ecs::entity src = w.select<Pos, Vel>().first();
        const ecs::duplicate_result dup = w.duplicate(src);
        CHECK(w.alive(dup.clone));
        CHECK(view.count() == 4);
        CHECK_VALID(w);
    }

    // The emplace return reference survives the bond fix-up relocation.
    {
        ecs::world w;
        w.bond<Pos, Vel>();
        for (int i = 0; i < 4; ++i)
        {
            w.spawn(Pos{i});  // unpaired tail
        }
        const ecs::entity e = w.spawn();
        w.add<Vel>(e, Vel{7});
        Pos& fresh = w.add<Pos>(e, Pos{42});  // pairs immediately: relocated to front
        CHECK(fresh.x == 42);
        CHECK(&fresh == w.find<Pos>(e));
        CHECK_VALID(w);
    }

    // unbond: stored views read empty; rebond rebuilds.
    {
        ecs::world w;
        auto view = w.bond<Pos, Vel>();
        const ecs::entity e = w.spawn(Pos{1}, Vel{1});
        static_cast<void>(e);
        CHECK(view.count() == 1);
        CHECK((w.unbond<Pos, Vel>()));
        CHECK((!w.unbond<Pos, Vel>()));     // already dissolved
        CHECK(view.count() == 0);           // tombstoned, not dangling
        auto rebuilt = w.bond<Vel, Pos>();  // argument order may differ
        CHECK(rebuilt.count() == 1);
        CHECK(view.count() == 0);  // the old view stays empty forever
        CHECK_VALID(w);
    }

    // purge/reset zero the partition; bonds survive and re-pair.
    {
        ecs::world w;
        auto view = w.bond<Pos, Vel>();
        w.spawn(Pos{1}, Vel{1});
        CHECK(view.count() == 1);
        w.purge<Vel>();
        CHECK(view.count() == 0);
        w.add<Vel>(w.select<Pos>().first(), Vel{2});
        CHECK(view.count() == 1);
        w.reset();
        CHECK(view.count() == 0);
        w.spawn(Pos{3}, Vel{3});
        CHECK(view.count() == 1);
        CHECK_VALID(w);
    }

#if QUIVER_CHECKS
    // The violation matrix.
    {
        ecs::world w;
        w.bond<Pos, Vel>();
        const ecs::entity e = w.spawn(Pos{1}, Vel{1});
        w.spawn(Pos{2});  // sort's size<2 fast path must not mask the refusals

        {
            violation_scope guard;
            w.bond<Pos, Hp>();  // Pos already bonded
            CHECK(violations_seen == 1);
        }
        {
            violation_scope guard;
            w.sort<Pos>([](const Pos& a, const Pos& b) { return a.x < b.x; });
            CHECK(violations_seen == 1);  // sort on a bonded pool
            w.sort_along<Pos, Hp>();
            CHECK(violations_seen == 2);  // bonded follow
            w.sort_along<Hp, Pos>();      // bonded LEAD is read-only: legal
            CHECK(violations_seen == 2);
        }
        {
            // Partner-lock coupling: iterating Vel refuses structural ops on Pos.
            violation_scope guard;
            w.each<Vel>(
                [&](ecs::entity, Vel&)
                {
                    CHECK(!w.remove<Pos>(e));
                    return false;
                });
            CHECK(violations_seen == 1);
            CHECK(w.has<Pos>(e));
        }
        {
            // A hook on one bonded pool may not structurally touch the other.
            violation_scope guard;
            const ecs::entity bystander = w.spawn(Pos{5});
            const ecs::hook_token t =
                w.on_add<Pos>([](ecs::world& world, ecs::entity, void* user)
                              { world.add<TagB>(*static_cast<ecs::entity*>(user)); },
                              const_cast<ecs::entity*>(&bystander));
            static_cast<void>(t);
            // TagB is not bonded: fine. Now bond it and watch the refusal.
            CHECK(violations_seen == 0);
            w.unhook(t);
            w.bond<TagB, Hp>();
            const ecs::hook_token t2 =
                w.on_add<Hp>([](ecs::world& world, ecs::entity e2, void*)
                             { world.add<TagB>(e2); },  // TagB's partner Hp is dispatch-locked
                             nullptr);
            const ecs::entity h = w.spawn();
            w.add<Hp>(h, Hp{1});
            CHECK(violations_seen == 1);  // tag add refused under partner lock
            CHECK(!w.has<TagB>(h));
            w.unhook(t2);
        }
        {
            violation_scope guard;
            w.each<Pos>(
                [&](ecs::entity, Pos&)
                {
                    CHECK((!w.unbond<Pos, Vel>()));  // refused while locked
                    return false;
                });
            CHECK(violations_seen == 1);
        }
        {
            violation_scope guard;
            auto missing = w.bonded<Pos, Stable>();  // no such bond
            CHECK(violations_seen == 1);
            CHECK(missing.count() == 0);
        }
        CHECK_VALID(w);
    }

    // validate() detects a broken mirror (corruption injected on one side).
    {
        violation_scope guard;
        ecs::world w;
        w.bond<Pos, Vel>();
        w.spawn(Pos{1}, Vel{1});
        w.spawn(Pos{2}, Vel{2});
        ecs::test_access::corrupt_bond<Pos>(w);
        const auto r = w.validate();
        CHECK(!r.has_value());
        CHECK(r.error().code == ecs::fault_code::bond_broken);
    }
#endif
}

static void test_split()
{
    section("selection::split(n): parallel chunks under one umbrella lock");
    ecs::world w;
    std::vector<ecs::entity> expected;
    for (int i = 0; i < 1000; ++i)
    {
        const ecs::entity e = w.spawn(Pos{i});
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
    const auto sel = w.select<Pos, Vel>(ecs::except<TagB>{});

    // Union of parts == each(), no duplicates, for several n.
    for (const std::size_t n : {std::size_t{1}, std::size_t{3}, std::size_t{4}, std::size_t{7}})
    {
        std::unordered_set<ecs::entity> seen;
        std::size_t visits = 0;
        {
            const auto work = sel.split(n);
            CHECK(work.parts() == n);
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
        CHECK(visits == expected.size());  // exactly once each
        CHECK(seen.size() == expected.size());
    }

    // Degenerates: n > size, n == 0, empty selection.
    {
        const auto tiny = w.select<TagB>();
        const auto work = tiny.split(10000);
        std::size_t visits = 0;
        for (std::size_t i = 0; i < work.parts(); ++i)
        {
            work.part(i).entities([&](ecs::entity) { ++visits; });
        }
        CHECK(visits == tiny.count());
        const auto zero = sel.split(0);
        CHECK(zero.parts() == 1);
        const auto none = w.select<EmptyPacked>().split(4);
        none.part(0).entities([](ecs::entity) { CHECK(false); });
        none.part(9).entities([](ecs::entity) { CHECK(false); });  // out of range: empty part
    }

    // maybe<> works through parts.
    {
        const auto msel = w.select<Pos, ecs::maybe<Vel>>();
        const auto work = msel.split(3);
        std::size_t with = 0;
        std::size_t without = 0;
        for (std::size_t i = 0; i < work.parts(); ++i)
        {
            work.part(i).each([&](Pos&, Vel* v) { (v != nullptr ? with : without) += 1; });
        }
        CHECK(with == 500);
        CHECK(without == 500);
    }

#if QUIVER_CHECKS
    {
        violation_scope guard;
        const auto work = sel.split(2);
        w.add<Pos>(w.spawn(), Pos{1});  // structural op while split lives
        CHECK(violations_seen == 1);
    }
#endif

    // Real threads (checked builds too: the umbrella owns all counter writes).
    {
        const auto par = w.select<Pos, const Vel>();
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
    CHECK_VALID(w);

    // Early exit stops one part, not the others.
    {
        const auto work = sel.split(2);
        std::size_t first_visits = 0;
        work.part(0).entities(
            [&](ecs::entity)
            {
                ++first_visits;
                return false;  // stop immediately
            });
        CHECK(first_visits == 1);
        std::size_t second_visits = 0;
        work.part(1).entities([&](ecs::entity) { ++second_visits; });
        CHECK(second_visits > 0);
    }
}

static void test_archives()
{
    section("archives: pack / unpack / graft");

    // Round-trip: mixed storage kinds, slot holes, recycled generations,
    // sorted order — into a fresh world with exact ids.
    byte_writer out;
    std::vector<ecs::entity> survivors;
    {
        ecs::world w;
        for (int i = 0; i < 10; ++i)
        {
            const ecs::entity e = w.spawn(Pos{i});
            if (i % 2 == 0)
            {
                w.add<Stable>(e, Stable{i * 10});
            }
            if (i % 3 == 0)
            {
                w.add<TagA>(e);
            }
        }
        // Holes + recycled slots:
        w.live_entities([&](ecs::entity e) { survivors.push_back(e); });
        w.kill(survivors[2]);
        w.kill(survivors[5]);
        w.spawn(Pos{100});  // recycles a slot with a bumped generation
        survivors.clear();
        w.live_entities([&](ecs::entity e) { survivors.push_back(e); });
        w.sort<Pos>([](const Pos& a, const Pos& b) { return a.x > b.x; });
        std::vector<int> order_before;
        w.each<const Pos>([&](const Pos& p) { order_before.push_back(p.x); });

        ecs::pack<Pos, Stable, TagA>(w, out);

        ecs::world fresh;
        byte_reader in{&out.data};
        const auto r = ecs::unpack<Pos, Stable, TagA>(fresh, in);
        CHECK(r.has_value());
        CHECK(fresh.live_count() == w.live_count());
        for (const ecs::entity e : survivors)
        {
            CHECK(fresh.alive(e));  // exact index+generation
            CHECK(fresh.has<Pos>(e) == w.has<Pos>(e));
            CHECK(fresh.has<Stable>(e) == w.has<Stable>(e));
            CHECK(fresh.has<TagA>(e) == w.has<TagA>(e));
            if (const Pos* p = fresh.find<Pos>(e))
            {
                CHECK(p->x == w.get<Pos>(e).x);
            }
        }
        std::vector<int> order_after;
        fresh.each<const Pos>([&](const Pos& p) { order_after.push_back(p.x); });
        CHECK(order_before == order_after);  // dense order round-trips
        CHECK_VALID(fresh);
    }

#if QUIVER_CHECKS
    // unpack refuses a non-empty world.
    {
        violation_scope guard;
        ecs::world busy;
        busy.spawn(Pos{1});
        byte_reader in{&out.data};
        const auto r = ecs::unpack<Pos, Stable, TagA>(busy, in);
        CHECK(!r.has_value());
        CHECK(r.error().code == ecs::fault_code::world_not_empty);
        CHECK(violations_seen == 1);
    }
#endif

    // Type-list mismatch faults cleanly.
    {
        ecs::world fresh;
        byte_reader in{&out.data};
        const auto r = ecs::unpack<Vel, Stable, TagA>(fresh, in);  // wrong first type
        CHECK(!r.has_value());
        CHECK(r.error().code == ecs::fault_code::archive_mismatch);
    }

    // graft: fresh ids into a populated world, relinked entity members.
    {
        ecs::world w;
        const ecs::entity leader = w.spawn(Pos{1});
        const ecs::entity grunt = w.spawn(Pos{2});
        w.add<Squad>(grunt, Squad{leader, 5});
        w.add<Squad>(leader, Squad{leader, 9});
        w.add<ForeignLink>(grunt, ForeignLink{leader});
        byte_writer save;
        ecs::pack<Pos, Squad, ForeignLink>(w, save);

        ecs::world host;
        host.spawn(Pos{777});  // already populated
        byte_reader in{&save.data};
        const auto grafted = ecs::graft<Pos, Squad, ForeignLink>(host, in);
        CHECK(grafted.has_value());
        const ecs::graft_map& map = *grafted;
        CHECK(map.size() == 2);
        const ecs::entity new_leader = map.resolve(leader);
        const ecs::entity new_grunt = map.resolve(grunt);
        CHECK(host.alive(new_leader) && host.alive(new_grunt));
        CHECK(new_leader != leader || new_grunt != grunt);       // fresh handles
        CHECK(host.get<Squad>(new_grunt).leader == new_leader);  // member relink
        CHECK(host.get<Squad>(new_leader).leader == new_leader);
        CHECK(host.get<ForeignLink>(new_grunt).target == new_leader);  // trait relink
        CHECK(host.get<Squad>(new_grunt).morale == 5);
        CHECK(map.resolve(ecs::no_entity) == ecs::no_entity);  // unknown -> no_entity
        std::size_t mapped = 0;
        map.each(
            [&](ecs::entity, ecs::entity fresh_e)
            {
                CHECK(host.alive(fresh_e));
                ++mapped;
            });
        CHECK(mapped == 2);
        CHECK_VALID(host);
    }

    // Orphan REFERENCE inside a component: resolve answers no_entity. The
    // referenced entity must be outside the ARCHIVE (the entity section holds
    // every live handle), so it dies before the pack.
    {
        ecs::world w;
        const ecs::entity outsider = w.spawn();
        const ecs::entity insider = w.spawn(Pos{1});
        w.add<Squad>(insider, Squad{outsider, 1});
        w.kill(outsider);  // the stored leader handle now dangles by design
        byte_writer save;
        ecs::pack<Pos, Squad>(w, save);

        ecs::world host;
        byte_reader in{&save.data};
        const auto grafted = ecs::graft<Pos, Squad>(host, in);
        CHECK(grafted.has_value());
        CHECK(host.get<Squad>(grafted->resolve(insider)).leader == ecs::no_entity);
    }

    // Bonded pools re-pair as unpack adds components.
    {
        ecs::world w;
        w.spawn(Pos{1}, Vel{1});
        w.spawn(Pos{2});
        byte_writer save;
        ecs::pack<Pos, Vel>(w, save);

        ecs::world fresh;
        auto view = fresh.bond<Pos, Vel>();
        byte_reader in{&save.data};
        CHECK((ecs::unpack<Pos, Vel>(fresh, in).has_value()));
        CHECK(view.count() == 1);
        CHECK_VALID(fresh);
    }
}

static void test_globals()
{
    section("globals: world singletons on a marked entity");
    ecs::world w;
    const std::size_t before = w.live_count();

    ecs::entity_ref g = w.globals();
    CHECK(g.alive());
    CHECK(w.live_count() == before + 1);  // visible, honest accounting
    g.obtain<Tint>(Tint{1}).color = 7;
    CHECK(w.globals().get<Tint>().color == 7);
    CHECK(w.globals().id() == g.id());  // stable across calls

#if QUIVER_CHECKS
    {
        violation_scope guard;
        w.kill(g.id());
        CHECK(violations_seen == 1);
        CHECK(g.alive());  // refused
        const ecs::duplicate_result dup = w.duplicate(g.id());
        CHECK(violations_seen == 2);
        CHECK(dup.clone == ecs::no_entity);
    }
#endif

    // Buffer kills funnel through world::kill -> skipped.
    {
        ecs::command_buffer cmd;
        cmd.kill(g.id());
        violation_scope guard;
        w.apply(cmd);
        CHECK(g.alive());
    }

    // reset() wipes it; the next globals() respawns lazily.
    const ecs::entity old_id = g.id();
    w.reset();
    CHECK(!w.alive(old_id));
    ecs::entity_ref fresh = w.globals();
    CHECK(fresh.alive());
    CHECK(!fresh.has<Tint>());  // state died with the reset

    // Archives: packing the mark lets the restored world FIND its globals.
    fresh.obtain<Tint>(Tint{42});
    byte_writer save;
    ecs::pack<ecs::globals_mark, Tint>(w, save);
    ecs::world loaded;
    byte_reader in{&save.data};
    CHECK((ecs::unpack<ecs::globals_mark, Tint>(loaded, in).has_value()));
    CHECK(loaded.globals().get<Tint>().color == 42);  // found, not respawned
    CHECK(loaded.live_count() == w.live_count());

    // const world: never spawns.
    {
        const ecs::world quiet;
        ecs::const_entity_ref cg = quiet.globals();
        CHECK(!cg.alive());
        CHECK(quiet.live_count() == 0);
    }

#if QUIVER_CHECKS
    // A second marked entity is a validate() fault.
    {
        ecs::world broken;
        static_cast<void>(broken.globals());
        broken.add<ecs::globals_mark>(broken.spawn());
        const auto r = broken.validate();
        CHECK(!r.has_value());
        CHECK(r.error().code == ecs::fault_code::globals_broken);
    }
#endif
    CHECK_VALID(w);
}

static void test_pipeline()
{
    section("pipeline: ordered stages with an auto-applied deferred buffer");
    ecs::world w;
    ecs::pipeline frame;
    std::vector<int> order;

    frame
        .stage("spawn",
               [&](ecs::world& world, float)
               {
                   world.spawn(Hp{1});
                   order.push_back(1);
               })
        .stage("damage",
               [&](ecs::world& world, float)
               {
                   order.push_back(2);
                   world.each<Hp>([&](ecs::entity e, Hp&) { frame.deferred().kill(e); });
               })
        .stage("audit",
               [&](ecs::world& world, float)
               {
                   order.push_back(3);
                   // The kill from the previous stage has already applied.
                   CHECK(world.select<Hp>().count() == 0);
               });
    CHECK(frame.stages() == 3);
    CHECK(frame.label(1) == "damage");

    frame.run(w, 0.16F);
    CHECK((order == std::vector<int>{1, 2, 3}));
    CHECK(w.live_count() == 0);

    // Reusable across frames; move-only stage callables work.
    auto big = std::make_unique<int>(5);
    frame.stage([big = std::move(big)](ecs::world&, float) { CHECK(*big == 5); });
    frame.run(w, 0.16F);
    CHECK(w.live_count() == 0);
    CHECK_VALID(w);
}

static void test_ref_conversions()
{
    section("basic_entity_ref: const conversion and constness flow");
    ecs::world w;
    const ecs::entity e = w.spawn(Pos{3});
    ecs::entity_ref mut = w.ref(e);
    ecs::const_entity_ref view = mut;  // converting constructor
    CHECK(view.alive());
    CHECK(view.get<Pos>().x == 3);
    static_assert(std::is_same_v<decltype(view.get<Pos>()), const Pos&>);
    static_assert(std::is_same_v<decltype(mut.get<Pos>()), Pos&>);
    static_assert(std::is_same_v<decltype(view.find<Pos>()), const Pos*>);
    static_assert(std::is_same_v<decltype(mut.find<Pos>()), Pos*>);
    mut.get<Pos>().x = 4;
    CHECK(view.get<Pos>().x == 4);
    CHECK_VALID(w);
}

static void test_review_regressions()
{
    section("review regressions: apply reentrancy, reset window, moves, globals");

    // A hook recording into the SAME buffer being applied: the new ops replay
    // in the same pass (no UAF — the ASan config proves it; no silent drop).
    {
        ecs::world w;
        ecs::command_buffer cmd;
        struct ctx_t
        {
            ecs::command_buffer* cmd = nullptr;
            int chained = 0;
        } ctx{&cmd, 0};
        const ecs::hook_token t = w.on_add<Pos>(
            [](ecs::world&, ecs::entity e, void* user)
            {
                auto* c = static_cast<ctx_t*>(user);
                if (c->chained < 8)
                {
                    ++c->chained;
                    c->cmd->add<Vel>(e, Vel{c->chained});  // mid-apply record
                }
            },
            &ctx);
        std::vector<ecs::entity> spawned;
        for (int i = 0; i < 8; ++i)
        {
            const ecs::entity e = w.spawn();
            spawned.push_back(e);
            cmd.add<Pos>(e, Pos{i});
        }
        const ecs::apply_result r = w.apply(cmd);
        CHECK(r.applied == 16);  // 8 recorded + 8 chained mid-apply
        for (const ecs::entity e : spawned)
        {
            CHECK(w.has<Vel>(e));  // the chained ops really ran
        }
        CHECK(cmd.empty());  // and were cleared with the rest
        CHECK(w.unhook(t));
        CHECK_VALID(w);
    }

    // A mid-apply chained op may even target a mid-apply provisional spawn.
    {
        ecs::world w;
        ecs::command_buffer cmd;
        const ecs::hook_token t = w.on_add<Hp>(
            [](ecs::world& world, ecs::entity, void* user)
            {
                auto* c = static_cast<ecs::command_buffer*>(user);
                if (world.select<TagA>().count() == 0)
                {
                    const ecs::entity child = c->spawn();
                    c->add<TagA>(child);
                }
            },
            &cmd);
        cmd.add<Hp>(w.spawn(), Hp{1});
        w.apply(cmd);
        CHECK(w.select<TagA>().count() == 1);
        CHECK(w.unhook(t));
        CHECK_VALID(w);
    }

    // reset(): a hook reading a bonded view mid-reset sees an EMPTY partition
    // (no stale mirror over a wiped partner), and validate stays green.
    {
        ecs::world w;
        auto view = w.bond<Pos, Vel>();
        w.spawn(Pos{1}, Vel{1});
        w.spawn(Pos{2}, Vel{2});
        struct probe_t
        {
            std::size_t seen = 999;
            bool valid_mid_reset = false;
        } probe;
        const ecs::hook_token t = w.on_remove<Vel>(
            [](ecs::world& world, ecs::entity, void* user)
            {
                auto* p = static_cast<probe_t*>(user);
                p->seen = world.bonded<Pos, Vel>().count();
                p->valid_mid_reset = world.validate().has_value();
            },
            &probe);
        w.reset();
        CHECK(probe.seen == 0);  // partition zeroed before the wipes
        CHECK(probe.valid_mid_reset);
        CHECK(view.count() == 0);
        CHECK(w.unhook(t));
        CHECK_VALID(w);
    }

#if QUIVER_CHECKS
    // pipeline: registering a stage during run() is refused.
    {
        violation_scope guard;
        ecs::world w;
        ecs::pipeline frame;
        frame.stage("self-grow",
                    [&frame](ecs::world&, float) { frame.stage([](ecs::world&, float) {}); });
        frame.run(w, 0.0F);
        CHECK(violations_seen == 1);
        CHECK(frame.stages() == 1);              // refused, not deferred
        frame.stage([](ecs::world&, float) {});  // fine between runs
        CHECK(frame.stages() == 2);
    }
#endif

    // scoped_hook anchors to the POOL: it still releases after a world move.
    {
        int fires = 0;
        ecs::world a;
        ecs::scoped_hook guard(
            a,
            a.on_add<Pos>([](ecs::world&, ecs::entity, void* user) { ++*static_cast<int*>(user); },
                          &fires));
        ecs::world b = std::move(a);
        b.spawn(Pos{1});
        CHECK(fires == 1);  // the connection moved with the pools
        guard.release();    // and the pool-anchored release finds it
        b.spawn(Pos{2});
        CHECK(fires == 1);
    }

    // The same property keeps a tracker safe across a world move: its hooks
    // disconnect in the destination's pools when it dies (ASan would catch a
    // dangling dispatch).
    {
        ecs::world a;
        ecs::world b;
        {
            ecs::tracker<Hp> moved_under(a);
            b = std::move(a);
            b.spawn(Hp{1});
            CHECK(moved_under.added().size() == 1);
        }
        b.spawn(Hp{2});  // no tracker left: must not dispatch into freed memory
    }

#if QUIVER_CHECKS
    // globals: the kill/duplicate refusal is membership-based, so the
    // restored entity is protected BEFORE the first globals() call.
    {
        ecs::world w;
        w.globals().obtain<Tint>(Tint{5});
        byte_writer save;
        ecs::pack<ecs::globals_mark, Tint>(w, save);

        ecs::world loaded;
        byte_reader in{&save.data};
        CHECK((ecs::unpack<ecs::globals_mark, Tint>(loaded, in).has_value()));
        const ecs::entity restored = loaded.select<ecs::globals_mark>().first();
        violation_scope guard;
        loaded.kill(restored);  // cold cache: still refused
        CHECK(violations_seen == 1);
        CHECK(loaded.alive(restored));
        const ecs::duplicate_result dup = loaded.duplicate(restored);
        CHECK(violations_seen == 2);
        CHECK(dup.clone == ecs::no_entity);
        CHECK_VALID(loaded);
    }

    // globals: a refused lazy mark add self-heals on the next call.
    {
        violation_scope guard;
        ecs::world w;
        ecs::entity carrier;
        {
            const auto marks = w.select<ecs::globals_mark>();
            const auto lock = marks.range();  // holds the mark pool's lock
            carrier = w.globals().id();       // spawn ok; mark add refused
            CHECK(violations_seen == 1);
            CHECK(!w.has<ecs::globals_mark>(carrier));
        }
        const ecs::entity healed = w.globals().id();  // re-adds the mark
        CHECK(healed == carrier);
        CHECK(w.has<ecs::globals_mark>(carrier));
        CHECK_VALID(w);
    }
#endif

    // tracker: removed() keeps history across slot recycling.
    {
        ecs::world w;
        ecs::tracker<Hp> deaths(w, ecs::track::removed);
        const ecs::entity first = w.spawn(Hp{1});
        w.kill(first);
        const ecs::entity second = w.spawn(Hp{2});  // recycles the slot
        CHECK(second.index() == first.index());
        w.remove<Hp>(second);
        CHECK(deaths.removed().size() == 2);  // both entities' records survive
        CHECK(deaths.removed()[0] == first);
        CHECK(deaths.removed()[1] == second);
        deaths.clear();
    }

    // Mixed-const bonds compile and propagate constness into callbacks.
    {
        ecs::world w;
        auto view = w.bond<Pos, const Vel>();
        w.spawn(Pos{1}, Vel{2});
        int sum = 0;
        view.each([&](Pos& p, const Vel& v) { sum = p.x + v.v; });
        CHECK(sum == 3);
        CHECK((w.bonded<const Pos, const Vel>().count() == 1));
        CHECK_VALID(w);
    }
}

static void test_genericity_seams()
{
    section("genericity: custom pools, pinned labels, raw access");

    // The bond swappability wall covers DERIVED packed pools too (regression
    // for the same_as -> derived_from guard fix).
    static_assert(std::derived_from<ecs::pool_of_t<Telemetry>, ecs::packed_pool_of<Telemetry>>);

    // A custom storage backend registered through pool_of<T>: every layer of
    // the library drives it unchanged.
    {
        static_assert(std::same_as<ecs::pool_of_t<Telemetry>, telemetry_pool>);
        telemetry_pool::erases = 0;
        telemetry_pool::wipes = 0;
        ecs::world w;
        int hook_fires = 0;
        const ecs::hook_token t = w.on_add<Telemetry>(
            [](ecs::world&, ecs::entity, void* user) { ++*static_cast<int*>(user); }, &hook_fires);
        for (int i = 0; i < 6; ++i)
        {
            const ecs::entity e = w.spawn(Telemetry{i});
            if (i % 2 == 0)
            {
                w.add<TagA>(e);
            }
        }
        CHECK(hook_fires == 6);  // hooks dispatch through the custom pool

        int sum = 0;
        w.each<const Telemetry>([&](const Telemetry& tl) { sum += tl.v; });
        CHECK(sum == 15);  // selections drive it

        auto view = w.bond<Telemetry, TagA>();  // bonds partition it
        CHECK(view.count() == 3);
        CHECK((w.unbond<Telemetry, TagA>()));

        w.sort<Telemetry>([](const Telemetry& a, const Telemetry& b)
                          { return a.v > b.v; });  // sorting drives it too
        int first_v = -1;
        w.select<const Telemetry>().each(
            [&](const Telemetry& tl)
            {
                first_v = tl.v;
                return false;
            });
        CHECK(first_v == 5);

        byte_writer save;
        ecs::pack<Telemetry>(w, save);  // archives walk it
        ecs::world fresh;
        byte_reader in{&save.data};
        CHECK((ecs::unpack<Telemetry>(fresh, in).has_value()));
        CHECK(fresh.select<const Telemetry>().count() == 6);

        w.remove<Telemetry>(w.select<const Telemetry>().first());
        CHECK(telemetry_pool::erases >= 1);  // the override observed it
        w.reset();
        CHECK(telemetry_pool::wipes >= 1);
        CHECK_VALID(w);
        CHECK(w.unhook(t));
    }

    // A stable-derived custom backend: pointer stability + the override both
    // hold through churn, bonds, and archives.
    {
        audit_pool::erases = 0;
        ecs::world w;
        const ecs::entity a = w.spawn();
        Audit* pinned = &w.add<Audit>(a, Audit{1});
        for (int i = 0; i < 16; ++i)
        {
            const ecs::entity e = w.spawn();
            w.add<Audit>(e, Audit{i});
            if (i % 2 == 0)
            {
                w.add<TagB>(e);
            }
        }
        CHECK(pinned == w.find<Audit>(a));  // stable storage stays stable
        auto view = w.bond<Audit, TagB>();
        CHECK(view.count() == 8);
        CHECK(pinned == w.find<Audit>(a));  // bond swaps move bookkeeping only

        byte_writer save;
        ecs::pack<Audit>(w, save);
        ecs::world fresh;
        byte_reader in{&save.data};
        CHECK((ecs::unpack<Audit>(fresh, in).has_value()));
        CHECK(fresh.select<const Audit>().count() == 17);

        w.remove<Audit>(a);
        CHECK(audit_pool::erases >= 1);
        CHECK_VALID(w);
    }

    // Pinned identity: labels replace the compiler-derived spelling, so the
    // persisted hash is portable across toolchains.
    {
        static_assert(ecs::name_of<LabeledA>() == "test.labeled_a");
        static_assert(ecs::name_of<ForeignVec>() == "test.foreign_vec");
        static_assert(ecs::hash_of<LabeledA>() != ecs::hash_of<ForeignVec>());
        ecs::world w;
        w.spawn(LabeledA{7});
        CHECK(w.find_pool<LabeledA>().name() == "test.labeled_a");
        CHECK(static_cast<bool>(w.find_pool_by_hash(ecs::hash_of<LabeledA>())));

        byte_writer save;
        ecs::pack<LabeledA>(w, save);  // keyed by the pinned hash
        ecs::world fresh;
        byte_reader in{&save.data};
        CHECK((ecs::unpack<LabeledA>(fresh, in).has_value()));
        CHECK(fresh.get<LabeledA>(fresh.select<const LabeledA>().first()).v == 7);
    }

#if QUIVER_CHECKS
    // Two distinct types sharing a label collide at registration — loudly.
    {
        violation_scope guard;
        ecs::world w;
        w.spawn(DupLabel1{1});
        w.spawn(DupLabel2{2});
        CHECK(violations_seen == 1);
    }
#endif

    // Type-erased payload access: the bytes behind any component, no
    // templates — what generic editors build on.
    {
        ecs::world w;
        const ecs::entity e = w.spawn(Pos{41});
        w.add<TagA>(e);
        w.add<Stable>(e, Stable{5});

        const ecs::pool_ref positions = w.find_pool<Pos>();
        CHECK(positions.info().bytes_per_item == sizeof(Pos));
        void* bytes = positions.raw(e);
        CHECK(bytes != nullptr);
        int v = 0;
        std::memcpy(&v, bytes, sizeof(int));
        CHECK(v == 41);
        const int next = 42;  // a property-grid write
        std::memcpy(bytes, &next, sizeof(int));
        CHECK(w.get<Pos>(e).x == 42);

        CHECK(w.find_pool<Stable>().raw(e) != nullptr);   // stable pools too
        CHECK(w.find_pool<TagA>().raw(e) == nullptr);     // tags carry no bytes
        CHECK(positions.raw(ecs::no_entity) == nullptr);  // dead/stale answer null
        const ecs::entity ghost = w.spawn();
        w.kill(ghost);
        CHECK(positions.raw(ghost) == nullptr);

        // Dense walk: entity_at + raw_at pair up for whole-pool dumps.
        std::size_t walked = 0;
        for (std::size_t pos = 0; pos < positions.size(); ++pos)
        {
            CHECK(positions.raw_at(pos) != nullptr);
            CHECK(positions.entity_at(pos) != ecs::no_entity);
            ++walked;
        }
        CHECK(walked == positions.size());
        CHECK(positions.raw_at(positions.size()) == nullptr);  // out of range
        CHECK_VALID(w);
    }
}

static void test_compile_time_toolkit()
{
    section("compile-time toolkit: types<>, values<>, manifests, introspection");

    // The list algebra, all at compile time.
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

    // The value-side mirror, exact types preserved.
    using V = ecs::values<1, 2U, std::size_t{7}>;
    static_assert(V::size == 3);
    static_assert(ecs::value_at<0>(V{}) == 1);
    static_assert(std::same_as<decltype(ecs::value_at<1>(V{})), unsigned>);
    static_assert(ecs::contains_value<2U, V>);
    static_assert(!ecs::contains_value<9, V>);
    static_assert(ecs::index_of_value<std::size_t{7}>(V{}) == 2);

    // for_each_type: the runtime fold over a list.
    {
        std::vector<std::string_view> names;
        ecs::for_each_type(L{}, [&]<class T>() { names.push_back(ecs::name_of<T>()); });
        CHECK(names.size() == 3);
        CHECK(names[0] == ecs::name_of<Pos>());
        CHECK(names[2] == ecs::name_of<TagA>());
    }

    // Selections from manifests, and list introspection back out of them.
    {
        ecs::world w;
        for (int i = 0; i < 6; ++i)
        {
            const ecs::entity e = w.spawn(Pos{i});
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
        const auto sel = w.select(Movers{}, ecs::except<TagB>{});
        static_assert(std::same_as<decltype(sel)::included, Movers>);
        static_assert(std::same_as<decltype(sel)::excluded, ecs::types<TagB>>);
        std::size_t manifest_count = 0;
        sel.each([&](Pos&, const Vel&) { ++manifest_count; });
        CHECK(manifest_count == sel.count());
        CHECK((sel.count() == w.select<Pos, const Vel>(ecs::except<TagB>{}).count()));

        std::size_t each_count = 0;
        w.each(ecs::types<Pos>{}, [&](Pos&) { ++each_count; });
        CHECK(each_count == 6);
        w.each(Movers{}, ecs::except<TagB>{}, [&](Pos&, const Vel&) { --each_count; });
        CHECK(each_count == 6 - manifest_count);

        // Generic code over ANY selection type via its exported lists.
        constexpr std::size_t arity = decltype(sel)::included::size;
        static_assert(arity == 2);

        auto bview = w.bond<Pos, Vel>();
        static_assert(std::same_as<decltype(bview)::pair, ecs::types<Pos, Vel>>);
        static_cast<void>(bview);
    }

    // Archive manifests: name the set once, reuse everywhere.
    {
        using Saved = ecs::types<Pos, Stable, TagA>;
        ecs::world w;
        const ecs::entity e = w.spawn(Pos{3});
        w.add<Stable>(e, Stable{4});
        w.add<TagA>(e);

        byte_writer save;
        ecs::pack(w, save, Saved{});
        ecs::world fresh;
        byte_reader in{&save.data};
        CHECK(ecs::unpack(fresh, in, Saved{}).has_value());
        CHECK(fresh.get<Pos>(e).x == 3);
        CHECK(fresh.has<TagA>(e));

        ecs::world host;
        byte_reader in2{&save.data};
        const auto grafted = ecs::graft(host, in2, Saved{});
        CHECK(grafted.has_value());
        CHECK(host.get<Pos>(grafted->resolve(e)).x == 3);
        CHECK_VALID(host);
    }
}

static void test_world_ops_and_inspection()
{
    section("purge, shrink, counters, inspector, multiple worlds");
    ecs::world w;
    for (int i = 0; i < 32; ++i)
    {
        w.spawn(Pos{i}, Vel{i});
    }
    w.purge<Vel>();
    CHECK(w.select<Vel>().count() == 0);
    CHECK(w.select<Pos>().count() == 32);  // other pools untouched
    CHECK(w.live_count() == 32);           // purging components kills no entities

    for (int i = 0; i < 16; ++i)
    {
        w.kill(w.current_handle(static_cast<std::uint32_t>(i)));
    }
    CHECK(w.live_count() == 16);
    CHECK(w.slot_count() == 32);
    w.shrink();
    CHECK(w.live_count() == 16);  // shrink frees slack, changes nothing visible
    CHECK(w.select<Pos>().count() == 16);
    CHECK_VALID(w);

    bool saw_pos = false;
    w.each_pool(
        [&](const ecs::pool_info& info)
        {
            if (info.name == ecs::name_of<Pos>())
            {
                saw_pos = true;
                CHECK(info.size == 16);
                CHECK(info.bytes_per_item == sizeof(Pos));
                CHECK(info.kind == ecs::storage::packed);
            }
        });
    CHECK(saw_pos);
    CHECK(ecs::name_of<Pos>() == "Pos");

    // Two worlds registering types in different orders both index correctly.
    ecs::world first;
    ecs::world second;
    first.spawn(OnlyHere1{1});
    second.spawn(OnlyHere2{2});
    second.spawn(OnlyHere1{3});
    first.spawn(OnlyHere2{4});
    CHECK(first.select<OnlyHere1>().count() == 1);
    CHECK(first.select<OnlyHere2>().count() == 1);
    CHECK(second.select<OnlyHere1>().count() == 1);
    CHECK(second.select<OnlyHere2>().count() == 1);
    CHECK_VALID(first);
    CHECK_VALID(second);
}

// NOLINTNEXTLINE(bugprone-exception-escape) — probe exceptions are caught at their throw sites
int main()
{
    test_entity_basics();
    test_lifecycle_and_reuse();
    test_component_basics();
    test_destructor_balance_packed();
    test_tags();
    test_move_only();
    test_stable_storage();
    test_iteration();
    test_filters_and_driver();
    test_selection_reuse_and_const_world();
    test_command_buffer_basics();
    test_command_buffer_spawn();
    test_command_buffer_payloads();
#if QUIVER_CHECKS
    test_violations();
    test_negative_validation();
#else
    std::printf("--- (QUIVER_CHECKS off: violation and corruption tests skipped)\n");
#endif
    test_relationships();
    test_snapshot();
    test_move_semantics();
    test_iteration_lock_unwinding();
#if QUIVER_CHECKS
    test_tag_add_refused_during_iteration();
#endif
    test_non_movable_components();
    test_overaligned_command_payloads();
    test_apply_spawn_callback();
    test_hash_identity();
    test_range_iteration();
    test_maybe_components();
    test_sort();
    test_query_helpers();
    test_hooks();
    test_blueprint();
    test_duplicate();
    test_entity_ref();
    test_obtain_and_find_all();
    test_runtime_queries();
    test_sort_along();
    test_chunk_capacity_and_footprint();
    test_pmr_support();
    test_sort_non_involution();
    test_hook_edge_cases();
    test_arena_warm_overaligned();
    test_runtime_selection_self_mutation();
    test_empty_entity_ref();
    test_hook_connection_forms();
    test_tracker();
    test_bonded_pairs();
    test_split();
    test_archives();
    test_globals();
    test_pipeline();
    test_ref_conversions();
    test_review_regressions();
    test_genericity_seams();
    test_compile_time_toolkit();
    test_world_ops_and_inspection();
    test_has_all_any();
    test_amend();
    test_driven_by();
    test_sort_algorithm();
    test_custom_pool_from_scratch();
    test_bond_n_ary();
    test_bond_n_ary_paths();
#if QUIVER_CHECKS
    test_bond_n_ary_violations();
#endif
    test_bond_n_ary_unbond();
    test_bond_observed_views();
    test_bond_partition_sort();
    test_bond_view_count();
    test_any_of_basics();
    test_any_of_driving();
    test_any_of_composition();
#if QUIVER_CHECKS
    test_any_of_violations();
#endif

    std::printf("%d checks, %d failed\n", checks_run, checks_failed);
    return checks_failed == 0 ? 0 : 1;
}
