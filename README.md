# ecs

A single-header Entity Component System for real-time games and simulation.
C++23 - no dependencies - no RTTI - no exceptions of its own - MIT.

ecs is the entity storage and query layer of a game engine, nothing else.
Systems stay plain functions; the library never owns your game loop.

```cpp
#include <ecs.hpp>

ecs::world world;
ecs::entity player = world.spawn(Transform{{0, 0}}, Velocity{{1, 0}});

world.each<Transform, const Velocity>([](Transform& t, const Velocity& v)
{
    t.position += v.value;
});

ecs::command_buffer cmd;
world.each<Health>([&](ecs::entity e, Health& h) { if (h.value <= 0) cmd.kill(e); });
world.apply(cmd);  // the frame's one sync point
```

## Features

- 8-byte `{index, generation}` handles, parallel-array pools, no expression
  templates -- everything reads in a debugger watch window.
- Bonded pools: `bond<A, B, ...>()` mirror-partitions up to 8 owners for
  multi-component iteration at parallel-array speed.
- One query concept: `select<A, const B>(except<C>{})` -- callbacks, ranges,
  `maybe<T>`, `any_of<A, B>`, `split(n)` parallel chunks, `driven_by<T>()`.
- Frame-safe: `command_buffer` defers spawn/kill/add/remove; per-pool locks
  enforce the iteration rules in checked builds.
- Reactive: add/replace/remove hooks with RAII `scoped_hook`, `tracker<T>`
  drains, `watcher<...>` condition monitors. Events are entities.
- Save/load/merge: `pack`/`unpack` round-trip exact ids through your archive
  callables; `graft` merges with fresh ids and explicit `ecs_relink` relinking.
- Storage policies per component: `packed` (default), `stable` (pointer-stable
  chunks, non-movable types), `tag` (automatic for empty types); sortable.
- Extensible: `pool_of<T>` custom-storage seam, RTTI-free reflection registry,
  `ecs::any`, runtime queries, pmr allocators, `validate()` invariant audits.

Not a framework: no scheduler, no renderer, no scene graph.

## Getting started

Copy `src/ecs.hpp` into your project; that is the entire integration.
Components are plain structs; empty ones become zero-storage tags.
One knob: `ECS_CHECKS` (default: on without `NDEBUG`) compiles in stale-handle
detection and iteration locks.

## Building this repo

CMake presets with the vcpkg toolchain (`VCPKG_ROOT` must be set; the manifest
has zero dependencies).

```bat
build.bat   :: format + configure + clang-tidy + build + ctest
```

Tests, an example tour, and benchmarks live in `tests/`, `examples/`, `bench/`.

## License

MIT -- see [LICENSE](LICENSE.md).
