// ============================================================================
// gameplay.cpp -- a miniature skirmish that tours the whole library.
//
// Acts:
//   1. setup: blueprints, globals, bonds, hooks, trackers
//   2. frame pipeline: bonded iteration, event entities, deferred sync point
//   3. parallel integration via selection::split
//   4. save / load: pack to an in-memory archive, unpack, prove equivalence
//   5. reinforcements: graft an archive into the live world, relink handles
//   6. inspector: runtime queries, footprint, validate
//   7. property grid: reflected field reads and structural verbs by hash
//
// Output is deterministic; run after changes and diff.
// ============================================================================

#include <ecs.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// ----------------------------------------------------------------- components

struct Vec2
{
    float x = 0;
    float y = 0;
};

static Vec2 operator+(Vec2 a, Vec2 b)
{
    return {a.x + b.x, a.y + b.y};
}
static Vec2 operator*(Vec2 a, float s)
{
    return {a.x * s, a.y * s};
}

struct Transform
{
    Vec2 position;
};

struct Velocity
{
    Vec2 value;
};

struct SpriteRef
{
    std::uint32_t atlas_index = 0;
};

struct Health
{
    int value = 0;
};

struct Burning  // tag: bonded to Health for filtered iteration
{
};

struct PlayerTag
{
};

struct Squad  // stores entity handles; relinks them across a graft
{
    ecs::entity leader;

    void ecs_relink(const ecs::graft_map& map) { leader = map.resolve(leader); }
};

struct MatchClock  // lives on world.globals()
{
    float elapsed = 0;
    int frame = 0;
};

struct Explosion  // an EVENT, modeled as a short-lived entity
{
    Vec2 center;
    int damage = 0;
};

// ------------------------------------------------- an in-memory archive shape
// pack/unpack/graft call your archive for every value; this one is raw bytes.

struct byte_writer
{
    std::vector<std::byte> data;

    template <class T>
        requires std::is_trivially_copyable_v<T>
    void operator()(const T& value)
    {
        const auto* raw = reinterpret_cast<const std::byte*>(&value);
        data.insert(data.end(), raw, raw + sizeof(T));
    }
};

struct byte_reader
{
    const std::vector<std::byte>* data = nullptr;
    std::size_t pos = 0;

    template <class T>
        requires std::is_trivially_copyable_v<T>
    void operator()(T& value)
    {
        std::memcpy(&value, data->data() + pos, sizeof(T));
        pos += sizeof(T);
    }
};

// ----------------------------------------------- a system object with hooks
// Member-function hooks via scoped_hook: connections die with the system.

struct GpuSprites
{
    int live = 0;

    void upload(ecs::world&, ecs::entity) { ++live; }
    void release(ecs::entity) { --live; }

    ecs::scoped_hook on_upload;
    ecs::scoped_hook on_release;

    explicit GpuSprites(ecs::world& w)
        : on_upload(w, w.on_add<SpriteRef, &GpuSprites::upload>(this)),
          on_release(w, w.on_remove<SpriteRef, &GpuSprites::release>(this))
    {
    }
};

// NOLINTNEXTLINE(bugprone-exception-escape) -- only bad_alloc can escape
int main()
{
    std::puts("=== act 1: setup ===");
    ecs::world world;

    // Bonds: Transform & Velocity iterate as parallel arrays (zero probes);
    // Health & Burning gives damage-over-time a dense filtered walk.
    auto movers = world.bond<Transform, Velocity>();
    auto burning = world.bond<Health, Burning>();

    // GPU bookkeeping reacts to SpriteRef add/remove.
    GpuSprites gpu(world);

    // The UI drains health changes once per frame.
    ecs::tracker<Health> health_events(world, ecs::track::replaced | ecs::track::removed);

    // World state without a singleton framework: components on globals().
    world.globals().obtain<MatchClock>();

    // A reusable spawn recipe; the batch spawn hands each goblin back for tweaks.
    ecs::blueprint goblin{Transform{Vec2{0, 0}}, Velocity{Vec2{1, 0}}, SpriteRef{7U}, Health{20}};

    int column = 0;
    world.spawn(goblin,
                4,
                [&](ecs::entity g)
                {
                    world.get<Transform>(g).position = {static_cast<float>(column) * 10.0F, 0};
                    if (column % 2 == 0)
                    {
                        world.add<Burning>(g);  // enters the Health & Burning bond
                    }
                    ++column;
                });
    const ecs::entity player = world.spawn(Transform{{50, 0}}, Velocity{{0, 0}});
    world.add<SpriteRef>(player, SpriteRef{1U});
    world.add<Health>(player, Health{100});
    world.add<PlayerTag>(player);

    // duplicate(): stamp an elite from an existing goblin, then buff it.
    const ecs::entity elite = world.duplicate(world.select<Health>().first()).clone;
    world.get<Health>(elite).value = 40;

    std::printf("movers=%zu burning=%zu sprites=%d live=%zu\n",
                movers.count(),
                burning.count(),
                gpu.live,
                world.live_count());

    std::puts("=== act 2: three pipeline frames ===");
    ecs::pipeline frame;
    frame
        .stage("clock",
               [](ecs::world& w, float dt)
               {
                   MatchClock& clock = w.globals().get<MatchClock>();
                   clock.elapsed += dt;
                   ++clock.frame;
               })
        .stage("movement",
               [&movers](ecs::world&, float dt)
               {
                   // Bonded iteration: parallel arrays, no per-entity probes.
                   movers.each([dt](Transform& t, const Velocity& v)
                               { t.position = t.position + (v.value * dt); });
               })
        .stage("burning",
               [&burning](ecs::world&, float)
               { burning.each([](ecs::entity, Health& h) { h.value -= 3; }); })
        .stage("explosions",
               [&frame](ecs::world& w, float)
               {
                   // Events as entities: drain each Explosion, kill it via the deferred buffer.
                   w.each<const Explosion>(
                       [&](ecs::entity ev, const Explosion& boom)
                       {
                           w.each<Transform, Health>(
                               [&](Transform& t, Health& h)
                               {
                                   const float dx = t.position.x - boom.center.x;
                                   if (std::fabs(dx) < 15.0F)
                                   {
                                       h.value -= boom.damage;
                                   }
                               });
                           frame.deferred().kill(ev);
                       });
               })
        .stage("reaper",
               [&frame](ecs::world& w, float)
               {
                   w.each<const Health>(
                       [&](ecs::entity e, const Health& h)
                       {
                           if (h.value <= 0)
                           {
                               frame.deferred().kill(e);
                           }
                       });
               })
        .stage("ui",
               [&health_events](ecs::world& w, float)
               {
                   for (const ecs::entity e : health_events.removed())
                   {
                       std::printf("  [ui] entity %u fell%s\n",
                                   e.index(),
                                   w.alive(e) ? " (component only)" : "");
                   }
                   health_events.clear();
               });

    for (int i = 0; i < 3; ++i)
    {
        if (i == 1)
        {
            world.spawn(Explosion{{10, 0}, 25});  // an event entity
        }
        frame.run(world, 1.0F);
        const MatchClock& clock = world.globals().get<MatchClock>();
        std::printf("frame %d: movers=%zu burning=%zu sprites=%d t=%.0f\n",
                    clock.frame,
                    movers.count(),
                    burning.count(),
                    gpu.live,
                    static_cast<double>(clock.elapsed));
    }

    std::puts("=== act 3: parallel halves ===");
    {
        const auto sel = world.select<Transform, const Velocity>();
        const auto work = sel.split(2);  // umbrella holds the locks
        std::thread left([part = work.part(0)]
                         { part.each([](Transform& t, const Velocity&) { t.position.y += 1; }); });
        std::thread right([part = work.part(1)]
                          { part.each([](Transform& t, const Velocity&) { t.position.y += 1; }); });
        left.join();
        right.join();
        float sum = 0;
        sel.each([&](const Transform& t, const Velocity&) { sum += t.position.y; });
        std::printf(
            "every mover nudged once: sum=%.0f over %zu\n", static_cast<double>(sum), sel.count());
    }

    std::puts("=== act 4: save / load ===");
    {
        byte_writer save;
        ecs::pack<Transform,
                  Velocity,
                  SpriteRef,
                  Health,
                  Burning,
                  PlayerTag,
                  ecs::globals_mark,
                  MatchClock>(world, save);
        std::printf("packed %zu bytes\n", save.data.size());

        ecs::world loaded;
        byte_reader in{&save.data};
        const auto ok = ecs::unpack<Transform,
                                    Velocity,
                                    SpriteRef,
                                    Health,
                                    Burning,
                                    PlayerTag,
                                    ecs::globals_mark,
                                    MatchClock>(loaded, in);
        std::printf("unpack: %s; live %zu == %zu; clock frame %d; player hp %d\n",
                    ok ? "ok" : "FAULT",
                    loaded.live_count(),
                    world.live_count(),
                    loaded.globals().get<MatchClock>().frame,
                    loaded.get<Health>(loaded.select<const PlayerTag>().first()).value);
    }

    std::puts("=== act 5: reinforcements via graft ===");
    {
        // Authored elsewhere: a squad whose members point at their leader.
        ecs::world barracks;
        const ecs::entity sarge = barracks.spawn(Transform{{0, 100}}, Health{30});
        barracks.add<Squad>(sarge, Squad{sarge});
        for (int i = 0; i < 2; ++i)
        {
            const ecs::entity grunt = barracks.spawn(Transform{{5.0F * (i + 1), 100}}, Health{15});
            barracks.add<Squad>(grunt, Squad{sarge});
        }
        byte_writer save;
        ecs::pack<Transform, Health, Squad>(barracks, save);

        byte_reader in{&save.data};
        const auto grafted = ecs::graft<Transform, Health, Squad>(world, in);
        const ecs::entity new_sarge = grafted->resolve(sarge);
        std::size_t loyal = 0;
        world.each<const Squad>(
            [&](const Squad& s)
            {
                if (s.leader == new_sarge)
                {
                    ++loyal;
                }
            });
        std::printf("grafted %zu entities; %zu members follow the relinked leader\n",
                    grafted->size(),
                    loyal);
    }

    std::puts("=== act 6: the inspector ===");
    {
        // Runtime layer: address pools without types (editors, scripting).
        const ecs::pool_ref transforms = world.find_pool_by_hash(ecs::hash_of<Transform>());
        std::printf("pool '%.*s': %zu live, %zu bytes/item\n",
                    static_cast<int>(transforms.name().size()),
                    transforms.name().data(),
                    transforms.size(),
                    transforms.info().bytes_per_item);

        const ecs::memory_footprint fp = world.footprint();
        std::printf("footprint: %zu bytes total (%zu component, %zu index)\n",
                    fp.total(),
                    fp.component_bytes,
                    fp.index_bytes);
        std::printf("validate: %s\n", world.validate() ? "OK" : "FAULT");
    }

    std::puts("=== act 7: the property grid ===");
    {
        // Reflection identity == pool identity (same name hash): enumerate with
        // components_of, resolve each hash, read fields from the live bytes.
        ecs::reflect<Health>().field<&Health::value>("value");
        ecs::reflect<SpriteRef>().field<&SpriteRef::atlas_index>("atlas_index");

        const ecs::entity player = world.select<const PlayerTag>().first();
        world.components_of(player,
                            [&](const ecs::pool_info& info)
                            {
                                const ecs::reflection type = ecs::reflection_of(info.name_hash);
                                if (!type)
                                {
                                    return;  // unregistered: not for this grid
                                }
                                void* bytes = world.find_pool(info.id).raw(player);
                                type.each_field(
                                    [&](const ecs::field& f)
                                    {
                                        const ecs::any value = f.get_at(bytes);
                                        long long shown = 0;
                                        if (const int* i = value.try_as<int>())
                                        {
                                            shown = *i;
                                        }
                                        else if (const auto* u = value.try_as<std::uint32_t>())
                                        {
                                            shown = *u;
                                        }
                                        std::printf("  %.*s.%.*s = %lld\n",
                                                    static_cast<int>(type.name().size()),
                                                    type.name().data(),
                                                    static_cast<int>(f.name().size()),
                                                    f.name().data(),
                                                    shown);
                                    });
                            });

        // Structural verbs work by hash too: a "remove component" button.
        const ecs::reflection sprite = ecs::reflection_of<SpriteRef>();
        std::printf("remove SpriteRef via reflection: %s\n",
                    sprite.remove_from(world, player) ? "ok" : "refused");
        std::printf("still has SpriteRef: %s\n", sprite.present_on(world, player) ? "yes" : "no");
    }
    return world.validate() ? 0 : 1;
}
