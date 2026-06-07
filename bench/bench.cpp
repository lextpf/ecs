// ============================================================================
// bench.cpp — order-of-magnitude timings for quiver's hot operations.
//
// Methodology: each case runs `repeats` times over `entity_count` entities and
// the BEST run is reported (best-of-N suppresses scheduler noise; it answers
// "how fast can this be", not "how fast is it on average"). A checksum is
// accumulated into a volatile sink so the optimizer cannot delete the work.
//
// These numbers are for spotting regressions and gross mistakes, not for
// marketing. An ECS's real cost depends on your component sizes, access
// patterns, and cache pressure: measure in your game, with your data, before
// believing any table — this one included. Build with optimizations and
// NDEBUG (QUIVER_CHECKS off); a checked build measures the safety rails too.
// ============================================================================

#include <quiver.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ecs = quiver;

namespace
{

struct Transform
{
    float x = 0;
    float y = 0;

    [[nodiscard]] float sum() const { return x + y; }
};

struct Velocity
{
    float dx = 0;
    float dy = 0;
};

struct Sleeping  // tag
{
};

struct Mass
{
    float kg = 0;
};

struct Heat
{
    float degrees = 0;
};

volatile std::uint64_t sink = 0;  // defeats dead-code elimination

constexpr std::size_t entity_count = 100'000;
constexpr int repeats = 12;

using clock_type = std::chrono::steady_clock;

// Times body() only; reset() runs between repetitions off the clock.
template <class F, class R = void (*)()>
double best_ns_per_op(std::size_t ops, F&& body, R&& reset = [] {})
{
    double best = 1e300;
    for (int r = 0; r < repeats; ++r)
    {
        const auto start = clock_type::now();
        body();
        const auto stop = clock_type::now();
        const double ns = std::chrono::duration<double, std::nano>(stop - start).count();
        best = std::min(best, ns / static_cast<double>(ops));
        reset();
    }
    return best;
}

void report(const char* name, double ns_per_op)
{
    std::printf("  %-34s %10.2f ns/op\n", name, ns_per_op);
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) — only bad_alloc can escape; that is fatal anyway
int main()
{
    std::printf("quiver bench — %zu entities, best of %d runs\n", entity_count, repeats);
#if QUIVER_CHECKS
    std::printf("  NOTE: QUIVER_CHECKS is ON; this measures the checked build.\n");
#endif

    // --- entity lifecycle ----------------------------------------------------
    {
        ecs::world w;
        w.reserve_entities(entity_count);
        std::vector<ecs::entity> entities(entity_count);
        report("spawn (bare)",
               best_ns_per_op(
                   entity_count,
                   [&]
                   {
                       for (std::size_t i = 0; i < entity_count; ++i)
                       {
                           entities[i] = w.spawn();
                       }
                   },
                   [&]
                   {
                       for (const ecs::entity e : entities)
                       {
                           w.kill(e);
                       }
                   }));

        for (std::size_t i = 0; i < entity_count; ++i)
        {
            entities[i] = w.spawn();
        }
        report("kill (no components)",
               best_ns_per_op(
                   entity_count,
                   [&]
                   {
                       for (const ecs::entity e : entities)
                       {
                           w.kill(e);
                       }
                   },
                   [&]
                   {
                       for (std::size_t i = 0; i < entity_count; ++i)
                       {
                           entities[i] = w.spawn();
                       }
                   }));
    }

    // --- component add / remove ----------------------------------------------
    {
        ecs::world w;
        w.reserve_entities(entity_count);
        w.reserve<Transform>(entity_count);
        std::vector<ecs::entity> entities(entity_count);
        for (std::size_t i = 0; i < entity_count; ++i)
        {
            entities[i] = w.spawn();
        }
        report("add<Transform>",
               best_ns_per_op(
                   entity_count,
                   [&]
                   {
                       for (std::size_t i = 0; i < entity_count; ++i)
                       {
                           w.add<Transform>(
                               entities[i],
                               Transform{static_cast<float>(i), static_cast<float>(i)});
                       }
                   },
                   [&]
                   {
                       for (const ecs::entity e : entities)
                       {
                           w.remove<Transform>(e);
                       }
                   }));
        for (std::size_t i = 0; i < entity_count; ++i)
        {
            w.add<Transform>(entities[i], Transform{1, 1});
        }
        report("remove<Transform>",
               best_ns_per_op(
                   entity_count,
                   [&]
                   {
                       for (const ecs::entity e : entities)
                       {
                           w.remove<Transform>(e);
                       }
                   },
                   [&]
                   {
                       for (std::size_t i = 0; i < entity_count; ++i)
                       {
                           w.add<Transform>(entities[i], Transform{1, 1});
                       }
                   }));
    }

    // --- membership probes -----------------------------------------------------
    {
        ecs::world w;
        w.reserve_entities(entity_count);
        std::vector<ecs::entity> entities(entity_count);
        for (std::size_t i = 0; i < entity_count; ++i)
        {
            entities[i] = w.spawn(Transform{1, 1});
            if (i % 2 == 0)
            {
                w.add<Velocity>(entities[i], Velocity{1, 1});
            }
        }
        report("has_all<Transform, Velocity>",
               best_ns_per_op(entity_count,
                              [&]
                              {
                                  std::uint64_t hits = 0;
                                  for (const ecs::entity e : entities)
                                  {
                                      hits += w.has_all<Transform, Velocity>(e) ? 1u : 0u;
                                  }
                                  sink += hits;
                              }));
    }

    // --- iteration -------------------------------------------------------------
    {
        ecs::world w;
        w.reserve_entities(entity_count);
        w.reserve<Transform>(entity_count);
        w.reserve<Velocity>(entity_count / 2);
        for (std::size_t i = 0; i < entity_count; ++i)
        {
            const ecs::entity e = w.spawn(Transform{static_cast<float>(i), 0});
            if (i % 2 == 0)
            {
                w.add<Velocity>(e, Velocity{1, 2});
            }
            if (i % 8 == 0)
            {
                w.add<Sleeping>(e);
            }
        }

        report("each<Transform> (100k)",
               best_ns_per_op(entity_count,
                              [&]
                              {
                                  float acc = 0;
                                  w.each<Transform>([&](const Transform& t) { acc += t.x; });
                                  sink += static_cast<std::uint64_t>(acc);
                              }));

        report("each<Transform, Velocity> (50k)",
               best_ns_per_op(entity_count / 2,
                              [&]
                              {
                                  float acc = 0;
                                  w.each<Transform, const Velocity>(
                                      [&](Transform& t, const Velocity& v)
                                      {
                                          t.x += v.dx;
                                          acc += t.x;
                                      });
                                  sink += static_cast<std::uint64_t>(acc);
                              }));

        report("... + except<Sleeping>",
               best_ns_per_op(entity_count / 2,
                              [&]
                              {
                                  float acc = 0;
                                  w.each<Transform, const Velocity>(
                                      ecs::except<Sleeping>{},
                                      [&](Transform& t, const Velocity& v) { acc += t.x + v.dx; });
                                  sink += static_cast<std::uint64_t>(acc);
                              }));

        // Stored selection vs rebuilding through world.each every call: the
        // difference is the per-call setup (pool lookup + registration check),
        // visible only when iterations are tiny and frequent.
        ecs::world small;
        for (int i = 0; i < 64; ++i)
        {
            small.spawn(Transform{}, Velocity{1, 1});
        }
        auto stored = small.select<Transform, const Velocity>();
        constexpr std::size_t frames = 20'000;

        report("64-entity frame, ad-hoc each",
               best_ns_per_op(frames,
                              [&]
                              {
                                  for (std::size_t f = 0; f < frames; ++f)
                                  {
                                      small.each<Transform, const Velocity>(
                                          [](Transform& t, const Velocity& v) { t.x += v.dx; });
                                  }
                              }));

        report("64-entity frame, stored selection",
               best_ns_per_op(frames,
                              [&]
                              {
                                  for (std::size_t f = 0; f < frames; ++f)
                                  {
                                      stored.each([](Transform& t, const Velocity& v)
                                                  { t.x += v.dx; });
                                  }
                              }));
    }

    // --- command buffer ----------------------------------------------------
    {
        ecs::world w;
        w.reserve_entities(entity_count);
        w.reserve<Velocity>(entity_count);
        std::vector<ecs::entity> entities(entity_count);
        for (std::size_t i = 0; i < entity_count; ++i)
        {
            entities[i] = w.spawn();
        }
        ecs::command_buffer cmd;
        constexpr std::size_t batch = 10'000;
        report("record + apply add<Velocity>",
               best_ns_per_op(
                   batch,
                   [&]
                   {
                       for (std::size_t i = 0; i < batch; ++i)
                       {
                           cmd.add<Velocity>(entities[i], Velocity{1, 1});
                       }
                       const ecs::apply_result r = w.apply(cmd);
                       sink += r.applied;
                   },
                   [&]
                   {
                       for (std::size_t i = 0; i < batch; ++i)
                       {
                           w.remove<Velocity>(entities[i]);
                       }
                   }));
    }

    // --- bonded pairs --------------------------------------------------------
    {
        ecs::world w;
        w.reserve_entities(entity_count);
        w.reserve<Transform>(entity_count);
        w.reserve<Velocity>(entity_count);
        std::vector<ecs::entity> entities(entity_count);
        for (std::size_t i = 0; i < entity_count; ++i)
        {
            entities[i] = w.spawn(Transform{static_cast<float>(i), 0});
            if (i % 2 == 0)
            {
                w.add<Velocity>(entities[i], Velocity{1, 1});
            }
        }
        const auto sel = w.select<Transform, const Velocity>();
        report("2-comp iteration, select (probes)",
               best_ns_per_op(entity_count / 2,
                              [&]
                              {
                                  sel.each([](Transform& t, const Velocity& v) { t.x += v.dx; });
                                  sink += 1;
                              }));
        auto view = w.bond<Transform, Velocity>();
        report("2-comp iteration, bonded (linear)",
               best_ns_per_op(entity_count / 2,
                              [&]
                              {
                                  view.each([](Transform& t, const Velocity& v) { t.x += v.dx; });
                                  sink += 1;
                              }));

        // The maintenance price: add/remove churn on the bonded pair.
        constexpr std::size_t churn = 10'000;
        report("bonded add+remove<Velocity> churn",
               best_ns_per_op(churn * 2,
                              [&]
                              {
                                  for (std::size_t i = 1; i < (churn * 2); i += 2)
                                  {
                                      w.add<Velocity>(entities[i], Velocity{1, 1});
                                  }
                                  for (std::size_t i = 1; i < (churn * 2); i += 2)
                                  {
                                      w.remove<Velocity>(entities[i]);
                                  }
                              }));
    }

    // --- any_of combinators ----------------------------------------------------
    {
        ecs::world w;
        w.reserve_entities(entity_count);
        std::vector<ecs::entity> entities(entity_count);
        for (std::size_t i = 0; i < entity_count; ++i)
        {
            entities[i] = w.spawn();
            if (i % 2 == 0)
            {
                w.add<Transform>(entities[i], Transform{1, 1});
            }
            if (i % 3 == 0)
            {
                w.add<Velocity>(entities[i], Velocity{1, 1});
            }
        }
        const std::size_t union_size = 66'666;  // |A ∪ B| for the op count
        const auto either = w.select<ecs::any_of<Transform, Velocity>>();
        report("any_of<2> union drive",
               best_ns_per_op(union_size,
                              [&]
                              {
                                  std::uint64_t n = 0;
                                  either.each([&](ecs::entity, Transform*, Velocity*) { ++n; });
                                  sink += n;
                              }));
        report("two selects back-to-back (the workaround)",
               best_ns_per_op(union_size,
                              [&]
                              {
                                  std::uint64_t n = 0;
                                  w.select<Transform>().each([&](ecs::entity, Transform&) { ++n; });
                                  w.select<Velocity>(ecs::except<Transform>{})
                                      .each([&](ecs::entity, Velocity&) { ++n; });
                                  sink += n;
                              }));
        report("has_any probe loop",
               best_ns_per_op(entity_count,
                              [&]
                              {
                                  std::uint64_t n = 0;
                                  for (const ecs::entity e : entities)
                                  {
                                      n += w.has_any<Transform, Velocity>(e) ? 1 : 0;
                                  }
                                  sink += n;
                              }));
    }

    // --- reflection dispatch -----------------------------------------------------
    {
        ecs::reflect<Transform>().field<&Transform::x>("x").method<&Transform::sum>("sum");
        const ecs::reflection r = ecs::reflection_of<Transform>();
        const ecs::field fx = r.find_field("x");
        const ecs::method sum = r.find_method("sum");
        Transform t{1, 2};
        ecs::any obj = ecs::any::ref(t);
        constexpr std::size_t hits = 100'000;
        report("field get via reflection",
               best_ns_per_op(hits,
                              [&]
                              {
                                  float acc = 0;
                                  for (std::size_t i = 0; i < hits; ++i)
                                  {
                                      acc += fx.get(obj).as<float>();
                                  }
                                  sink += static_cast<std::uint64_t>(acc);
                              }));
        report("method invoke via reflection",
               best_ns_per_op(hits,
                              [&]
                              {
                                  float acc = 0;
                                  for (std::size_t i = 0; i < hits; ++i)
                                  {
                                      acc += sum.invoke(obj, {}).as<float>();
                                  }
                                  sink += static_cast<std::uint64_t>(acc);
                              }));
        report("direct member read (baseline)",
               best_ns_per_op(hits,
                              [&]
                              {
                                  float acc = 0;
                                  for (std::size_t i = 0; i < hits; ++i)
                                  {
                                      acc += t.x;
                                  }
                                  sink += static_cast<std::uint64_t>(acc);
                              }));

        // The structural bridge: add/remove by hash vs the typed verbs.
        ecs::world w;
        constexpr std::size_t churn = 10'000;
        std::vector<ecs::entity> entities(churn);
        for (auto& e : entities)
        {
            e = w.spawn();
        }
        report("add via reflection (add_to)",
               best_ns_per_op(
                   churn,
                   [&]
                   {
                       for (const ecs::entity e : entities)
                       {
                           r.add_to(w, e, ecs::any::make<Transform>(Transform{1, 1}));
                       }
                   },
                   [&]
                   {
                       for (const ecs::entity e : entities)
                       {
                           w.remove<Transform>(e);
                       }
                   }));
        report("add<Transform> (typed baseline)",
               best_ns_per_op(
                   churn,
                   [&]
                   {
                       for (const ecs::entity e : entities)
                       {
                           w.add<Transform>(e, Transform{1, 1});
                       }
                   },
                   [&]
                   {
                       for (const ecs::entity e : entities)
                       {
                           w.remove<Transform>(e);
                       }
                   }));
    }

    // --- watcher overhead --------------------------------------------------------
    {
        ecs::world w;
        constexpr std::size_t churn = 10'000;
        std::vector<ecs::entity> entities(churn);
        for (auto& e : entities)
        {
            e = w.spawn(Transform{1, 1});
        }
        const auto churn_once = [&]
        {
            for (const ecs::entity e : entities)
            {
                w.add<Velocity>(e, Velocity{1, 1});
            }
            for (const ecs::entity e : entities)
            {
                w.remove<Velocity>(e);
            }
        };
        report("add+remove churn, unwatched", best_ns_per_op(churn * 2, churn_once));
        ecs::watcher<ecs::types<Transform, Velocity>> moving(w);
        report("add+remove churn, watched (2 conds)",
               best_ns_per_op(churn * 2,
                              [&]
                              {
                                  churn_once();
                                  moving.clear();
                              }));
    }

    // --- bonded groups (N-ary) -------------------------------------------------
    {
        ecs::world w;
        w.reserve_entities(entity_count);
        std::vector<ecs::entity> entities(entity_count);
        for (std::size_t i = 0; i < entity_count; ++i)
        {
            entities[i] = w.spawn(Transform{static_cast<float>(i), 0});
            if (i % 2 == 0)
            {
                w.add<Velocity>(entities[i], Velocity{1, 1});
            }
            if (i % 4 == 0)
            {
                w.add<Mass>(entities[i], Mass{1});
            }
        }
        const auto sel = w.select<Transform, const Velocity, const Mass>();
        report("3-comp iteration, select (probes)",
               best_ns_per_op(entity_count / 4,
                              [&]
                              {
                                  sel.each([](Transform& t, const Velocity& v, const Mass& m)
                                           { t.x += v.dx * m.kg; });
                                  sink += 1;
                              }));
        auto view = w.bond<Transform, Velocity, Mass>();
        report("3-comp iteration, bonded (linear)",
               best_ns_per_op(entity_count / 4,
                              [&]
                              {
                                  view.each([](Transform& t, const Velocity& v, const Mass& m)
                                            { t.x += v.dx * m.kg; });
                                  sink += 1;
                              }));

        // Observed extras pay one sparse probe per row, on top of the
        // zero-probe owned walk.
        for (std::size_t i = 0; i < entity_count; i += 8)
        {
            w.add<Heat>(entities[i], Heat{300});
        }
        auto observing = w.bonded<Transform, Velocity, Mass, ecs::maybe<Heat>>();
        report("3-owned + observed maybe<Heat>",
               best_ns_per_op(entity_count / 4,
                              [&]
                              {
                                  observing.each(
                                      [](Transform& t, const Velocity& v, const Mass& m, Heat* h)
                                      { t.x += v.dx * m.kg + (h != nullptr ? h->degrees : 0.0f); });
                                  sink += 1;
                              }));

        // Maintenance at three owners: entities enter/leave through Mass.
        constexpr std::size_t churn = 10'000;
        report("3-owner bond add+remove<Mass> churn",
               best_ns_per_op(churn * 2,
                              [&]
                              {
                                  for (std::size_t i = 2; i < (churn * 4); i += 4)
                                  {
                                      w.add<Mass>(entities[i], Mass{1});
                                  }
                                  for (std::size_t i = 2; i < (churn * 4); i += 4)
                                  {
                                      w.remove<Mass>(entities[i]);
                                  }
                              }));

        // Maintenance at four owners: the tag side churns.
        ecs::world w4;
        std::vector<ecs::entity> quad(churn);
        for (std::size_t i = 0; i < churn; ++i)
        {
            quad[i] = w4.spawn(Transform{1, 1});
            w4.add<Velocity>(quad[i], Velocity{1, 1});
            w4.add<Mass>(quad[i], Mass{1});
        }
        w4.bond<Transform, Velocity, Mass, Sleeping>();
        report("4-owner bond add+remove<Sleeping> churn",
               best_ns_per_op(churn * 2,
                              [&]
                              {
                                  for (const ecs::entity e : quad)
                                  {
                                      w4.add<Sleeping>(e);
                                  }
                                  for (const ecs::entity e : quad)
                                  {
                                      w4.remove<Sleeping>(e);
                                  }
                              }));
    }

    std::printf("sink=%llu (ignore; anti-elision)\n", static_cast<unsigned long long>(sink));
    std::printf("Reminder: measure in your game before trusting microbenchmarks.\n");
    std::printf("No fastest-ECS claims here: these numbers exist to catch regressions.\n");
    return 0;
}
