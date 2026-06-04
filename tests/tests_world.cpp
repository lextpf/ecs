// ============================================================================
// tests_world.cpp — membership, mutation, and ordering parity (M1): variadic
// has_all/has_any, amend, driven_by, sort algorithm injection, and the
// from-scratch custom-pool worked example. Registered in tests.cpp's main().
// ============================================================================

#include "test_harness.hpp"

#include <algorithm>

// Tied sort keys: `group` collides on purpose so only a stable algorithm
// pins the order within a tie class.
struct SortKey
{
    int group = 0;
    int seq = 0;
};

struct SortKeyStable
{
    int group = 0;
    int seq = 0;
    static constexpr auto quiver_storage = ecs::storage::stable;
};

// ------------------------------------------------ from-scratch custom pool
// The executable spec of the pool_of seam's FROM-SCRATCH path: derive
// quiver::basic_pool directly (not a built-in pool), implement the typed
// surface (emplace / at / at_pos / sort_dense / swap_positions / reserve)
// plus the cold virtuals, and every world feature — verbs, selections,
// hooks, sorting, bonds, duplication, inspection — keeps working.
//
// The layout is deliberately NOT one of the built-ins: each payload lives in
// its own heap box, so component pointers are stable and sorting swaps
// pointers only.
template <quiver::component T>
class boxed_pool : public quiver::basic_pool
{
public:
    explicit boxed_pool(std::pmr::memory_resource* memory) noexcept
        : quiver::basic_pool(
              memory, quiver::name_of<T>(), quiver::hash_of<T>(), ecs::storage::stable, sizeof(T)),
          boxes_(memory)
    {
    }

    ~boxed_pool() override
    {
        for (T* box : boxes_)
        {
            delete box;
        }
    }

    template <class... Args>
    T& emplace(ecs::entity e, Args&&... args)
    {
        boxes_.reserve(boxes_.size() + 1);
        auto box = std::make_unique<T>(std::forward<Args>(args)...);
        boxes_.push_back(box.get());
        box.release();
        attach(e);
        fire_add(e);
        return *boxes_[sparse_.get(e.index())];
    }

    [[nodiscard]] T* at(std::uint32_t index) noexcept
    {
        const std::uint32_t pos = sparse_.get(index);
        return pos == quiver::detail::npos32 ? nullptr : boxes_[pos];
    }

    [[nodiscard]] const T* at(std::uint32_t index) const noexcept
    {
        const std::uint32_t pos = sparse_.get(index);
        return pos == quiver::detail::npos32 ? nullptr : boxes_[pos];
    }

    [[nodiscard]] T& at_pos(std::uint32_t pos) noexcept { return *boxes_[pos]; }
    [[nodiscard]] const T& at_pos(std::uint32_t pos) const noexcept { return *boxes_[pos]; }

    [[nodiscard]] void* item_address(std::uint32_t pos) noexcept override { return boxes_[pos]; }

    template <class PosCompare, class Algo = quiver::detail::std_sort>
    void sort_dense(PosCompare cmp, Algo&& algo = {})
    {
        sort_dense_impl(
            cmp,
            [this](std::uint32_t a, std::uint32_t b) { std::swap(boxes_[a], boxes_[b]); },
            std::forward<Algo>(algo));
    }

    bool copy_item(std::uint32_t src_index, ecs::entity dst) override
    {
        if constexpr (std::copy_constructible<T>)
        {
            T detached(*boxes_[sparse_.get(src_index)]);
            emplace(dst, std::move(detached));
            return true;
        }
        else
        {
            return false;
        }
    }

    void swap_positions(std::uint32_t a, std::uint32_t b)
    {
        swap_dense(a, b);
        std::swap(boxes_[a], boxes_[b]);
    }

    void mirror_swap(std::uint32_t a, std::uint32_t b) noexcept override { swap_positions(a, b); }

    void erase_if_present(std::uint32_t index) noexcept override
    {
        if (!contains(index))
        {
            return;
        }
        fire_remove(dense_[sparse_.get(index)]);
        const std::uint32_t pos = detach(index);
        delete boxes_[pos];
        boxes_[pos] = boxes_.back();
        boxes_.pop_back();
    }

    void wipe() noexcept override
    {
        fire_remove_all();
        for (T* box : boxes_)
        {
            delete box;
        }
        boxes_.clear();
        dense_.clear();
        sparse_.clear();
    }

    void compact() override { boxes_.shrink_to_fit(); }

    void reserve(std::size_t n)
    {
        boxes_.reserve(n);
        dense_.reserve(n);
    }

    [[nodiscard]] std::size_t item_capacity() const noexcept override { return boxes_.capacity(); }

    [[nodiscard]] std::expected<void, quiver::fault> check(
        const quiver::detail::entity_table& table) const override
    {
        if (boxes_.size() != dense_.size())
        {
            return std::unexpected(
                quiver::fault{quiver::fault_code::sparse_dense_desync, name(), "box count"});
        }
        return check_membership(table);
    }

private:
    std::pmr::vector<T*> boxes_;
};

// The component routed into the from-scratch pool, plus a Counted variant
// for lifetime balance.
struct Crate
{
    int v = 0;
};

struct CrateCounted : Counted
{
    using Counted::Counted;
};

template <>
struct quiver::pool_of<Crate>
{
    using type = boxed_pool<Crate>;
};

template <>
struct quiver::pool_of<CrateCounted>
{
    using type = boxed_pool<CrateCounted>;
};

void test_has_all_any()
{
    section("has_all / has_any: variadic membership");
    ecs::world w;

    const ecs::entity e = w.spawn(Pos{1}, Vel{2});
    const ecs::entity lone = w.spawn(Pos{3});

    // has_all: every listed component present.
    CHECK((w.has_all<Pos, Vel>(e)));
    CHECK((!w.has_all<Pos, Vel>(lone)));
    CHECK((!w.has_all<Pos, Vel, Hp>(e)));  // Hp pool never registered: absent

    // has_any: at least one present.
    CHECK((w.has_any<Pos, Hp>(lone)));
    CHECK((!w.has_any<Vel, Hp>(lone)));

    // Tags are pure membership — exactly what these probes answer.
    w.add<TagA>(e);
    CHECK((w.has_all<Pos, TagA>(e)));
    CHECK((w.has_any<TagB, TagA>(e)));
    CHECK((!w.has_any<TagB, Hp>(e)));

    // Dead, stale, and null handles: false, never a violation.
    const ecs::entity dead = w.spawn(Pos{9});
    w.kill(dead);
    CHECK((!w.has_all<Pos, Vel>(dead)));
    CHECK((!w.has_any<Pos, Vel>(dead)));
    CHECK((!w.has_all<Pos, Vel>(ecs::no_entity)));
    CHECK((!w.has_any<Pos, Vel>(ecs::no_entity)));

    // A const world serves the same probes.
    const ecs::world& cw = w;
    CHECK((cw.has_all<Pos, Vel>(e)));
    CHECK((cw.has_any<Hp, Vel>(e)));

    CHECK_VALID(w);
}

void test_amend()
{
    section("amend: observed in-place mutation");
    ecs::world w;

    const ecs::entity e = w.spawn(Pos{1});

    // Mutates in place and returns the same component.
    Pos& p = w.amend<Pos>(e, [](Pos& pos) { pos.x += 41; });
    CHECK(p.x == 42);
    CHECK(w.find<Pos>(e) == &p);

    // on_replace fires exactly once, after the callable has run.
    struct seen
    {
        int fired = 0;
        int value_at_fire = -1;
    } s;
    const ecs::hook_token t = w.on_replace<Pos>(
        [](ecs::world& world, ecs::entity who, void* user)
        {
            auto* k = static_cast<seen*>(user);
            ++k->fired;
            k->value_at_fire = world.get<Pos>(who).x;
        },
        &s);
    w.amend<Pos>(e, [](Pos& pos) { pos.x = 7; });
    CHECK(s.fired == 1);
    CHECK(s.value_at_fire == 7);
    w.unhook(t);

    // The tracker's replaced() channel collects amends like replace/put.
    {
        ecs::tracker<Pos> moved(w);
        w.amend<Pos>(e, [](Pos& pos) { pos.x = 8; });
        CHECK(moved.replaced().size() == 1);
        CHECK(moved.replaced()[0] == e);
    }

    // Not structural: legal during iteration over the same pool, including
    // on the entity currently visited. The lock unwinds with the loop.
    const ecs::entity f = w.spawn(Pos{100});
    int visited = 0;
    w.select<Pos>().each(
        [&](ecs::entity who, Pos&)
        {
            ++visited;
            w.amend<Pos>(who, [](Pos& pos) { pos.x += 1; });
        });
    CHECK(visited == 2);
    CHECK(w.get<Pos>(e).x == 9);
    CHECK(w.get<Pos>(f).x == 101);
    w.sort<Pos>([](const Pos& a, const Pos& b) { return a.x < b.x; });  // lock released

    // No copies: the callable works on the live component.
    {
        const ecs::entity c = w.spawn(Counted{5});
        const int after_spawn = Counted::total_ctors;
        w.amend<Counted>(c, [](Counted& k) { k.value = 6; });
        CHECK(Counted::total_ctors == after_spawn);
        CHECK(w.get<Counted>(c).value == 6);
    }

    CHECK_VALID(w);
}

void test_driven_by()
{
    section("driven_by: manual query driver override");
    ecs::world w;

    const ecs::entity a = w.spawn(Pos{3}, Vel{30});
    const ecs::entity b = w.spawn(Pos{1}, Vel{10});
    const ecs::entity c = w.spawn(Pos{2}, Vel{20});
    w.spawn(Pos{99});  // Pos-only: matched only once Vel turns optional below

    // Pos (4) outnumbers Vel (3): the default driver is Vel. Order the big
    // pool, then force it to drive — sort<T> + driven_by<T> is quiver's
    // ordered multi-component iteration.
    w.sort<Pos>([](const Pos& l, const Pos& r) { return l.x < r.x; });

    const auto sel = w.select<Pos, Vel>();
    const auto by_pos = sel.driven_by<Pos>();

    std::vector<int> xs;
    by_pos.each([&](Pos& p, Vel&) { xs.push_back(p.x); });
    CHECK(xs.size() == 3);
    CHECK(xs[0] == 1 && xs[1] == 2 && xs[2] == 3);

    // The override changes WHICH pool drives, never the match set.
    std::size_t matched = 0;
    sel.each([&](Pos&, Vel&) { ++matched; });
    CHECK(matched == 3);
    CHECK(by_pos.count() == sel.count());
    CHECK(by_pos.contains(a) && by_pos.contains(b) && by_pos.contains(c));

    // split carves the OVERRIDDEN driver's dense order: walking the parts in
    // order reproduces the sorted sequence.
    {
        std::vector<int> from_parts;
        const auto work = by_pos.split(2);
        for (std::size_t i = 0; i < work.parts(); ++i)
        {
            work.part(i).each([&](Pos& p, Vel&) { from_parts.push_back(p.x); });
        }
        CHECK(from_parts == xs);
    }

    // Composes with except<> and maybe<>; the override still orders the loop.
    w.add<TagA>(b);
    std::vector<int> filtered;
    w.select<Pos, ecs::maybe<Vel>>(ecs::except<TagA>{})
        .driven_by<Pos>()
        .each([&](Pos& p, Vel*) { filtered.push_back(p.x); });
    CHECK(filtered.size() == 3);
    CHECK(filtered[0] == 2 && filtered[1] == 3 && filtered[2] == 99);

    // Override on a const world whose pools never registered: empty, no
    // violation.
    const ecs::world cold;
    std::size_t cold_visits = 0;
    cold.select<Pos, Vel>().driven_by<Pos>().each([&](const Pos&, const Vel&) { ++cold_visits; });
    CHECK(cold_visits == 0);

    CHECK_VALID(w);
}

void test_sort_algorithm()
{
    section("sort: algorithm injection");
    ecs::world w;

    // Scrambled spawn order with tied groups. std::stable_sort must keep
    // spawn order within each tie class — the default std::sort guarantees
    // no such thing.
    for (int i = 0; i < 6; ++i)
    {
        w.spawn(SortKey{i % 2, i}, SortKeyStable{i % 2, i});
    }

    w.sort<SortKey>([](const SortKey& l, const SortKey& r) { return l.group < r.group; },
                    stable_algo{});

    std::vector<std::pair<int, int>> order;
    w.select<SortKey>().each([&](SortKey& k) { order.emplace_back(k.group, k.seq); });
    const std::vector<std::pair<int, int>> expected{{0, 0}, {0, 2}, {0, 4}, {1, 1}, {1, 3}, {1, 5}};
    CHECK(order == expected);

    // Stable pools permute bookkeeping only: payload pointers survive the
    // injected algorithm exactly like the default one.
    const ecs::entity probe = w.select<SortKeyStable>().first();
    const SortKeyStable* before = w.find<SortKeyStable>(probe);
    w.sort<SortKeyStable>([](const SortKeyStable& l, const SortKeyStable& r)
                          { return l.group < r.group; },
                          stable_algo{});
    CHECK(w.find<SortKeyStable>(probe) == before);

    std::vector<std::pair<int, int>> stable_order;
    w.select<SortKeyStable>().each([&](SortKeyStable& k)
                                   { stable_order.emplace_back(k.group, k.seq); });
    CHECK(stable_order == expected);

    // The entity-comparator form takes the algorithm too.
    w.sort<SortKey>([](ecs::entity l, ecs::entity r) { return l.index() < r.index(); },
                    stable_algo{});
    std::vector<int> seqs;
    w.select<SortKey>().each([&](SortKey& k) { seqs.push_back(k.seq); });
    const std::vector<int> spawn_order{0, 1, 2, 3, 4, 5};
    CHECK(seqs == spawn_order);

#if QUIVER_CHECKS
    // The refusal rules are the algorithm-blind ones.
    {
        violation_scope guard;
        w.select<SortKey>().each(
            [&](ecs::entity, SortKey&)
            {
                w.sort<SortKey>([](const SortKey& l, const SortKey& r) { return l.seq < r.seq; },
                                stable_algo{});
                return false;
            });
        CHECK(violations_seen == 1);
        std::vector<int> after;
        w.select<SortKey>().each([&](SortKey& k) { after.push_back(k.seq); });
        CHECK(after == spawn_order);  // refused: order untouched
    }
#endif

    CHECK_VALID(w);
}

void test_custom_pool_from_scratch()
{
    section("pool_of: from-scratch backend (boxed pool)");
    ecs::world w;

    // Verbs flow through the custom emplace/at surface.
    const ecs::entity a = w.spawn(Pos{1});
    Crate& c = w.add<Crate>(a, Crate{10});
    CHECK(c.v == 10);
    CHECK(w.has<Crate>(a));
    CHECK(w.get<Crate>(a).v == 10);

    // The boxed layout's own property: pointers survive further adds.
    const Crate* pinned = w.find<Crate>(a);
    for (int i = 0; i < 64; ++i)
    {
        w.add<Crate>(w.spawn(), Crate{100 + i});
    }
    CHECK(w.find<Crate>(a) == pinned);

    // replace / amend / put / obtain / remove drive the same surface.
    w.replace<Crate>(a, Crate{11});
    w.amend<Crate>(a, [](Crate& k) { k.v += 1; });
    CHECK(w.get<Crate>(a).v == 12);
    w.put<Crate>(a, Crate{13});
    CHECK(w.obtain<Crate>(a).v == 13);
    CHECK(w.remove<Crate>(a));
    CHECK(!w.has<Crate>(a));
    w.add<Crate>(a, Crate{20});

    // Hooks fire from the custom pool's emplace/erase paths.
    struct hook_counts
    {
        int added = 0;
        int removed = 0;
    } hc;
    const ecs::hook_token ta = w.on_add<Crate>([](ecs::world&, ecs::entity, void* user)
                                               { ++static_cast<hook_counts*>(user)->added; },
                                               &hc);
    const ecs::hook_token tr = w.on_remove<Crate>([](ecs::world&, ecs::entity, void* user)
                                                  { ++static_cast<hook_counts*>(user)->removed; },
                                                  &hc);
    const ecs::entity h = w.spawn(Crate{1});
    w.remove<Crate>(h);
    CHECK(hc.added == 1 && hc.removed == 1);
    w.unhook(ta);
    w.unhook(tr);

    // Selections intersect the custom pool with built-ins.
    std::size_t both = 0;
    w.select<Crate, Pos>().each([&](Crate&, Pos&) { ++both; });
    CHECK(both == 1);  // only `a` carries Pos too

    // Sorting permutes boxes (pointer-stable) and honors injected algorithms.
    // (a was re-boxed by the remove/re-add above: capture the new address.)
    const Crate* reboxed = w.find<Crate>(a);
    w.sort<Crate>([](const Crate& l, const Crate& r) { return l.v < r.v; }, stable_algo{});
    CHECK(w.find<Crate>(a) == reboxed);
    int prev = -1;
    bool ordered = true;
    w.select<Crate>().each(
        [&](Crate& k)
        {
            ordered = ordered && prev <= k.v;
            prev = k.v;
        });
    CHECK(ordered);

    // Bonds run their mirrored partition through the custom mirror_swap.
    w.bond<Crate, Pos>();
    const auto pair = w.bonded<Crate, Pos>();
    CHECK(pair.count() == 1);
    const ecs::entity lone = w.select<Crate>(ecs::except<Pos>{}).first();
    w.add<Pos>(lone, Pos{7});
    CHECK(pair.count() == 2);
    CHECK_VALID(w);
    w.unbond<Crate, Pos>();

    // duplicate routes through copy_item.
    const auto dup = w.duplicate(a);
    CHECK(w.has<Crate>(dup.clone));
    CHECK(w.get<Crate>(dup.clone).v == w.get<Crate>(a).v);

    // Inspection reports the custom pool with its declared kind; type-erased
    // payload access goes through item_address.
    bool seen = false;
    w.each_pool(
        [&](const ecs::pool_info& info)
        {
            seen = seen ||
                   (info.name_hash == ecs::hash_of<Crate>() && info.kind == ecs::storage::stable);
        });
    CHECK(seen);
    auto pr = w.find_pool<Crate>();
    CHECK(pr.raw(a) == static_cast<void*>(w.find<Crate>(a)));

    // Archives drive the typed surface too.
    {
        byte_writer out;
        ecs::pack<Crate>(w, out);
        ecs::world fresh;
        byte_reader in{&out.data};
        const auto ok = ecs::unpack<Crate>(fresh, in);
        CHECK(ok.has_value());
        CHECK(fresh.select<Crate>().count() == w.select<Crate>().count());
    }

    // Lifetime balance through purge's wipe path.
    {
        const int live_before = Counted::live;
        for (int i = 0; i < 8; ++i)
        {
            w.add<CrateCounted>(w.spawn(), 5);
        }
        CHECK(Counted::live == live_before + 8);
        w.purge<CrateCounted>();
        CHECK(Counted::live == live_before);
    }

    CHECK_VALID(w);
}
