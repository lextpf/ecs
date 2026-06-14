# ecs

A single-header C++23 Entity Component System for real-time games and
simulation: plain-struct components, cache-friendly view queries, and
message-driven systems. MIT-licensed.

**Contents**

- [What is an ECS?](#what-is-an-ecs) / [Hello world](#hello-world)
- **1**\. [Components](#1-components) **2**. [Registry and entities](#2-registry-and-entities) **3**. [Queries](#3-queries)
- **4**\. [Systems](#4-systems-and-the-frame-loop) **5**. [Deferred changes](#5-deferred-changes) **6**. [Relationships](#6-relationships)
- **7**\. [Reacting to change](#7-reacting-to-change) **8**. [Globals](#8-globals) **9**. [Save / load](#9-save-load-and-merge)
- **10**\. [Blueprints](#10-blueprints) **11**. [Storage](#11-storage-policies) **12**. [Reflection](#12-reflection-and-runtime-tooling)
- **13**\. [Compile-time toolkit](#13-compile-time-toolkit) **14**. [Custom traits](#14-custom-entity-traits) **15**. [Diagnostics](#15-diagnostics-and-safety)
- [Performance](#performance) / [Cheat sheet](#cheat-sheet) / [Building](#building)

---

## What is an ECS?

A way to organize game state around *data* instead of *objects*:

- An **entity** is just an id, an 8-byte handle with no data and no behavior ("a row number").
- A **component** is a plain data struct (`Position`, `Health`). You *compose* an entity
  by attaching components; a bullet is `Position + Velocity`, a wall is `Position`. No
  inheritance, no base class.
- A **system** is behavior: code that queries every entity with a given set of
  components and acts on them.

Components of one type live together in a tight array, so a query streams over
contiguous memory (fast, cache-friendly), and you change what an entity *is* at runtime
by adding/removing components. The **registry** owns everything; *you* own the frame
loop.

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
data-only (behavior lives in systems). An **empty** struct is a *tag*: zero storage,
presence is the whole message.

```cpp
struct Position { float x = 0, y = 0; };
struct Health   { int hp = 100; };
struct Frozen   {};   // a tag: can be filtered, but never delivered to a callback
```

Components should be movable for the default storage; pin non-movable types
(`std::atomic`, `std::mutex`) with `stable` storage (see [Storage](#11-storage-policies)).

**Contracts (optional, compile-time).** A system can *require* a component to provide
certain members. Member/method requirements are plain C++23 `requires`; the library
adds the **structural** checks `requires` can't express, reusing the compile-time
reflection from [section 12](#12-reflection-and-runtime-tooling):

```cpp
ecs::reflectable<T>                          // T is a flat aggregate
ecs::has_field<T, "hp">                      // ...with a member spelled "hp"
ecs::fields_all<T, std::is_floating_point>   // ...whose every member is floating-point

template <class T>
concept Damageable = ecs::has_field<T, "hp"> && requires(T& t, int n) { t.hurt(n); };
world.each<Health>([](Damageable auto& h) { h.hurt(1); });   // clear compile error if Health doesn't fit
```

(You can *check* a field name but not synthesize an accessor from a string; `has_field`
needs `ecs::field_names_supported`, true on the usual toolchains.)

## 2. Registry and entities

`ecs::registry` owns everything; `ecs::entity` is an 8-byte handle you copy freely;
`ecs::no_entity` is null.

**Creating.**

```cpp
ecs::entity a = world.create(Position{1, 1}, Health{50});   // with components, one call
ecs::entity b = world.create().component<Position>({2, 0}).component<Velocity>({0, -1});  // fluent
```

| Form | Use when |
|---|---|
| `create()` | you'll attach components later (or just want an id) |
| `create(A{}, B{})` | you know the components up front (the common case) |
| `create().component<A>(...)` | values are built up step by step |
| `create_n(n, out\|fn)` | spawning a *wave* (Bulk, below) |
| `create(blueprint[, n])` | stamping a prefab (see [Blueprints](#10-blueprints)) |

`.component<T>` attaches `T` (like `add<T>`) but returns the builder for chaining; `add<T>`
returns a reference to the new component.

**Changing.** Five mutators that differ only by what they do when the component is
absent vs present, and which [hook](#7-reacting-to-change) fires:

| Call | absent | present | hook | reach for it when |
|---|---|---|---|---|
| `add<T>(e,...)` | attaches | **error** (dup) | `on_add` | attaching something new |
| `put<T>(e,...)` | attaches | overwrites | add/replace | "set this", existed or not |
| `obtain<T>(e,...)` | attaches | returns **unchanged** | `on_add` if added | "make sure it exists", then use it |
| `replace<T>(e,...)` | **error** | overwrites | `on_replace` | a known component, fresh value |
| `amend<T>(e,fn)` | **error** | `fn(T&)` in place | `on_replace` | edit in place **and** notify observers |

```cpp
world.add<Velocity>(a, {1, 0});                          // a must NOT already have it
world.put<Health>(a, {40});                              // upsert
world.amend<Health>(a, [](Health& h){ h.hp -= 5; });    // edit, observed
```

`replace`/`amend` edit in place (a held `Health&` stays valid); `add` may move packed
components. A bare `world.get<Health>(a).hp -= 5` works but is **invisible** to hooks and
[trackers](#7-reacting-to-change). Route observed writes through `amend`.

**Reading.** Every call below asks about the *one* entity `a` (its own components); to
find *entities* across the world, use a [query](#3-queries):

```cpp
bool yes      = world.has<Velocity>(a);                  // also has_all<A,B>, has_any<A,B>
Velocity& v   = world.get<Velocity>(a);                  // reference; ABORTS if absent
Velocity* opt = world.find<Velocity>(a);                 // pointer, or nullptr (no abort)
auto [p, h]   = world.find_all<Position, Health>(a);     // tuple of pointers, null where absent
```

> **Abort rule.** `get`/`add`/`replace`/`amend` treat misuse (missing, duplicate, dead
> handle) as a bug and abort. Use `find`/`has`/`put`/`obtain` when absence is expected.
> Checked builds print a diagnostic first ([Diagnostics](#15-diagnostics-and-safety)).

**Lifecycle.** Destroying bumps a generation counter, so an old copy of the handle reads
"not alive" even after the slot is reused, leaving no dangling ids.

```cpp
world.destroy(a);
bool live = world.alive(a);                          // false; stale handles detected
ecs::entity clone = world.duplicate(b);              // copies all of b's components
std::size_t n = world.live_count();
world.each([](ecs::entity e) { /* every live entity */ });   // no-component each()
```

**A fluent handle.** `world.ref(e)` wraps `(world, entity)` so you can chain verbs; it
forwards every per-entity verb and converts back to `ecs::entity`:

```cpp
ecs::entity_filler hero = world.ref(player);
hero.component<Position>({0, 0}).component<Health>({100});
if (hero.has_all<Position, Health>()) hero.amend<Health>([](Health& h){ h.hp -= 1; });
```

**Bulk.** Spawn or clear *waves* in one pre-sized call instead of looping:

```cpp
std::vector<ecs::entity> mob;
world.create_n(100, std::back_inserter(mob));                 // 100 ids into a buffer
world.insert<Health>(mob.begin(), mob.end(), Health{5});      // splat one value (or zip a range)
world.destroy(mob.begin(), mob.end());                        // destroy a range
```

(`create_n`, not `create(n)`: a count would otherwise look like just another component.)

## 3. Queries

A query visits every entity that has *all* listed components. The key idea: **how you
spell a component decides its role.**

| Spelling | Role | Callback gets |
|---|---|---|
| `T` | required, read-write | `T&` |
| `const T` | required, read-only | `const T&` |
| `exists<T>{}` / `!exists<T>{}` | required / forbidden, not delivered | nothing |
| `maybe<T>` | optional | `T*` (null if absent) |

```cpp
// One-shot:
world.each<Position, const Velocity>([](Position& p, const Velocity& v){ p.x += v.dx; });

// Reusable view (stateless and copy-cheap, just pool pointers). Build once, query often:
auto movers = world.view<Position, const Velocity>();
movers.each([](Position& p, const Velocity& v){ p.x += v.dx; });
```

`world.each<A,B>(fn)` is exactly `world.view<A,B>().each(fn)`; reach for the view when you
also want `count()`/`first()`/`contains()` or want to keep it across frames. A view is an
inner join driven off the smallest pool, and never allocates while iterating.

**Callback shape.** An optional leading `entity` parameter, and a `bool` return to stop
early (your "find first" / "take while"):

```cpp
ecs::entity firstDead = ecs::no_entity;
world.view<const Health>().each([&](ecs::entity e, const Health& h)
{
    if (h.hp <= 0) { firstDead = e; return false; }   // false stops the loop
    return true;
});
```

**Filters** narrow by membership without delivering the component; combine with `&&`/`||`:

```cpp
world.each<Health>([](Health& h){ /* ... */ }, ecs::exists<Frozen>{} && !ecs::exists<Burning>{});
world.view<const Position, ecs::maybe<const Tint>>().each(
    [](const Position& p, const Tint* tint){ if (tint) { /* tinted */ } });   // maybe<> -> pointer
```

Filtering on a component *value* (not membership) stays inside the callback: `if (p.x < 0) return;`.

**Element access.** Answer questions without writing a loop:

```cpp
auto v = world.view<Health>();
v.empty();  v.contains(e);  v.count();
v.first();    // first match, or no_entity
v.back();     // last match
v.single();   // THE one match. Checked: zero or many is a violation (a uniqueness assert,
              // like LINQ Single(); use it for "the player", "the active camera")
for (auto&& [e, h] : v.each())     { /* ascending  */ }
for (auto&& [e, h] : v.reversed()) { /* descending */ }
std::vector<ecs::entity> ids = v.collect();   // materialize (this one allocates)
```

**Ordering.** You never sort to *find* (random access is O(1) regardless of order);
sorting only changes the *visit order*. `sort<T>` reorders a pool once (draw
back-to-front, resolve by initiative); `driven_by<T>` picks the driver for a single pass
without touching storage:

```cpp
world.sort<Position>([](const Position& a, const Position& b){ return a.x < b.x; });
world.view<Position, const Velocity>().driven_by<Position>().each(/* ... */);
```

**Parallelism.** `split(n)` hands you `n` chunks for your own threads (the library spawns
none):

```cpp
auto work = world.view<Position, const Velocity>().split(4);
for (std::size_t i = 0; i < work.parts(); ++i)
    run_on_thread(work.part(i));   // each part().each(...) runs in parallel
```

**A specific set of entities.** `each_of(span)` joins the view against an externally
produced list, visiting only the alive and matching ones (this is how a
[tracker](#7-reacting-to-change) drain becomes a query):

```cpp
world.view<Position, const Sprite>().each_of(someEntities, [](Position& p, const Sprite& s){ /* ... */ });
```

## 4. Systems and the frame loop

A **system** reacts to a typed **message event** (a value you dispatch). Systems are
grouped into **features** (any tag type) so you can toggle a whole group.

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

- `.after<Other>()` orders one system after another (stable topo-sort; a cycle is a
  checked violation).
- A system can handle several events (`system<A, B>`, one `process` override each) and may
  re-`dispatch` a sub-event (nested dispatch is self-contained).

**Immediate vs queued.** `dispatch(ev)` runs handlers now. To *collect* events during a
frame and run them at one point, enqueue then flush (a flush snapshots first, so events
enqueued *during* it wait for the next flush, preventing runaway loops):

```cpp
world.enqueue(damage_event{target, 5});
world.flush<damage_event>();   // or flush() for every queued type; queued<E>() counts
```

**Listeners.** When you just want a small reaction (and to capture local state), `connect`
registers any callable, including a capturing lambda, and returns a removal token:

```cpp
ecs::listener_token t = world.connect<damage_event>(
    [&log](ecs::registry& w, const damage_event& ev){ log.record(ev); });
world.disconnect(t);
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
world.apply(cmd);                       // the frame's sync point
```

It records `create`/`add`/`put`/`remove`/`destroy` plus `adopt`/`orphan`/`destroy_subtree`;
a buffered `create` returns a *provisional* handle that resolves at `apply`. Inside a
system, record into `world.deferred()` instead; the dispatcher applies it automatically
after each system, so the next one sees the changes.

---

## 6. Relationships

An optional, sparse **single-parent tree** sits on top of the flat registry and stores just topology
(no transforms, no scene graph). Use it for scene graphs, bone trees, UI panels and their
widgets, inventory contents, a squad and its members. It's one built-in `kin` component
(parent + sibling links), so edits are O(1) and free for unlinked entities. But
**traversal is pointer-chasing**, not the contiguous scan a query is.

```cpp
world.adopt(parent, child);   world.orphan(child);
ecs::entity p = world.parent_of(child);   std::size_t k = world.child_count(parent);
world.children_of(parent, [](ecs::entity c){ /* direct children, in order */ });

world.descendants_of(root, [](ecs::entity d){ /* pre-order subtree */ });
world.ancestors_of(node, [](ecs::entity a){ /* parent, grandparent, ... */ });
world.roots([](ecs::entity r){ /* every parentless entity */ });   // root_of, depth_of too
world.destroy_subtree(root);                        // destroy root + all descendants
ecs::entity copy = world.duplicate_subtree(root);   // clone, preserving internal links
```

(Plain `destroy` unlinks one entity, so its children become roots; `destroy_subtree`
cascades. `adopt`/`orphan`/`destroy_subtree` are also recordable on a command buffer.)

**Relationship walk vs query, two different questions:**

- *"every entity with components A and B"* -> a **query** (`view<A,B>`): the contiguous
  fast path. Entities that share components need no grouping, since **the query is the group.**
- *"the children/subtree of this specific entity"* -> a **relationship** walk: follows the
  `kin` links, cost is the nodes you visit. Great for a subtree of dozens/hundreds, not
  for sweeping the world.

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

```cpp
// Hooks: a plain function pointer + void* user, or a member function via scoped_hook (RAII):
ecs::scoped_hook up(world, world.on_add<SpriteRef, &GpuSprites::upload>(this));
// also on_remove / on_replace; world.on_add<T>(&fn, user) returns a hook_token you unhook().

// tracker<T>: a deduped drain you read once a frame. Only replace/put/amend count as a
// "replace" (a bare get<T> write is invisible by design). The drain feeds each_of:
ecs::tracker<Health> hits(world, ecs::track::replaced);   // or added / removed / all
world.view<const Health, Sprite>().each_of(hits.replaced(),
    [](const Health& h, Sprite& s){ s.flash = (h.hp < 25); });
hits.clear();

// watcher: a multi-component CONDITION; collects entities that began matching. matched()
// is live truth: an entity that stops matching is evicted automatically.
ecs::watcher<ecs::types<Burning, Health>, ecs::except<Shielded>> on_fire(world);
for (ecs::entity e : on_fire.matched()) { /* just became burning + unshielded */ }
on_fire.clear();
```

(`changed<C>` in a watcher also collects entities whose `C` was replaced while matching.
Both `tracker` and `watcher` point into the registry, so destroy them before the world.)

## 8. Globals

World-scoped state (camera, clock, input) lives on the `globals()` entity, a lazily
created singleton, so you don't invent a resource system. All of section 2's verbs work
on it, and because it's a real entity it rides along in `pack` and `live_count`.

```cpp
struct MatchClock { float elapsed = 0; };
world.globals().obtain<MatchClock>();              // create on first use
world.globals().get<MatchClock>().elapsed += dt;
```

## 9. Save, load and merge

Serialization is *typed*: `ecs` owns ordering and identity; you own the byte encoding
through a tiny writer/reader callable (`void operator()(const auto&)` /
`void operator()(auto&)`). Two ways to load, differing in **which ids the entities get**:

```cpp
// pack/unpack: the EXACT same ids into an EMPTY world, for saves, rollback, snapshots
// where handles stored elsewhere must stay valid.
ecs::pack<Position, Velocity, Health>(world, out);
auto r = ecs::unpack<Position, Velocity, Health>(restored, in, /*max_entities=*/100000);
if (!r) { /* r.error().code: archive_mismatch or archive_too_large */ }

// graft: FRESH ids into a POPULATED world (nothing collides), to load a level chunk, instance
// a sub-scene. Returns an old->new map.
auto map = ecs::graft<Position, Velocity, Health>(world, in);
ecs::entity now = map->resolve(oldHandle);
```

Three guards make loads robust:

- **Schema-drift detection**: a consteval per-component fingerprint (size, field count,
  names); a changed field shape fails with `archive_mismatch` instead of misreading bytes.
- **`max_entities` cap**: bounds an untrusted stream *before* any allocation.
- **Explicit relinking**: since `graft` renumbers, a component storing an `ecs::entity`
  opts in: `void ecs_relink(const ecs::graft_map& m) { foe = m.resolve(foe); }`.

Parent/child links serialize separately, *after* the component pass:
`pack_links` / `unpack_links` / `graft_links`.

## 10. Blueprints

A recorded set of component values to stamp out repeatedly:

```cpp
ecs::blueprint goblin(Health{30}, Velocity{-1, 0});
ecs::entity one = world.create(goblin);
world.create(goblin, 100, [](ecs::entity e){ /* tweak each instance */ });   // a hundred
```

## 11. Storage policies

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

## 12. Reflection and runtime tooling

Two kinds, for two problems:

- **Registered**: a process-wide, RTTI-free registry keyed by type name, so editors,
  consoles, and savers work with components *by string* at runtime. Opt in once with
  `reflect<T>()`.
- **Aggregate**: a structural, zero-registration compile-time walk over a flat struct's
  members. Use it when *you* write generic C++ and never need the string name.

```cpp
// Registered:
ecs::reflect<Position>().fields("x", "y");          // names (positional .fields() -> "0","1")
ecs::reflect<Transform>().field<&Transform::pos>("pos").method<&Transform::reset>("reset");

ecs::reflection r = ecs::reflection_of<Position>();  // or reflection_of("Position") / (hash)
ecs::any pos = ecs::any::make<Position>(Position{1, 2});   // make<T> owns; ref(obj) aliases
ecs::set(pos, "x", ecs::any::make<float>(9.0F));     // get/set a field, invoke a method, by name
r.each_field([](const ecs::field& f){ /* enumerate */ });
ecs::for_each([](const ecs::reflection& t){ /* every registered type */ });

// Inspect a live world with no compile-time type:
world.each_pool([](const ecs::pool_info& p){ /* name, size... */ });
world.components_of(e, [](const ecs::pool_info& p){ /* what this entity has */ });

// Aggregate (no registration):
ecs::for_each(pos, [](auto& m){ /* each member by reference */ });
std::size_t h = ecs::hash_fields(pos);   bool eq = ecs::fields_equal(a, b);
ecs::write_fields(out, pos);  ecs::read_fields(in, pos);   // layout-portable codec
```

`ecs::string_id("camera")` hashes a string into the same id space as `hash_of<T>()`, so
your own string keys share one namespace.

## 13. Compile-time toolkit

`ecs::types<...>` is a type-list manifest you can pass and manipulate at compile time:

```cpp
using Saved = ecs::types<Position, Velocity, Health>;
ecs::pack(world, out, Saved{});                          // manifest form of pack<...>
ecs::for_each(Saved{}, []<class T>(){ /* once per type */ });
```

Set algebra (`joined_t`, `intersection_t`, `difference_t`, `filter_t`, ...) and predicate
folds (`all_of_v`, `any_of_v`, `count_if_v`) build larger manifests from smaller ones.

## 14. Custom entity traits

The 8-byte default handle (31-bit index, 32-bit generation) suits most games; for a
smaller handle, template the registry on your own traits:

```cpp
struct small_traits {
    using index_type = std::uint16_t;  using generation_type = std::uint16_t;
    static constexpr std::uint32_t index_bits = 15;        // 1 bit reserved
};
ecs::basic_registry<small_traits> small_world;            // ~4-byte handles, up to 32k slots
```

Every public type has a `basic_*<Traits>` form; `registry`/`view`/... are aliases for the
default. Entities are hashable: `std::unordered_map<ecs::entity, int, ecs::entity_hash>`.

## 15. Diagnostics and safety

One knob, **`ECS_CHECKS`** (on unless `NDEBUG`): stale-handle detection, iteration locks,
and consistency asserts, routed through a replaceable handler.

During a query you may write component *values* and call bare `create()` or a command
buffer; what's **refused** (reported, then no-op) is a structural change to the pool being
iterated (remove, tag add, destroy, sort, hook connect/disconnect), `apply`/`reset` while
iterating, and `connect`/`disconnect`/`add_system` while dispatching.

```cpp
if (auto v = world.validate(); !v) std::printf("broken: %s\n", v.error().note);  // expected<void,fault>
ecs::memory_footprint mem = world.footprint();
ecs::set_violation_handler([](const char* msg){ log_error(msg); });   // log instead of abort
```

`ecs` throws nothing of its own (only cold grow/reserve may propagate `std::bad_alloc`);
hot lookups and iteration never allocate.

---

## Performance

Shaped so the per-frame query stays well inside a frame budget:

- **O(1) random access**: sparse sets; `get`/`has`/`find` are index math, no searching.
- **Contiguous iteration**: dense packed arrays; no pointer chasing, no virtual call in
  the loop.
- **Smallest-pool join**: `view<A,B>` walks the smaller pool and probes the rest in O(1).
- **No allocation** building or iterating a view.

Order-of-magnitude numbers, so *measure your own game* (100k entities, release,
`ECS_CHECKS` off, best-of-12):

| Operation | Cost | In a 1 ms frame |
|---|---|---|
| iterate `view<A>` | ~1.5 ns/entity | ~650k entities |
| iterate `view<A, B>` | ~2.8 ns/entity | ~360k entities |
| `get<T>` / `has_all` | ~3-4 ns | thousands of lookups, free |
| `add` / `remove` | ~9 / ~5 ns | tens of thousands of edits |
| raw member read (baseline) | ~0.36 ns | the view loop is ~4x that |

So a typical update over tens of thousands of entities is tens of *micro*seconds. To keep
it there: mark read-only components `const`; never make structural changes mid-loop (use a
[command buffer](#5-deferred-changes)); iterate with queries, not [trees](#6-relationships);
use `split(n)` for threaded passes and `sort<T>` once for ordered ones; and ship with
`NDEBUG` so the checks compile out.

---

## Cheat sheet

| Task | Call |
|---|---|
| Create | `create(A{}, B{})` / `create()` / `.component<A>(...)` / `create_n(n, out\|fn)` |
| Add / remove | `add<T>(e,...)` / `remove<T>(e)` / `insert<T>(first,last[,val])` |
| Read | `get` (abort) / `find` (nullptr) / `has` / `has_all` / `has_any` / `find_all` |
| Change | `put` (upsert) / `obtain` (get-or-add) / `replace` (must exist) / `amend<T>(e,fn)` (observed) |
| Destroy | `destroy(e)` / `destroy(first,last)` / `destroy_subtree(root)` |
| Query | `each<A, const B>(fn[, filter])` / `view<...>()` |
| Roles | `T` (ref) / `const T` (read-only) / `exists<T>{}` (filter) / `maybe<T>` (pointer) |
| View ops | `each` / `entities` / `count` / `first` / `back` / `single` / `reversed` / `each_of(span)` / `collect` |
| Order / parallel | `sort<T>(cmp)` / `driven_by<T>()` / `split(n)` |
| Systems | `feature<F>().add_system<S>().after<O>()` ; `dispatch(ev)` |
| Queue / listen | `enqueue(ev)` / `flush<E>()` / `queued<E>()` ; `connect<E>(fn) -> token` / `disconnect` |
| Defer | `command_buffer` (you apply) / `world.deferred()` (auto) ; `apply(cmd)` |
| Relationships | `adopt` / `children_of` / `descendants_of` / `reorder_child` / `destroy_subtree` |
| React | hooks + `scoped_hook` ; `tracker<T>` ; `watcher<types<...>, except<...>, changed<C>>` |
| Globals | `world.globals().obtain<T>()` / `.get<T>()` |
| Save / load | `pack`/`unpack` (exact ids) / `graft` (fresh ids) (+ `pack_links`, `max_entities`, `ecs_relink`) |
| Prefab | `ecs::blueprint bp(A{}, B{})` ; `create(bp[, count])` |
| Storage | `static constexpr ecs::storage ecs_storage` ; `pool_of<T>` |
| Reflect | `reflect<T>().fields(...)` ; `reflection_of(...)` ; `get/set/invoke(any, name)` |
| Inspect | `each_pool` ; `components_of` ; `validate()` ; `footprint()` |

## Building

CMake presets with the vcpkg toolchain (`VCPKG_ROOT` set; the manifest pulls only `gtest`).

```bat
build.bat   :: format + configure + clang-tidy + build
test.bat    :: build and run the Google Test binary
```

Tests live in `tests/`. To use `ecs` elsewhere, copy `src/ecs.hpp`; that one file is the entire
integration. The one knob is `ECS_CHECKS`.

## License

MIT. See [LICENSE](LICENSE.md).
