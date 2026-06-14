#include "test_harness.hpp"

namespace
{
struct tick_event
{
    float dt = 0.0F;
};

struct order_system : ecs::system<tick_event>
{
    std::vector<int>* order;
    int id;

    order_system(std::vector<int>* log, int marker)
        : order(log),
          id(marker)
    {
    }

    void process(ecs::registry&, const tick_event&) override { order->push_back(id); }
};

struct core_feature
{
};

struct extra_feature
{
};

struct step_event
{
    int amount = 0;
};

struct render_event
{
};

struct dual_system : ecs::system<step_event, render_event>
{
    int* steps;
    int* renders;

    dual_system(int* s, int* r)
        : steps(s),
          renders(r)
    {
    }

    void process(ecs::registry&, const step_event& ev) override { *steps += ev.amount; }
    void process(ecs::registry&, const render_event&) override { ++*renders; }
};

struct spawn_system : ecs::system<tick_event>
{
    std::vector<int>* order;
    ecs::entity* spawned;

    spawn_system(std::vector<int>* log, ecs::entity* out)
        : order(log),
          spawned(out)
    {
    }

    void process(ecs::registry& w, const tick_event&) override
    {
        *spawned = w.deferred().create(Hp{1});
        order->push_back(1);
    }
};

struct kill_system : ecs::system<tick_event>
{
    std::vector<int>* order;
    ecs::entity* spawned;

    kill_system(std::vector<int>* log, ecs::entity* target)
        : order(log),
          spawned(target)
    {
    }

    void process(ecs::registry& w, const tick_event&) override
    {
        order->push_back(2);
        static_cast<void>(spawned);
        w.deferred().destroy(w.view<Hp>().first());
    }
};

struct audit_system : ecs::system<tick_event>
{
    std::vector<int>* order;

    explicit audit_system(std::vector<int>* log)
        : order(log)
    {
    }

    void process(ecs::registry& w, const tick_event&) override
    {
        order->push_back(3);
        EXPECT_EQ(w.view<Hp>().count(), 0);
    }
};
}

TEST(Features, SingleEventOrder)
{
    ecs::registry w;
    std::vector<int> order;

    w.feature<core_feature>().add_system<order_system>(&order, 1).add_system<order_system>(&order,
                                                                                           2);
    w.feature<extra_feature>().add_system<order_system>(&order, 3);

    w.dispatch(tick_event{1.0F});
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));

    w.dispatch(tick_event{1.0F});
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 1, 2, 3}));

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Features, MultiEventSystem)
{
    ecs::registry w;
    int steps = 0;
    int renders = 0;

    w.feature<core_feature>().add_system<dual_system>(&steps, &renders);

    w.dispatch(step_event{5});
    EXPECT_EQ(steps, 5);
    EXPECT_EQ(renders, 0);

    w.dispatch(render_event{});
    EXPECT_EQ(steps, 5);
    EXPECT_EQ(renders, 1);

    w.dispatch(step_event{3});
    EXPECT_EQ(steps, 8);
    EXPECT_EQ(renders, 1);

    w.dispatch(tick_event{0.16F});
    EXPECT_EQ(steps, 8);
    EXPECT_EQ(renders, 1);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Features, OrderAndDeferred)
{
    ecs::registry w;
    std::vector<int> order;
    ecs::entity spawned;

    w.feature<core_feature>()
        .add_system<spawn_system>(&order, &spawned)
        .add_system<kill_system>(&order, &spawned)
        .add_system<audit_system>(&order);

    w.dispatch(tick_event{1.0F});
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(w.live_count(), 0);
    EXPECT_TRUE(RegistryValid(w));
}

TEST(Features, ToggleFeature)
{
    ecs::registry w;
    std::vector<int> order;

    w.feature<core_feature>().add_system<order_system>(&order, 1);
    w.feature<extra_feature>().add_system<order_system>(&order, 2);

    w.dispatch(tick_event{1.0F});
    EXPECT_EQ(order, (std::vector<int>{1, 2}));

    order.clear();
    w.disable_feature<extra_feature>();
    w.dispatch(tick_event{1.0F});
    EXPECT_EQ(order, (std::vector<int>{1}));

    order.clear();
    w.enable_feature<extra_feature>();
    w.dispatch(tick_event{1.0F});
    EXPECT_EQ(order, (std::vector<int>{1, 2}));

    EXPECT_TRUE(RegistryValid(w));
}

#if ECS_CHECKS
namespace
{
struct reentrant_system : ecs::system<tick_event>
{
    ecs::registry* self;

    explicit reentrant_system(ecs::registry* r)
        : self(r)
    {
    }

    void process(ecs::registry&, const tick_event&) override
    {
        self->feature<extra_feature>().add_system<order_system>(nullptr, 99);
    }
};
}

TEST(Features, AddSystemDuringDispatchViolation)
{
    violation_scope guard;
    ecs::registry w;

    w.feature<core_feature>().add_system<reentrant_system>(&w);
    w.dispatch(tick_event{1.0F});

    EXPECT_EQ(violations_seen, 1);
    EXPECT_TRUE(RegistryValid(w));
}
#endif

namespace
{
struct ordering_feature
{
};
struct sys_a : ecs::system<tick_event>
{
    std::vector<int>* order;
    explicit sys_a(std::vector<int>* log)
        : order(log)
    {
    }
    void process(ecs::registry&, const tick_event&) override { order->push_back(1); }
};
struct sys_b : ecs::system<tick_event>
{
    std::vector<int>* order;
    explicit sys_b(std::vector<int>* log)
        : order(log)
    {
    }
    void process(ecs::registry&, const tick_event&) override { order->push_back(2); }
};
struct sys_c : ecs::system<tick_event>
{
    std::vector<int>* order;
    explicit sys_c(std::vector<int>* log)
        : order(log)
    {
    }
    void process(ecs::registry&, const tick_event&) override { order->push_back(3); }
};
}

TEST(Features, SystemOrdering)
{
    ecs::registry w;
    std::vector<int> order;
    w.feature<ordering_feature>()
        .add_system<sys_a>(&order)
        .after<sys_b>()
        .add_system<sys_b>(&order)
        .after<sys_c>()
        .add_system<sys_c>(&order);
    w.dispatch(tick_event{1.0F});
    EXPECT_EQ(order, (std::vector<int>{3, 2, 1}));

    ecs::registry w2;
    std::vector<int> order2;
    w2.feature<ordering_feature>()
        .add_system<sys_a>(&order2)
        .after<order_system>()
        .add_system<sys_b>(&order2);
    w2.dispatch(tick_event{1.0F});
    EXPECT_EQ(order2, (std::vector<int>{1, 2}));
}

#if ECS_CHECKS
TEST(Features, OrderingCycleViolation)
{
    ecs::registry w;
    std::vector<int> order;
    const violation_scope guard;
    w.feature<ordering_feature>()
        .add_system<sys_a>(&order)
        .after<sys_b>()
        .add_system<sys_b>(&order)
        .after<sys_a>();
    w.dispatch(tick_event{1.0F});
    EXPECT_GE(violations_seen, 1);
    EXPECT_EQ(order.size(), 2U);
}
#endif

namespace
{
struct nest_outer_event
{
};
struct nest_inner_event
{
};
struct NestMarker
{
};

struct emit_inner_system : ecs::system<nest_outer_event>
{
    ecs::registry* self;
    ecs::entity target;

    emit_inner_system(ecs::registry* r, ecs::entity t)
        : self(r),
          target(t)
    {
    }

    void process(ecs::registry& w, const nest_outer_event&) override
    {
        w.deferred().add<NestMarker>(target);
        self->dispatch(nest_inner_event{});
    }
};

struct inner_noop_system : ecs::system<nest_inner_event>
{
    void process(ecs::registry&, const nest_inner_event&) override {}
};

struct inner_probe_system : ecs::system<nest_inner_event>
{
    ecs::entity target;
    bool* saw;

    inner_probe_system(ecs::entity t, bool* s)
        : target(t),
          saw(s)
    {
    }

    void process(ecs::registry& w, const nest_inner_event&) override
    {
        *saw = w.has<NestMarker>(target);
    }
};
}

TEST(Features, NestedDispatchBufferIsolation)
{
    ecs::registry w;
    const ecs::entity target = w.create();
    bool inner_saw_marker = true;

    w.feature<core_feature>()
        .add_system<emit_inner_system>(&w, target)
        .add_system<inner_noop_system>()
        .add_system<inner_probe_system>(target, &inner_saw_marker);

    w.dispatch(nest_outer_event{});

    EXPECT_FALSE(inner_saw_marker);
    EXPECT_TRUE(w.has<NestMarker>(target));
    EXPECT_TRUE(RegistryValid(w));
}

#if ECS_CHECKS
namespace
{
struct guard_emit_system : ecs::system<nest_outer_event>
{
    ecs::registry* self;

    explicit guard_emit_system(ecs::registry* r)
        : self(r)
    {
    }

    void process(ecs::registry&, const nest_outer_event&) override
    {
        self->dispatch(nest_inner_event{});
    }
};

struct guard_add_system : ecs::system<nest_outer_event>
{
    ecs::registry* self;

    explicit guard_add_system(ecs::registry* r)
        : self(r)
    {
    }

    void process(ecs::registry&, const nest_outer_event&) override
    {
        self->feature<extra_feature>().add_system<inner_noop_system>();
    }
};
}

TEST(Features, NestedDispatchKeepsAddGuard)
{
    violation_scope guard;
    ecs::registry w;

    w.feature<core_feature>().add_system<inner_noop_system>();
    w.feature<core_feature>().add_system<guard_emit_system>(&w).add_system<guard_add_system>(&w);

    w.dispatch(nest_outer_event{});

    EXPECT_EQ(violations_seen, 1);
    EXPECT_TRUE(RegistryValid(w));
}

namespace
{
struct loop_event
{
};

struct recursive_loop_system : ecs::system<loop_event>
{
    ecs::registry* self;

    explicit recursive_loop_system(ecs::registry* r)
        : self(r)
    {
    }

    void process(ecs::registry&, const loop_event&) override { self->dispatch(loop_event{}); }
};
}

TEST(Features, NestedDispatchDepthTripwire)
{
    violation_scope guard;
    ecs::registry w;

    w.feature<core_feature>().add_system<recursive_loop_system>(&w);
    w.dispatch(loop_event{});

    EXPECT_EQ(violations_seen, 1);
    EXPECT_TRUE(RegistryValid(w));
}
#endif

TEST(Features, QueuedEvents)
{
    ecs::registry w;
    std::vector<int> log;
    w.feature<core_feature>().add_system<order_system>(&log, 1);

    w.enqueue(tick_event{1.0F});
    w.enqueue(tick_event{1.0F});
    EXPECT_EQ(w.queued<tick_event>(), 2U);
    EXPECT_TRUE(log.empty());

    w.flush<tick_event>();
    EXPECT_EQ(log, (std::vector<int>{1, 1}));
    EXPECT_EQ(w.queued<tick_event>(), 0U);

    w.flush();
    EXPECT_EQ(log.size(), 2U);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Features, QueuedFlushWaitsForNext)
{
    ecs::registry w;
    int runs = 0;
    bool once = false;
    w.connect<tick_event>(
        [&](ecs::registry& reg, const tick_event& ev)
        {
            ++runs;
            if (!once)
            {
                once = true;
                reg.enqueue(ev);
            }
        });

    w.enqueue(tick_event{1.0F});
    w.flush();
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(w.queued<tick_event>(), 1U);

    w.flush();
    EXPECT_EQ(runs, 2);
    EXPECT_EQ(w.queued<tick_event>(), 0U);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Features, CallableListeners)
{
    ecs::registry w;
    std::vector<int> log;

    const ecs::listener_token a =
        w.connect<tick_event>([&](ecs::registry&, const tick_event&) { log.push_back(1); });
    const ecs::listener_token b =
        w.connect<tick_event>([&](const tick_event&) { log.push_back(2); });

    w.dispatch(tick_event{1.0F});
    EXPECT_EQ(log, (std::vector<int>{1, 2}));

    w.disconnect(a);
    w.dispatch(tick_event{1.0F});
    EXPECT_EQ(log, (std::vector<int>{1, 2, 2}));

    w.feature<core_feature>().add_system<order_system>(&log, 9);
    log.clear();
    w.dispatch(tick_event{1.0F});
    EXPECT_EQ(log, (std::vector<int>{2, 9}));

    EXPECT_TRUE(RegistryValid(w));
}
