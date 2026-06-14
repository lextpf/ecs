#include "test_harness.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <vector>

struct SortKey
{
    int group = 0;
    int seq = 0;
};

struct SortKeyStable
{
    int group = 0;
    int seq = 0;
    static constexpr auto ecs_storage = ecs::storage::stable;
};

template <ecs::component T>
class boxed_pool : public ecs::basic_pool
{
public:
    explicit boxed_pool(std::pmr::memory_resource* memory) noexcept
        : ecs::basic_pool(
              memory, ecs::name_of<T>(), ecs::hash_of<T>(), ecs::storage::stable, sizeof(T)),
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
        return pos == ecs::detail::npos32 ? nullptr : boxes_[pos];
    }

    [[nodiscard]] const T* at(std::uint32_t index) const noexcept
    {
        const std::uint32_t pos = sparse_.get(index);
        return pos == ecs::detail::npos32 ? nullptr : boxes_[pos];
    }

    [[nodiscard]] T& at_pos(std::uint32_t pos) noexcept { return *boxes_[pos]; }
    [[nodiscard]] const T& at_pos(std::uint32_t pos) const noexcept { return *boxes_[pos]; }

    [[nodiscard]] void* item_address(std::uint32_t pos) noexcept override { return boxes_[pos]; }

    template <class PosCompare, class Algo = ecs::detail::std_sort>
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

    [[nodiscard]] std::expected<void, ecs::fault> check(
        const ecs::detail::entity_table& table) const override
    {
        if (boxes_.size() != dense_.size())
        {
            return std::unexpected(
                ecs::fault{ecs::fault_code::sparse_dense_desync, name(), "box count"});
        }
        return check_membership(table);
    }

private:
    std::pmr::vector<T*> boxes_;
};

struct Crate
{
    int v = 0;
};

struct CrateCounted : Counted
{
    using Counted::Counted;
};

template <>
struct ecs::pool_of<Crate>
{
    using type = boxed_pool<Crate>;
};

template <>
struct ecs::pool_of<CrateCounted>
{
    using type = boxed_pool<CrateCounted>;
};

TEST(World, HasAllAny)
{
    ecs::registry w;

    const ecs::entity e = w.create(Pos{1}, Vel{2});
    const ecs::entity lone = w.create(Pos{3});

    EXPECT_TRUE((w.has_all<Pos, Vel>(e)));
    EXPECT_FALSE((w.has_all<Pos, Vel>(lone)));
    EXPECT_FALSE((w.has_all<Pos, Vel, Hp>(e)));

    EXPECT_TRUE((w.has_any<Pos, Hp>(lone)));
    EXPECT_FALSE((w.has_any<Vel, Hp>(lone)));

    w.add<TagA>(e);
    EXPECT_TRUE((w.has_all<Pos, TagA>(e)));
    EXPECT_TRUE((w.has_any<TagB, TagA>(e)));
    EXPECT_FALSE((w.has_any<TagB, Hp>(e)));

    const ecs::entity dead = w.create(Pos{9});
    w.destroy(dead);
    EXPECT_FALSE((w.has_all<Pos, Vel>(dead)));
    EXPECT_FALSE((w.has_any<Pos, Vel>(dead)));
    EXPECT_FALSE((w.has_all<Pos, Vel>(ecs::no_entity)));
    EXPECT_FALSE((w.has_any<Pos, Vel>(ecs::no_entity)));

    const ecs::registry& cw = w;
    EXPECT_TRUE((cw.has_all<Pos, Vel>(e)));
    EXPECT_TRUE((cw.has_any<Hp, Vel>(e)));

    EXPECT_TRUE(RegistryValid(w));
}

TEST(World, Amend)
{
    ecs::registry w;

    const ecs::entity e = w.create(Pos{1});

    Pos& p = w.amend<Pos>(e, [](Pos& pos) { pos.x += 41; });
    EXPECT_EQ(p.x, 42);
    EXPECT_EQ(w.find<Pos>(e), &p);

    struct seen
    {
        int fired = 0;
        int value_at_fire = -1;
    } s;
    const ecs::hook_token t = w.on_replace<Pos>(
        [](ecs::registry& reg, ecs::entity who, void* user)
        {
            auto* k = static_cast<seen*>(user);
            ++k->fired;
            k->value_at_fire = reg.get<Pos>(who).x;
        },
        &s);
    w.amend<Pos>(e, [](Pos& pos) { pos.x = 7; });
    EXPECT_EQ(s.fired, 1);
    EXPECT_EQ(s.value_at_fire, 7);
    w.unhook(t);

    const ecs::entity f = w.create(Pos{100});
    int visited = 0;
    w.view<Pos>().each(
        [&](ecs::entity who, Pos&)
        {
            ++visited;
            w.amend<Pos>(who, [](Pos& pos) { pos.x += 1; });
        });
    EXPECT_EQ(visited, 2);
    EXPECT_EQ(w.get<Pos>(e).x, 8);
    EXPECT_EQ(w.get<Pos>(f).x, 101);
    w.sort<Pos>([](const Pos& a, const Pos& b) { return a.x < b.x; });

    {
        const ecs::entity c = w.create(Counted{5});
        const int after_spawn = Counted::total_ctors;
        w.amend<Counted>(c, [](Counted& k) { k.value = 6; });
        EXPECT_EQ(Counted::total_ctors, after_spawn);
        EXPECT_EQ(w.get<Counted>(c).value, 6);
    }

    EXPECT_TRUE(RegistryValid(w));
}

TEST(World, DrivenBy)
{
    ecs::registry w;

    const ecs::entity a = w.create(Pos{3}, Vel{30});
    const ecs::entity b = w.create(Pos{1}, Vel{10});
    const ecs::entity c = w.create(Pos{2}, Vel{20});
    w.create(Pos{99});

    w.sort<Pos>([](const Pos& l, const Pos& r) { return l.x < r.x; });

    const auto sel = w.view<Pos, Vel>();
    const auto by_pos = sel.driven_by<Pos>();

    std::vector<int> xs;
    by_pos.each([&](Pos& p, Vel&) { xs.push_back(p.x); });
    EXPECT_EQ(xs.size(), 3);
    EXPECT_TRUE(xs[0] == 1 && xs[1] == 2 && xs[2] == 3);

    std::size_t matched = 0;
    sel.each([&](Pos&, Vel&) { ++matched; });
    EXPECT_EQ(matched, 3);
    EXPECT_EQ(by_pos.count(), sel.count());
    EXPECT_TRUE(by_pos.contains(a) && by_pos.contains(b) && by_pos.contains(c));

    {
        std::vector<int> from_parts;
        const auto work = by_pos.split(2);
        for (std::size_t i = 0; i < work.parts(); ++i)
        {
            work.part(i).each([&](Pos& p, Vel&) { from_parts.push_back(p.x); });
        }
        EXPECT_EQ(from_parts, xs);
    }

    w.add<TagA>(b);
    std::vector<int> filtered;
    w.view<Pos, ecs::maybe<Vel>>(!ecs::exists<TagA>{})
        .driven_by<Pos>()
        .each([&](Pos& p, Vel*) { filtered.push_back(p.x); });
    EXPECT_EQ(filtered.size(), 3);
    EXPECT_TRUE(filtered[0] == 2 && filtered[1] == 3 && filtered[2] == 99);

    const ecs::registry cold;
    std::size_t cold_visits = 0;
    cold.view<Pos, Vel>().driven_by<Pos>().each([&](const Pos&, const Vel&) { ++cold_visits; });
    EXPECT_EQ(cold_visits, 0);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(World, SortAlgorithm)
{
    ecs::registry w;

    for (int i = 0; i < 6; ++i)
    {
        w.create(SortKey{i % 2, i}, SortKeyStable{i % 2, i});
    }

    w.sort<SortKey>([](const SortKey& l, const SortKey& r) { return l.group < r.group; },
                    stable_algo{});

    std::vector<std::pair<int, int>> order;
    w.view<SortKey>().each([&](SortKey& k) { order.emplace_back(k.group, k.seq); });
    const std::vector<std::pair<int, int>> expected{{0, 0}, {0, 2}, {0, 4}, {1, 1}, {1, 3}, {1, 5}};
    EXPECT_EQ(order, expected);

    const ecs::entity probe = w.view<SortKeyStable>().first();
    const SortKeyStable* before = w.find<SortKeyStable>(probe);
    w.sort<SortKeyStable>([](const SortKeyStable& l, const SortKeyStable& r)
                          { return l.group < r.group; },
                          stable_algo{});
    EXPECT_EQ(w.find<SortKeyStable>(probe), before);

    std::vector<std::pair<int, int>> stable_order;
    w.view<SortKeyStable>().each([&](SortKeyStable& k)
                                 { stable_order.emplace_back(k.group, k.seq); });
    EXPECT_EQ(stable_order, expected);

    w.sort<SortKey>([](ecs::entity l, ecs::entity r) { return l.index() < r.index(); },
                    stable_algo{});
    std::vector<int> seqs;
    w.view<SortKey>().each([&](SortKey& k) { seqs.push_back(k.seq); });
    const std::vector<int> spawn_order{0, 1, 2, 3, 4, 5};
    EXPECT_EQ(seqs, spawn_order);

#if ECS_CHECKS
    {
        violation_scope guard;
        w.view<SortKey>().each(
            [&](ecs::entity, SortKey&)
            {
                w.sort<SortKey>([](const SortKey& l, const SortKey& r) { return l.seq < r.seq; },
                                stable_algo{});
                return false;
            });
        EXPECT_EQ(violations_seen, 1);
        std::vector<int> after;
        w.view<SortKey>().each([&](SortKey& k) { after.push_back(k.seq); });
        EXPECT_EQ(after, spawn_order);
    }
#endif

    EXPECT_TRUE(RegistryValid(w));
}

TEST(World, CustomPoolFromScratch)
{
    ecs::registry w;

    const ecs::entity a = w.create(Pos{1});
    Crate& c = w.add<Crate>(a, Crate{10});
    EXPECT_EQ(c.v, 10);
    EXPECT_TRUE(w.has<Crate>(a));
    EXPECT_EQ(w.get<Crate>(a).v, 10);

    const Crate* pinned = w.find<Crate>(a);
    for (int i = 0; i < 64; ++i)
    {
        w.add<Crate>(w.create(), Crate{100 + i});
    }
    EXPECT_EQ(w.find<Crate>(a), pinned);

    w.replace<Crate>(a, Crate{11});
    w.amend<Crate>(a, [](Crate& k) { k.v += 1; });
    EXPECT_EQ(w.get<Crate>(a).v, 12);
    w.put<Crate>(a, Crate{13});
    EXPECT_EQ(w.obtain<Crate>(a).v, 13);
    EXPECT_TRUE(w.remove<Crate>(a));
    EXPECT_FALSE(w.has<Crate>(a));
    w.add<Crate>(a, Crate{20});

    struct hook_counts
    {
        int added = 0;
        int removed = 0;
    } hc;
    const ecs::hook_token ta = w.on_add<Crate>([](ecs::registry&, ecs::entity, void* user)
                                               { ++static_cast<hook_counts*>(user)->added; },
                                               &hc);
    const ecs::hook_token tr = w.on_remove<Crate>([](ecs::registry&, ecs::entity, void* user)
                                                  { ++static_cast<hook_counts*>(user)->removed; },
                                                  &hc);
    const ecs::entity h = w.create(Crate{1});
    w.remove<Crate>(h);
    EXPECT_TRUE(hc.added == 1 && hc.removed == 1);
    w.unhook(ta);
    w.unhook(tr);

    std::size_t both = 0;
    w.view<Crate, Pos>().each([&](Crate&, Pos&) { ++both; });
    EXPECT_EQ(both, 1);

    const Crate* reboxed = w.find<Crate>(a);
    w.sort<Crate>([](const Crate& l, const Crate& r) { return l.v < r.v; }, stable_algo{});
    EXPECT_EQ(w.find<Crate>(a), reboxed);
    int prev = -1;
    bool ordered = true;
    w.view<Crate>().each(
        [&](Crate& k)
        {
            ordered = ordered && prev <= k.v;
            prev = k.v;
        });
    EXPECT_TRUE(ordered);

    const auto dup = w.duplicate(a);
    EXPECT_TRUE(w.has<Crate>(dup.clone));
    EXPECT_EQ(w.get<Crate>(dup.clone).v, w.get<Crate>(a).v);

    bool seen = false;
    w.each_pool(
        [&](const ecs::pool_info& info)
        {
            seen = seen ||
                   (info.name_hash == ecs::hash_of<Crate>() && info.kind == ecs::storage::stable);
        });
    EXPECT_TRUE(seen);
    auto pr = w.find_pool<Crate>();
    EXPECT_EQ(pr.raw(a), static_cast<void*>(w.find<Crate>(a)));

    {
        byte_writer out;
        ecs::pack<Crate>(w, out);
        ecs::registry fresh;
        byte_reader in{&out.data};
        const auto ok = ecs::unpack<Crate>(fresh, in);
        EXPECT_TRUE(ok.has_value());
        EXPECT_EQ(fresh.view<Crate>().count(), w.view<Crate>().count());
    }

    {
        const int live_before = Counted::live;
        for (int i = 0; i < 8; ++i)
        {
            w.add<CrateCounted>(w.create(), 5);
        }
        EXPECT_EQ(Counted::live, live_before + 8);
        w.purge<CrateCounted>();
        EXPECT_EQ(Counted::live, live_before);
    }

    EXPECT_TRUE(RegistryValid(w));
}

TEST(World, BulkCreateEntities)
{
    ecs::registry w;

    std::vector<ecs::entity> made;
    w.create_n(5, std::back_inserter(made));
    EXPECT_EQ(made.size(), 5u);
    for (const ecs::entity e : made)
    {
        EXPECT_TRUE(w.alive(e));
    }
    EXPECT_EQ(w.live_count(), 5u);

    int seen = 0;
    w.create_n(3, [&](ecs::entity e) { w.add<Pos>(e, Pos{++seen}); });
    EXPECT_EQ(seen, 3);
    EXPECT_EQ(w.view<Pos>().count(), 3u);

    w.destroy(made.begin(), made.end());
    for (const ecs::entity e : made)
    {
        EXPECT_FALSE(w.alive(e));
    }
    EXPECT_EQ(w.live_count(), 3u);

    w.create_n(0, std::back_inserter(made));
    EXPECT_EQ(w.live_count(), 3u);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(World, InsertRange)
{
    ecs::registry w;
    std::vector<ecs::entity> es;
    w.create_n(4, std::back_inserter(es));

    int adds = 0;
    const ecs::hook_token t =
        w.on_add<Vel>([](ecs::registry&, ecs::entity, void* u) { ++*static_cast<int*>(u); }, &adds);
    w.insert<Vel>(es.begin(), es.end(), Vel{7});
    EXPECT_EQ(adds, 4);
    for (const ecs::entity e : es)
    {
        EXPECT_EQ(w.get<Vel>(e).v, 7);
    }
    w.unhook(t);

    const std::array<Pos, 4> seed{Pos{1}, Pos{2}, Pos{3}, Pos{4}};
    w.insert<Pos>(es.begin(), es.end(), seed.begin());
    int sum = 0;
    w.view<Pos>().each([&](Pos& p) { sum += p.x; });
    EXPECT_EQ(sum, 10);

    EXPECT_TRUE(RegistryValid(w));
}
