# quiver

A single-header Entity Component System for real-time games and simulation.
**C++23 · no dependencies · no RTTI · no exceptions of its own · MIT.**

quiver is the entity storage and query layer of a game engine — and nothing
else. It aims to be a true successor to the big-template ECSs: their full
capability surface — group-class paired iteration, signals, reactive
collections, snapshot loaders with entity remapping, runtime queries, world
singletons, parallel chunking — redesigned in one readable header, plus an
experimental `import quiver;` module. Systems stay plain functions; quiver
never owns your game loop.

```cpp
#include <quiver.hpp>
namespace ecs = quiver;

ecs::world world;

ecs::entity player = world.spawn(Transform{{0, 0}}, Velocity{{1, 0}});

world.each<Transform, const Velocity>([](Transform& t, const Velocity& v)
{
    t.position += v.value;
});

ecs::command_buffer cmd;
world.each<Health>([&](ecs::entity e, Health& h) { if (h.value <= 0) cmd.kill(e); });
world.apply(cmd);   // the frame's one sync point

// and the wider toolbox:
world.bond<Transform, Velocity>();                 // EnTT-group-class speed,
world.bonded<Transform, Velocity>().each(...);     // two pools, zero probes
for (auto [e, t, v] : world.select<Transform, const Velocity>().range()) { ... }
world.select<Sprite, ecs::maybe<Tint>>().each([](Sprite& s, Tint* tint) { ... });
ecs::scoped_hook h(world, world.on_add<Sprite, &Gpu::upload>(&gpu));  // RAII signals
ecs::tracker<Health> hurt(world);                  // drain added/replaced/removed
auto work = sel.split(threads);                    // parallel chunks, one lock
ecs::pack<Transform, Health>(world, writer);       // save / load / graft (remapped)
world.globals().obtain<MatchClock>();              // singletons, ECS-style
ecs::pipeline frame;  frame.stage("move", ...);    // stages + auto-applied buffer
ecs::blueprint goblin;  goblin.add<Health>(30);  world.spawn(goblin);
world.duplicate(boss);  world.sort<Sprite>(by_atlas);
```

## Why another ECS?

| | quiver |
|---|---|
| **Debuggable** | 8-byte `{index, generation}` handles, two parallel arrays per pool, no expression templates — everything reads in a debugger watch window. |
| **Fast where it counts** | **Bonded pools**: `bond<A, B>()` — or `bond<A, B, C, ...>()` up to 8 owners — keeps the N-way intersection mirror-partitioned in every owner: multi-component iteration at parallel-array speed (4–6× the probing select in our bench), one O(1) swap per owner on maintenance, no claim on any pool outside the owned set. Full owning-groups power, with the group-conflict matrix designed out (owned sets never overlap or nest). |
| **Frame-safe by design** | A built-in `command_buffer` (deferred spawn/kill/add/remove with nonce-protected provisional handles, spawn-acknowledgement callback); mutation during iteration is *enforced* by per-pool locks in checked builds, not just documented. A minimal `pipeline` runs your stages and auto-applies its deferred buffer between them. |
| **One query concept** | `select<A, const B>(except<C>{})` returns a `selection` — callbacks, range-`for` with structured bindings, `maybe<T>` optional components, `first()`, `split(n)` parallel chunks, `driven_by<T>()` manual driver override (pair with `sort<T>` for ordered iteration), and a stored object that *is* your compiled query. Point probes scale too: `has_all<Ts...>`/`has_any<Ts...>`. Plus a runtime layer (`pool_ref`/`runtime_selection`) for editors and scripting. |
| **Reactive** | Hooks connect function pointers, free functions, or member functions (`on_add<T, &Sys::fn>(&sys)`), with RAII `scoped_hook` lifetime and `tracker<T>` deduped drain-pattern collections — one pointer test when unused, dispatched under the lock discipline so reentrant misuse is a reported violation, not UB. `amend<T>(e, fn)` mutates in place and fires `on_replace` — the observed-write funnel, legal mid-iteration. Events? They're entities — no second vocabulary. |
| **Save/load/merge built in** | `pack`/`unpack` round-trip exact entity ids through YOUR archive callables (encoding stays yours); `graft` merges archives into live worlds with fresh ids and *explicit* entity-handle relinking via `quiver_relink`/`relink_traits` — never guessed from member shapes. |
| **Spawn tooling** | `blueprint` recipes stamped any number of times, `duplicate()` whole-entity cloning, `obtain()` get-or-add, `entity_ref` forwarding handles, `globals()` world singletons as plain components. |
| **Storage policies** | `packed` (default, fastest), `stable` (pointer-stable chunks, accepts non-movable types, tunable via `chunk_capacity<T>`), `tag` (automatic for empty types). `sort<T>` / `sort_along<F, L>` control iteration order — with an injectable algorithm (`sort<T>(cmp, algo)`, e.g. `std::stable_sort`) — and stable pools sort without invalidating pointers. |
| **Abstracts well** | A published custom-storage seam: specialize `pool_of<T>` (derive a built-in pool and override the cold virtuals, or build from scratch on `basic_pool` — the test suite ships a complete boxed-pool example) and every layer — selections, hooks, bonds, archives — drives your backend unchanged. Pin portable component identity with `quiver_label`/`component_label<T>` (cross-compiler archive keys); read/write any component type-erased via `pool_ref::raw()`. A full compile-time toolkit (`types<>`/`values<>` list algebra, `for_each_type`) powers component **manifests**: `using Saved = types<...>` drives `select`, `pack`/`unpack`/`graft`, and editor loops from one declaration, and every selection exports its lists back (`included`/`excluded`). |
| **Memory-aware** | `std::pmr::memory_resource*` accepted by world/buffers/trackers — no template ceremony; `footprint()` and per-pool byte counts report honest totals. |
| **Trustable** | `validate()` audits every internal invariant — including bond partitions — and the tests prove it detects corruption; misuse routes through a replaceable violation handler. |
| **C++23 native** | Deducing-this collapses the const/non-const API surface, `std::move_only_function` stages, `std::expected` faults, and an experimental `import quiver;` named module (`-DQUIVER_MODULE=ON`). |

What it is **not**: a framework. No scheduler, no reflection, no serializer,
no scene graph. See [DESIGN.md](DESIGN.md) for the full rationale, the
invalidation table, and the API originality checklist.

## Getting started

Copy `include/quiver.hpp` into your project. That is the entire integration.

Component types are plain structs; empty ones become zero-storage tags
automatically:

```cpp
struct Burning {};                                  // tag
struct Name { std::string value;
              static constexpr auto quiver_storage = quiver::storage::stable; };
```

The one configuration knob: `QUIVER_CHECKS` (defaults to on without `NDEBUG`)
compiles in stale-handle detection and iteration locks.

## Building this repo (tests, example, benchmark)

Uses CMake presets with the vcpkg toolchain (`VCPKG_ROOT` must be set; the
manifest has zero dependencies — quiver itself needs nothing).

```bat
build.bat                      :: format + configure + clang-tidy + build + ctest
```

or piecemeal:

```bat
cmake --preset default         :: VS 2022 x64, x64-windows-static
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build\Release\quiver_example.exe
build\Release\quiver_bench.exe
cmake --preset compile-db      :: Ninja + clang++ sidecar (clang-tidy / clangd)
```

- `tests/tests.cpp` — ~1800 framework-free assertions covering handles,
  storage policies, filters, bonds, hooks, trackers, archives, splits, the
  command buffer, violations, and negative validation.
- `examples/gameplay.cpp` — a six-act tour: pipeline frames over bonded
  iteration, member-function hooks, tracker-driven UI, events-as-entities,
  parallel split halves, save/load round-trip, a grafted reinforcement
  squad with relinked leaders, and an inspector finale (runtime queries,
  footprint, validate).
- `bench/bench.cpp` — order-of-magnitude timings with methodology notes
  (measure in *your* game before believing any ECS benchmark, including this
  one).

## License

MIT — see [LICENSE](LICENSE).
