# ecs

A single-header C++23 Entity Component System for real-time games and
simulation: plain-struct components, cache-friendly view queries, and
message-driven systems. The whole library is one file, `src/ecs.hpp` -- copy
it, `#include <ecs.hpp>`, and go. MIT-licensed.

**Contents**

- [What is an ECS?](#what-is-an-ecs) / [Hello world](#hello-world)
- **1**\. [Components](#1-components) **2**\. [Registry and entities](#2-registry-and-entities) **3**\. [Queries](#3-queries) **4**\. [Systems](#4-systems-and-the-frame-loop) **5**\. [Deferred changes](#5-deferred-changes)
- *opt-in:* **6**\. [Relationships](#6-relationships) **7**\. [Reacting to change](#7-reacting-to-change) **8**\. [Globals](#8-globals) **9**\. [Persistence](#9-persistence-and-spawning) **10**\. [Storage](#10-storage-policies) **11**\. [Reflection](#11-reflection-and-runtime-tooling) **12**\. [Customization](#12-advanced-customization) **13**\. [Diagnostics](#13-diagnostics-and-safety)
- [Performance](#performance) / [Cheat sheet](#cheat-sheet) / [Building](#building)

---

## What is an ECS?

A way to organize game state around *data* instead of *objects*:

- An **entity** is just an id -- an 8-byte handle with no data and no behavior ("a row number").
- A **component** is a plain data struct (`Position`, `Health`). You *compose* an entity by
  attaching components: a bullet is `Position + Velocity`, a wall is `Position`. No
  inheritance, no base class.
- A **system** is behavior: code that queries every entity with a given set of components and
  acts on them.

Components of one type live together in a tight array, so a query streams over contiguous
memory (fast, cache-friendly), and you change what an entity *is* at runtime by adding or
removing components. The **registry** owns everything; *you* own the frame loop.

## Hello, world

```cpp
#include <ecs.hpp>
#include <cstdio>

struct Position { float x = 0, y = 0; };
struct Velocity { float dx = 0, dy = 0; };

int main()
{
    ecs::registry world;
    ecs::entity e = world.create(Position{0, 0}, Velocity{1, 2});

    // For every entity with both, integrate. const = read-only; Position is mutable.
    world.each<Position, const Velocity>(
        [](Position& p, const Velocity& v) { p.x += v.dx; p.y += v.dy; });

    std::printf("%.1f, %.1f\n", world.get<Position>(e).x, world.get<Position>(e).y);  // 1.0, 2.0
}
```

---

## 1. Components

Any plain struct is a component; no registration or annotation is needed. Keep them small and
data-only (behavior lives in systems). An **empty** struct is a *tag*: zero storage, presence
is the whole message.

```cpp
struct Position { float x = 0, y = 0; };
struct Health   { int hp = 100; };
struct Frozen   {};   // a tag: filter on it, but it is never delivered to a callback
```

Components should be movable for the default storage; pin non-movable types (`std::atomic`,
`std::mutex`) with `stable` storage (see [Storage](#10-storage-policies)).

**Optional compile-time contracts.** Beyond plain C++23 `requires`, the library adds
**structural** predicates a system can demand of a component (see
[Reflection](#11-reflection-and-runtime-tooling)): `ecs::reflectable<T>` (a flat aggregate),
`ecs::has_field<T, "hp">` (a member named `hp`), `ecs::fields_all<T, std::is_floating_point>`
(every member floating-point). Compose them with `requires` for a clear compile error when a
component doesn't fit. (`has_field` needs `ecs::field_names_supported`, true on the usual toolchains.)

## 2. Registry and entities

`ecs::registry` owns everything; `ecs::entity` is an 8-byte handle you copy freely;
`ecs::no_entity` is null.

**Creating.**

```cpp
ecs::entity a = world.create(Position{1, 1}, Health{50});   // with components, one call (the common case)
ecs::entity b = world.create().component<Position>({2, 0}).component<Velocity>({0, -1});  // fluent
ecs::entity c = world.create();                                          // a bare id, fill in later
```

`.component<T>` attaches `T` (like `add<T>`) but returns the builder for chaining; `add<T>`
returns a reference to the new component. For spawning *waves* see Bulk below; for prefabs see
[Persistence and spawning](#9-persistence-and-spawning).

**Changing.** Six mutators that differ only by what they do when the component is absent vs
present, and which [hook](#7-reacting-to-change) fires:

| Call | absent | present | hook | reach for it when |
|---|---|---|---|---|
| `add<T>(e,...)` | attaches | **error** (dup) | `on_add` | attaching something new |
| `put<T>(e,...)` | attaches | overwrites | add/replace | "set this", existed or not |
| `obtain<T>(e,...)` | attaches | returns **unchanged** | `on_add` if added | "make sure it exists", then use it |
| `replace<T>(e,...)` | **error** | overwrites | `on_replace` | a known component, fresh value |
| `amend<T>(e,fn)` | **error** | `fn(T&)` in place | `on_replace` | edit in place **and** notify observers |
| `touch<T>(e)` | **error** | no change | `on_replace` | re-notify observers without editing |

```cpp
world.add<Velocity>(a, {1, 0});                          // a must NOT already have it
world.put<Health>(a, {40});                              // upsert
world.amend<Health>(a, [](Health& h){ h.hp -= 5; });    // edit, observed
```

`replace`/`amend`/`touch` edit in place (a held `Health&` stays valid); `add` may move packed
components. A bare `world.get<Health>(a).hp -= 5` works but is **invisible** to hooks and
[trackers](#7-reacting-to-change); route observed writes through `amend` (or call `touch<T>`
after a manual edit).

**Reading.** Every call below asks about the *one* entity `a` (its own components); to find
*entities* across the world, use a [query](#3-queries):

```cpp
bool yes      = world.has<Velocity>(a);                  // one type
Velocity& v   = world.get<Velocity>(a);                  // reference; misuse aborts (see below)
Velocity* opt = world.find<Velocity>(a);                 // pointer, or nullptr (absence is fine)
auto [p, h]   = world.find_all<Position, Health>(a);     // tuple of pointers, null where absent
bool both     = world.has_all<Position, Health>(a);      // also has_any; both need >= 2 types
```

> **Abort rule.** `get`/`add`/`replace`/`amend` treat misuse (missing, duplicate, dead handle)
> as a bug: in checked builds they abort with a diagnostic; with `ECS_CHECKS` off, misuse is
> undefined behavior (e.g. a null deref). Use `find`/`has`/`put`/`obtain` when absence is
> expected. (See [Diagnostics](#13-diagnostics-and-safety).)

**Lifecycle.** Destroying bumps a generation counter, so an old copy of the handle reads "not
alive" even after the slot is reused, leaving no dangling ids.

```cpp
world.destroy(a);
bool live = world.alive(a);                          // false; stale handles detected
auto dup  = world.duplicate(b);                      // duplicate_result: the clone + copied/skipped
ecs::entity clone = dup;                             //   counts; converts implicitly to entity
std::size_t n = world.live_count();
world.each([](ecs::entity e) { /* every live entity */ });   // no-component each()
```

**A fluent handle.** `world.ref(e)` wraps `(world, entity)` into an `entity_filler` that forwards
every per-entity verb and converts back to `ecs::entity`:

```cpp
auto hero = world.ref(player);
hero.component<Position>({0, 0}).component<Health>({100});   // chain verbs on one entity
```

**Bulk.** Spawn or clear *waves* in one call instead of looping (`reserve_entities` / `reserve<T>`
pre-size the storage first):

```cpp
std::vector<ecs::entity> mob;
world.create_n(100, std::back_inserter(mob));                 // 100 ids into a buffer
world.insert<Health>(mob.begin(), mob.end(), Health{5});      // broadcast a value (or pass a
world.destroy(mob.begin(), mob.end());                        //   value iterator for per-entity)
```

(`create_n`, not `create(n)`: a count would look like just another component.)

## 3. Queries

A query visits every entity that has *all* listed components. The key idea: **how you spell a
component decides its role.**

| Spelling | Role | Callback gets |
|---|---|---|
| `T` | required, read-write | `T&` |
| `const T` | required, read-only | `const T&` |
| `exists<T>{}` / `!exists<T>{}` | required / forbidden, not delivered | nothing |
| `maybe<T>` | optional | `T*` (null if absent) |

```cpp
// One-shot:
world.each<Position, const Velocity>([](Position& p, const Velocity& v){ p.x += v.dx; });

// Reusable view (stateless and copy-cheap -- just pool pointers). Build once, query often:
auto movers = world.view<Position, const Velocity>();
movers.each([](Position& p, const Velocity& v){ p.x += v.dx; });
```

`world.each<A,B>(fn)` is exactly `world.view<A,B>().each(fn)`; reach for the view when you also
want `count()`/`first()`/`contains()` or want to keep it across frames. A view is an inner join
driven off the smallest pool, and never allocates while iterating.

**Callback shape.** An optional leading `entity` parameter, and a `bool` return to stop early
(your "find first" / "take while"):

```cpp
ecs::entity firstDead = ecs::no_entity;
world.view<const Health>().each([&](ecs::entity e, const Health& h)
{
    if (h.hp <= 0) { firstDead = e; return false; }   // false stops the loop
    return true;
});
```

**Filters** narrow without delivering the component; combine with `&&`/`||`/`!`. `exists<T>`
tests membership; `where<T>(pred)` tests a component *value*:

```cpp
world.each<Health>([](Health& h){ /* ... */ }, ecs::exists<Frozen>{} && !ecs::exists<Burning>{});
world.view<Sprite>(ecs::where<Health>([](const Health& h){ return h.hp < 25; }))
    .each([](Sprite& s){ s.flash = true; });                       // value filter; Health not delivered
world.view<const Position, ecs::maybe<const Tint>>().each(
    [](const Position& p, const Tint* tint){ if (tint) { /* tinted */ } });   // maybe<> -> pointer
```

A one-off value test can also just stay inside the callback: `if (p.x < 0) return;`.

**Element access.** Answer questions without writing a loop:

```cpp
auto v = world.view<Health>();
v.empty();  v.contains(e);  v.count();
v.first();    // first match, or no_entity
v.back();     // last match
v.single();   // THE one match. In checked builds zero-or-many is a violation (a uniqueness
              // assert, like LINQ Single); in Release it returns the sole match, else no_entity.
for (auto&& [e, h] : v.each())     { /* ascending  */ }
for (auto&& [e, h] : v.reversed()) { /* descending */ }
std::vector<ecs::entity> ids = v.collect();   // materialize (this one allocates)
```

**Ordering.** You never sort to *find* (random access is O(1) regardless of order); sorting only
changes the *visit order*. `world.sort<Position>(cmp)` reorders a pool once (e.g. draw
back-to-front); `view.driven_by<Position>().each(...)` picks the driver for one pass without
touching storage.

**Parallelism.** `view.split(n)` hands you `n` independent chunks (run `work.part(i).each(...)`
on your own threads; the library spawns none).

**A specific set of entities.** `view.each_of(span, fn)` joins the view against an externally
produced list, visiting only the alive and matching ones -- this is how a
[tracker](#7-reacting-to-change) drain (section 7) or a relationship subtree (section 6) becomes a
query.

> Writing component *values* mid-query is fine; **structural** edits to the iterated pool are a
> checked violation -- defer them to a [command buffer](#5-deferred-changes) (next section).

## 4. Systems and the frame loop

A **system** reacts to a typed **message event** (a value you dispatch). Systems are grouped
into **features** (any tag type) so you can toggle a whole group.

```cpp
struct update_event { float dt; };

class movement_system : public ecs::system<update_event>
{
public:
    void process(ecs::registry& w, const update_event& ev) override
    {
        w.each<Position, const Velocity>([&](Position& p, const Velocity& v)
        { p.x += v.dx * ev.dt; p.y += v.dy * ev.dt; });
    }
};

struct sim_feature {};
world.feature<sim_feature>()
    .add_system<gravity_system>()
    .add_system<movement_system>().after<gravity_system>();   // ordering edge

while (running) world.dispatch(update_event{dt});             // you own the loop
world.disable_feature<sim_feature>();                         // and enable_feature<F>()
```

- `.after<Other>()` orders one system after another (stable topo-sort; a cycle is a checked
  violation). A system can handle several events (`system<A, B>`, one `process` override each)
  and may re-`dispatch` a sub-event (nested dispatch is self-contained, up to a depth cap).
- Wiring systems or listeners *while a dispatch is in flight* is a checked violation -- change
  wiring between frames.

**Immediate vs queued.** `dispatch(ev)` runs handlers now. To *collect* events during a frame
and run them at one point, enqueue then flush (a flush snapshots first, so events enqueued
*during* it wait for the next flush, preventing runaway loops):

```cpp
world.enqueue(damage_event{target, 5});
world.flush<damage_event>();   // or flush() for every queued type; queued<E>() counts
```

**Listeners.** For a small reaction that captures local state, `connect<E>` registers any
callable -- `(registry&, const E&)` or just `(const E&)` -- and returns a `listener_token` you
pass to `disconnect`:

```cpp
auto t = world.connect<damage_event>([&log](const damage_event& ev){ log.record(ev); });
```

> **Two unrelated "event" notions:** dispatched **message events** here (values ->
> systems/listeners) vs reactive **change events** in [section 7](#7-reacting-to-change)
> (entities -> trackers/watchers). They don't interact.

## 5. Deferred changes

You may write component *values* during a query, but **structural** changes
(create/destroy/add/remove on the pool you're iterating) are refused mid-loop, since they'd
move the array under you. Record them in a **command buffer** and replay at one sync point:

```cpp
ecs::command_buffer cmd;
world.view<const Health>().each([&](ecs::entity e, const Health& h)
{ if (h.hp <= 0) cmd.destroy(e); });   // recorded, not applied
world.apply(cmd);                       // the frame's sync point; clears the buffer
```

It records `create`/`add`/`put`/`remove`/`destroy` plus `adopt`/`orphan`/`destroy_subtree`. A
buffered `cmd.create()` returns a *provisional* handle; to recover the real id afterward, use
the resolving overload:

```cpp
world.apply(cmd, [](ecs::entity provisional, ecs::entity real){ /* remember the mapping */ });
```

Inside a system, record into `world.deferred()` instead; the dispatcher applies it
automatically after each handler (system or listener), so the next one sees the changes.

---

> **Everything below is opt-in.** A complete game needs only sections 1-5. Relationships,
> change-reactions, persistence, custom storage, and reflection are tools you add when a
> specific need arises -- skip ahead and come back.

## 6. Relationships

An optional, sparse **single-parent tree** over the flat registry, storing just topology (no
transforms, no scene graph) -- for scene graphs, bone trees, UI panels, inventories, a squad and
its members. It's one built-in `kin` component (parent + sibling links): edits are O(1) and free
for unlinked entities, but **traversal is pointer-chasing**, not the contiguous scan a query is.

```cpp
world.adopt(parent, child);   world.orphan(child);
ecs::entity p = world.parent_of(child);   std::size_t k = world.child_count(parent);
world.children_of(parent, [](ecs::entity c){ /* direct children, in order */ });

world.descendants_of(root, [](ecs::entity d){ /* pre-order subtree */ });
world.ancestors_of(node, [](ecs::entity a){ /* parent, grandparent, ... */ });
world.roots([](ecs::entity r){ /* every entity in the tree with no parent */ });
std::size_t removed = world.destroy_subtree(root);  // destroy root + all descendants; returns the count
ecs::entity copy = world.duplicate_subtree(root);   // clone subtree, preserving internal links
```

(Plain `destroy` unlinks one entity, so its children become roots; `destroy_subtree` cascades.
`adopt`/`orphan`/`destroy_subtree` are command-buffer recordable. `roots` lists only entities
*in* the tree (a never-adopted entity isn't reported). Mutating links mid-traversal is a violation.)

**Walk vs query** answer different questions: *"every entity with A and B"* is a **query**
(`view<A,B>`, the contiguous fast path -- the query *is* the group); *"the subtree of this one
entity"* is a **walk** that follows `kin` links, costing only the nodes visited -- right for a
subtree of dozens or hundreds, not for sweeping the world.

To combine ("every descendant of `root` with a `Health`"), collect the subtree and join it
against a view:

```cpp
std::vector<ecs::entity> sub;
world.descendants_of(root, [&](ecs::entity d){ sub.push_back(d); });
world.view<Health, const Sprite>().each_of(sub, [](Health& h, const Sprite& s){ /* ... */ });
```

## 7. Reacting to change

Three tools that differ in *when* they tell you and *what shape* the answer is:

| Tool | Fires | You get | For |
|---|---|---|---|
| **hook** | synchronously, in the verb | a callback per change | side effects *at* the change (GPU upload, audio) |
| **`tracker<T>`** | you poll, once a frame | a deduped list of changed entities | batch reactions, decoupled from timing |
| **`watcher<...>`** | you poll | entities that just *entered* a condition | "became burning + unshielded this frame" |

A **hook** is called `(registry&, entity)` (or just `(entity)`, or a bound member); it is **not**
handed the component -- call `world.get<T>(e)` inside if you need the value. Register a free
function plus `void* user`, or a member via `scoped_hook` (RAII):

```cpp
struct GpuSprites { void upload(ecs::registry& w, ecs::entity e) { send(w.get<SpriteRef>(e)); } };
GpuSprites gpu;
ecs::scoped_hook up(world, world.on_add<SpriteRef, &GpuSprites::upload>(&gpu));
// also on_remove / on_replace. Free-function form: on_add<T>(&fn, user), fn a
// void(*)(registry&, entity, void*); any form returns a hook_token -> world.unhook(token).
```

Topology changes have parallel hooks: `world.on_adopt` / `on_orphan` / `on_reorder` (managed
with `ecs::scoped_relationship_hook`), mirroring the component hooks above.

```cpp
// tracker<T>: a deduped drain you read once a frame. Only replace/put/amend/touch count as a
// "replace" (a bare get<T> write is invisible by design). The drain feeds each_of:
ecs::tracker<Health> hits(world, ecs::track::replaced);   // or added / removed / all
world.view<const Health, Sprite>().each_of(hits.replaced(),
    [](const Health& h, Sprite& s){ s.flash = (h.hp < 25); });
hits.clear();

// watcher: a multi-component CONDITION; collects entities that began matching. matched() is
// live truth -- an entity that stops matching is evicted automatically.
ecs::watcher<ecs::types<Burning, Health>, ecs::except<Shielded>> on_fire(world);
for (ecs::entity e : on_fire.matched()) { /* just became burning + unshielded */ }
on_fire.clear();
```

(`changed<C>` in a watcher also collects entities whose `C` was replaced while matching;
`on_replace<T>` and `changed<C>` reject tags. `tracker` and `watcher` point into the registry
and are non-movable -- create them after the world and destroy them before it.)

## 8. Globals

World-scoped state (camera, clock, input) lives on the `globals()` entity, a lazily created
singleton, so you don't invent a resource system. Every section-2 verb works on it *except*
`destroy()` (clearing world state is `reset()`), and because it's a real entity it rides along
in `pack` and `live_count`.

```cpp
struct MatchClock { float elapsed = 0; };
world.globals().obtain<MatchClock>();              // create on first use
world.globals().get<MatchClock>().elapsed += dt;
```

## 9. Persistence and spawning

Two overlapping needs: getting a world to and from bytes, and stamping out many copies of a
recipe.

**Save / load.** Serialization is *typed*: `ecs` owns ordering and identity; you own the byte
encoding through a tiny writer/reader callable (`void operator()(const auto&)` /
`void operator()(auto&)`). Two ways to load, differing in **which ids the entities get**:

```cpp
// pack/unpack: the EXACT same ids into an EMPTY world -- saves, rollback, snapshots where
// handles stored elsewhere must stay valid. pack cannot fail; unpack returns std::expected.
ecs::pack<Position, Velocity, Health>(world, out);
auto r = ecs::unpack<Position, Velocity, Health>(restored, in, /*max_entities=*/100000);
if (!r) { /* r.error().code: world_not_empty, archive_mismatch, archive_too_large */ }

// graft: FRESH ids into a POPULATED world (nothing collides) -- load a level chunk, instance a
// sub-scene. Returns an old->new map (also std::expected).
auto map = ecs::graft<Position, Velocity, Health>(world, in);
ecs::entity now = map->resolve(oldHandle);
```

Several guards make loads robust:

- **Schema-drift detection**: a consteval per-component fingerprint (type identity, size,
  alignment, field count, names) fails with `archive_mismatch` instead of misreading bytes.
- **Empty-world rule**: `unpack` refuses a non-empty target (`world_not_empty`).
- **`max_entities` cap**: bounds an untrusted stream *before* any allocation.
- **Explicit relinking**: `graft` renumbers, so a component holding an `ecs::entity` opts in with
  `void ecs_relink(const ecs::graft_map& m) { foe = m.resolve(foe); }` (or a `relink_traits`
  specialization for a foreign type).

Parent/child links serialize separately, *after* the component pass: `pack_links` /
`unpack_links` / `graft_links`.

**Blueprints.** A recorded set of component values to stamp out repeatedly:

```cpp
ecs::blueprint goblin(Health{30}, Velocity{-1, 0});
ecs::entity one = world.create(goblin);                 // create(bp) returns a chainable filler
world.create(goblin, 100, [](ecs::entity e){ /* tweak each instance */ });   // a hundred
```

## 10. Storage policies

Each component type picks its pool layout; the default is almost always right:

| Policy | Layout | Choose when |
|---|---|---|
| `packed` (default) | dense array, fastest iteration | almost always |
| `stable` | pointer-stable chunks | non-movable types, or you hold component pointers across churn |
| `tag` | membership only, zero storage | **automatic** for empty structs |

```cpp
struct Body { std::mutex lock;   // non-movable -> must be stable
    static constexpr ecs::storage ecs_storage = ecs::storage::stable; };
// for a type you don't own:  template <> inline constexpr ecs::storage ecs::storage_policy<X> = ...;
```

For a fully custom backend (SoA, an arena), specialize the `ecs::pool_of<T>` seam.

## 11. Reflection and runtime tooling

Two kinds: **registered** -- a process-wide, RTTI-free registry keyed by type name, so editors,
consoles, and savers work with components *by string* at runtime (opt in with `reflect<T>()`); and
**aggregate** -- a zero-registration compile-time walk over a flat struct's members, for generic
C++ that never needs the string name.

```cpp
// Registered (once per process):
ecs::reflect<Position>().fields("x", "y");          // names (positional .fields() -> "0","1")
ecs::reflect<Transform>().field<&Transform::pos>("pos").method<&Transform::reset>("reset");

ecs::reflection r = ecs::reflection_of<Position>();  // or reflection_of("Position") / (hash)
ecs::any boxed = ecs::any::make<Position>(Position{1, 2});   // make<T> owns; ref(obj) aliases
ecs::set(boxed, "x", ecs::any::make<float>(9.0F));   // get/set a field or invoke a method, by name
r.each_field([](const ecs::field& f){ /* enumerate */ });
ecs::for_each([](const ecs::reflection& t){ /* every registered type */ });

world.each_pool(fn);  world.components_of(e, fn);    // inspect pools / an entity with no static type

// Aggregate (no registration) -- operates on a real struct value, not an `any`:
Position p{1, 2}, q{1, 2};
ecs::for_each(p, [](auto& m){ /* each member by reference */ });
std::size_t h = ecs::hash_fields(p);   bool eq = ecs::fields_equal(p, q);
ecs::write_fields(out, p);  ecs::read_fields(in, p);   // layout-portable codec
```

`ecs::string_id("camera")` hashes a string into the same id space as `hash_of<T>()`, so your own
string keys share one namespace.

## 12. Advanced customization

**Compile-time type lists.** `ecs::types<...>` is a manifest you can pass and manipulate at
compile time:

```cpp
using Saved = ecs::types<Position, Velocity, Health>;
ecs::pack(world, out, Saved{});                          // manifest form of pack<...>
ecs::for_each(Saved{}, []<class T>(){ /* once per type */ });
```

Set algebra (`joined_t`, `intersection_t`, `difference_t`, `filter_t`, ...) and predicate folds
(`all_of_v`, `any_of_v`, `count_if_v`) build larger manifests from smaller ones.

**Custom entity traits.** The 8-byte default handle (31-bit index, 32-bit generation) suits most
games; for a smaller handle, template the registry on your own traits:

```cpp
struct small_traits {
    using index_type = std::uint16_t;  using generation_type = std::uint16_t;
    static constexpr std::uint32_t index_bits = 15;        // 1 bit reserved; bounds the slot count
};
ecs::basic_registry<small_traits> small_world;            // ~4-byte handles, up to 32k slots
```

Every public type has a `basic_*<Traits>` form; `registry`/`view`/... are aliases for the
default. Entities are hashable: `std::unordered_map<ecs::entity, int, ecs::entity_hash>`.

## 13. Diagnostics and safety

One knob, **`ECS_CHECKS`** (on unless `NDEBUG`): stale-handle detection, iteration locks, and
consistency asserts, routed through a replaceable handler.

During a query you may write component *values* and call bare `create()` or a command buffer;
what's **refused** (reported, then no-op) is a structural change to the pool being iterated
(remove, tag add, destroy, sort, hook connect/disconnect), `apply`/`reset` while iterating, and
`connect`/`disconnect`/`add_system` while dispatching.

```cpp
if (auto v = world.validate(); !v)                       // expected<void, fault>
    std::printf("broken: %s\n", v.error().note);         // fault { code, pool, note }
ecs::memory_footprint mem = world.footprint();           // entity/component/index/bookkeeping bytes + total()
auto prev = ecs::set_violation_handler([](const char* msg){ log_error(msg); });  // returns the old handler
```

`ecs` throws nothing of its own (only cold grow/reserve may propagate `std::bad_alloc`); hot
lookups and iteration never allocate.

---

## Performance

Shaped so the per-frame query stays well inside a frame budget:

- **O(1) random access**: sparse sets; `get`/`has`/`find` are index math, no searching.
- **Contiguous iteration**: dense packed arrays; no pointer chasing, no virtual call in the loop.
- **Smallest-pool join**: `view<A,B>` walks the smaller pool and probes the rest in O(1).
- **No allocation** building or iterating a view.

In practice a one- or two-component view iterates at a few nanoseconds per entity (release,
`ECS_CHECKS` off), so a typical update over tens of thousands of entities costs tens of
*micro*seconds -- but *measure your own game*. To keep it there: mark read-only components
`const`, defer structural edits to a [command buffer](#5-deferred-changes), iterate with queries
(not [trees](#6-relationships)), use `split(n)` for threaded passes and `sort<T>` once for ordered
ones, and ship with `NDEBUG`.

---

## Cheat sheet

| Task | Call |
|---|---|
| Create | `create(A{}, B{})` / `create()` / `.component<A>(...)` / `create_n(n, out\|fn)` |
| Add / remove | `add<T>(e,...)` / `remove<T>(e)` / `insert<T>(first,last,val\|valueIt)` |
| Read | `get` (checked-abort) / `find` (nullptr) / `has` / `has_all` / `has_any` (>=2) / `find_all` |
| Change | `put` (upsert) / `obtain` (get-or-add) / `replace` (must exist) / `amend<T>(e,fn)` (observed) / `touch<T>(e)` (re-notify) |
| Destroy | `destroy(e)` / `destroy(first,last)` / `destroy_subtree(root)` |
| Query | `each<A, const B>(fn[, filter])` / `view<...>()` |
| Roles | `T` (ref) / `const T` (read-only) / `exists<T>{}` (membership) / `where<T>(pred)` (value) / `maybe<T>` (pointer) |
| View ops | `each` / `entities(fn)` / `count` / `first` / `back` / `single` / `reversed` / `each_of(span)` / `collect` |
| Order / parallel | `sort<T>(cmp)` / `driven_by<T>()` / `split(n)` |
| Systems | `feature<F>().add_system<S>().after<O>()` ; `dispatch(ev)` |
| Queue / listen | `enqueue(ev)` / `flush<E>()` / `queued<E>()` ; `connect<E>(fn) -> token` / `disconnect` |
| Defer | `command_buffer` (you apply) / `world.deferred()` (auto) ; `apply(cmd[, on_spawn])` |
| Relationships | `adopt` / `children_of` / `descendants_of` / `parent_of` / `destroy_subtree` |
| React | hooks (get `(registry&, entity)`) + `scoped_hook` ; `tracker<T>` ; `watcher<types<...>, except<...>, changed<C>>` |
| Globals | `world.globals().obtain<T>()` / `.get<T>()` |
| Save / load | `pack`/`unpack` (exact ids) / `graft` (fresh ids) (+ `pack_links`, `max_entities`, `ecs_relink`) |
| Prefab | `ecs::blueprint bp(A{}, B{})` ; `create(bp[, count])` |
| Storage | `static constexpr ecs::storage ecs_storage` ; `pool_of<T>` |
| Reflect | `reflect<T>().fields(...)` ; `reflection_of(...)` ; `get/set/invoke(any, name)` |
| Inspect | `each_pool` ; `components_of` ; `validate()` ; `footprint()` |

## Building

Using the library is trivial: it's all in `src/ecs.hpp`. Copy that one header into your project
and `#include <ecs.hpp>` -- there's no build step, no link step, and no dependency. The commands
below only build the *bundled* test suite (C++23; the repo assumes MSVC + `x64-windows-static`
via vcpkg, whose manifest pulls only `gtest`):

```bat
build.bat   :: format + configure + clang-tidy + build the tests
test.bat    :: build and run the Google Test binary
```

## License

This project is licensed under the MIT License - see the LICENSE.md file for details.
