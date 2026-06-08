#pragma once
// ============================================================================
// quiver.hpp — a single-header Entity Component System for real-time games.
//
//   C++23 - no dependencies - no RTTI - no exceptions of its own - MIT.
//
// The thirty-second tour:
//
//   quiver::world w;
//   quiver::entity e = w.spawn(Transform{...}, Velocity{...});
//
//   w.each<Transform, Velocity>([](quiver::entity e, Transform& t, Velocity& v)
//   {
//       t.position += v.value;
//   });
//
//   auto movers = w.select<Transform, Velocity>(quiver::except<Frozen>{});  // reusable
//   movers.each([](Transform& t, Velocity& v) { ... });                     // zero setup
//
//   quiver::command_buffer cmd;
//   w.each<Health>([&](quiver::entity e, Health& h) { if (h.value <= 0) cmd.kill(e); });
//   w.apply(cmd);
//
// And the rest of the toolbox:
//
//   w.bond<Transform, Velocity>();                           // mirrored pair: two
//   w.bonded<Transform, Velocity>().each(...);               // pools, zero probes
//   for (auto [e, t, v] : movers.range()) { ... }            // range-for + bindings
//   w.select<Transform, quiver::maybe<Tint>>()               // optional components
//   auto work = movers.split(4);  work.part(i).each(...);    // parallel chunks
//   w.sort<Sprite>(by_atlas);  w.sort_along<Sprite, Transform>();   // (re)ordering
//   w.on_add<Collider, &Physics::body>(&physics);            // hooks: fn/member/RAII
//   quiver::scoped_hook guard(w, token);  quiver::tracker<Health> hurt(w);
//   quiver::pack<Transform, Health>(w, writer);              // save/load/graft
//   w.globals().obtain<MatchClock>();                        // singletons, ECS-style
//   quiver::pipeline frame; frame.stage("move", fn);         // stages + deferred()
//   quiver::blueprint goblin; ... w.spawn(goblin);           // reusable spawn recipes
//   w.duplicate(e);  w.ref(e).add<Burning>();  w.obtain<Timer>(e);  // conveniences
//   quiver::runtime_selection q; q.include(w.find_pool_by_hash(h)); // editor queries
//   w.find_pool<T>().raw(e)                                  // type-erased bytes
//   template <> struct quiver::pool_of<T> {...};             // custom storage seam
//   static constexpr std::string_view quiver_label = "...";  // portable identity
//   using Saved = quiver::types<Transform, Health>;          // component manifests:
//   w.select(Saved{});  quiver::pack(w, out, Saved{});       // one list, everywhere
//   quiver::world w2{&my_arena_resource};                    // std::pmr, no templates
//
// Mutation rules during iteration (each / entities / range / split / bonded
// views / children_of):
//   ALLOWED:  writing component values; bare spawn(); recording into a
//             command_buffer; structural changes to pools that are not part
//             of any running iteration.
//   REFUSED (checked builds report a violation, then nothing happens):
//             remove on an iterated pool, tag adds to an iterated pool,
//             kill()/duplicate() of an entity belonging to an iterated pool,
//             purge/sort of an iterated pool, hook connect/disconnect on it,
//             and reset()/shrink()/apply()/world-teardown while anything
//             iterates — all checked before any state changes. (Full list:
//             DESIGN.md §10.)
//   REPORTED, THEN PROCEEDS: value add/put on an iterated pool (a reference
//             must be returned). The loop itself stays safe, but component
//             references the callback already holds may dangle — use the
//             command_buffer.
//
// Thread model: a world and the buffers feeding it are externally
// synchronized (single-threaded by contract). Component type ids and the
// violation handler are process-global and thread-safe.
//
// Exception policy: quiver itself throws nothing. Cold grow/reserve paths may
// propagate std::bad_alloc from the standard allocator. Hot lookups and
// iteration do not allocate.
//
// Configuration: QUIVER_CHECKS is the single toggle (default: on when NDEBUG
// is not defined). It enables stale-handle detection, iteration locks, and
// internal consistency asserts, all routed through a replaceable violation
// handler (see set_violation_handler). There are no API-usage macros.
// ============================================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef QUIVER_CHECKS
#ifdef NDEBUG
#define QUIVER_CHECKS 0
#else
#define QUIVER_CHECKS 1
#endif
#endif

namespace quiver
{

inline constexpr bool checks_enabled = (QUIVER_CHECKS != 0);

class world;
class command_buffer;
template <class W>
class basic_entity_ref;
using entity_ref = basic_entity_ref<world>;
using const_entity_ref = basic_entity_ref<const world>;
class runtime_selection;
template <class Excludes, class... Ts>
class bonded_view_t;
template <class Excludes, class... Ts>
class selection_t;
struct test_access;  // checked-build test backdoor; defined only when QUIVER_CHECKS is on

namespace detail
{
struct kin;  // internal parent/child link component; managed via adopt/orphan/kill only
}

// ----------------------------------------------------------------------------
// Violation handler
//
// Every checked-build safety violation (stale handle, structural change during
// iteration, ...) calls the installed handler before quiver decides what to do
// next. The default handler prints and aborts. A custom handler may return, in
// which case the offending operation is skipped where that is safe and
// proceeds (documented per call site) where a result must be produced.
// ----------------------------------------------------------------------------

using violation_handler = void (*)(const char* message);

namespace detail
{
inline void default_violation(const char* message)
{
    std::fputs("quiver violation: ", stderr);
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

inline std::atomic<violation_handler>& violation_slot() noexcept
{
    static std::atomic<violation_handler> slot{&default_violation};
    return slot;
}

inline void violate(const char* message)
{
    violation_slot().load(std::memory_order_relaxed)(message);
}

// Composes "<what> '<name>'" for messages that mention a component pool.
// Cold path, checked builds only; the buffer is thread-local scratch.
inline void violate_pool(const char* what, std::string_view pool_name)
{
    thread_local char buffer[192];
    const int n = std::snprintf(buffer,
                                sizeof(buffer),
                                "%s '%.*s'",
                                what,
                                static_cast<int>(pool_name.size()),
                                pool_name.data());
    violate(n > 0 ? buffer : what);
}
}  // namespace detail

// Installs a new process-wide violation handler and returns the previous one.
// Passing nullptr restores the default (print + abort).
inline violation_handler set_violation_handler(violation_handler handler) noexcept
{
    if (handler == nullptr)
    {
        handler = &detail::default_violation;
    }
    return detail::violation_slot().exchange(handler, std::memory_order_relaxed);
}

// ----------------------------------------------------------------------------
// Compile-time toolkit
//
// The vocabulary quiver's own metaprogramming runs on, published because the
// same shapes are useful around any ECS: bundles of component types travel
// between systems, archives, and queries as first-class values.
//
//   using Saved = quiver::types<Transform, Health, Burning>;
//   quiver::pack(world, writer, Saved{});            // archive manifests
//   auto sel = world.select(Saved{});                 // queries from lists
//   quiver::for_each_type(Saved{}, []<class T>() { register_editor<T>(); });
//
// Type-returning operations are traits (with _t aliases); value-returning
// ones are consteval functions or variable templates, matching name_of /
// hash_of. Everything is O(list) instantiations and self-contained.
// ----------------------------------------------------------------------------

// One compile-time constant carried as a type.
template <auto Value>
using constant = std::integral_constant<decltype(Value), Value>;

// A bundle of types, nothing more. The size is a member so lists carry their
// own arity; the self alias lets metafunctions return lists uniformly.
template <class... Ts>
struct types
{
    using self = types;
    static constexpr std::size_t size = sizeof...(Ts);
};

// The type at position I (compile error when out of range).
template <std::size_t I, class List>
struct type_at;

template <std::size_t I, class T, class... Rest>
struct type_at<I, types<T, Rest...>> : type_at<I - 1, types<Rest...>>
{
};

template <class T, class... Rest>
struct type_at<0, types<T, Rest...>>
{
    using type = T;
};

template <std::size_t I, class List>
using type_at_t = type_at<I, List>::type;

// Membership (usable directly in requires-clauses).
template <class T, class List>
inline constexpr bool contains_type = false;

template <class T, class... Ts>
inline constexpr bool contains_type<T, types<Ts...>> = (std::same_as<T, Ts> || ...);

// Position of T's first occurrence; ill-formed (with this message) if absent.
template <class T, class... Ts>
[[nodiscard]] consteval std::size_t index_of(types<Ts...>) noexcept
{
    static_assert((std::same_as<T, Ts> || ...), "quiver: T is not in this types<> list");
    constexpr bool hits[]{std::same_as<T, Ts>...};
    for (std::size_t i = 0; i < sizeof...(Ts); ++i)
    {
        if (hits[i])
        {
            return i;
        }
    }
    return sizeof...(Ts);  // unreachable: the static_assert above fired
}

// Concatenation of any number of lists.
template <class... Lists>
struct joined
{
    using type = types<>;
};

template <class... Ts>
struct joined<types<Ts...>>
{
    using type = types<Ts...>;
};

template <class... Ts, class... Us, class... Rest>
struct joined<types<Ts...>, types<Us...>, Rest...> : joined<types<Ts..., Us...>, Rest...>
{
};

template <class... Lists>
using joined_t = joined<Lists...>::type;

// The list without any occurrence of the given types.
template <class List, class... Drop>
struct without;

template <class... Ts, class... Drop>
struct without<types<Ts...>, Drop...>
{
    template <class T>
    using keep_one = std::conditional_t<(std::same_as<T, Drop> || ...), types<>, types<T>>;
    using type = joined_t<types<>, keep_one<Ts>...>;
};

template <class List, class... Drop>
using without_t = without<List, Drop...>::type;

// The list with duplicates removed (first occurrence wins).
template <class List>
struct distinct;

template <>
struct distinct<types<>>
{
    using type = types<>;
};

template <class T, class... Rest>
struct distinct<types<T, Rest...>>
{
    using type = joined_t<types<T>, typename distinct<without_t<types<Rest...>, T>>::type>;
};

template <class List>
using distinct_t = distinct<List>::type;

// Element-wise application of a standard metafunction (Op<T>::type).
template <template <class> class Op, class List>
struct mapped;

template <template <class> class Op, class... Ts>
struct mapped<Op, types<Ts...>>
{
    // Generic metafunction application; there is no _t shortcut for an
    // arbitrary Op. NOLINTNEXTLINE(modernize-type-traits)
    using type = types<typename Op<Ts>::type...>;
};

template <template <class> class Op, class List>
using mapped_t = mapped<Op, List>::type;

// Invokes fn's templated call operator once per type, in order:
//   for_each_type(list, []<class T>() { ... });
template <class... Ts, class F>
constexpr void for_each_type(types<Ts...>, F&& fn)
{
    (fn.template operator()<Ts>(), ...);
}

// The value-side mirror, for bundles of compile-time constants.
template <auto... Vs>
struct values
{
    using self = values;
    static constexpr std::size_t size = sizeof...(Vs);
};

// The value at position I, with its exact type preserved.
template <std::size_t I, auto First, auto... Rest>
[[nodiscard]] consteval auto value_at(values<First, Rest...>) noexcept
{
    static_assert(I <= sizeof...(Rest), "quiver: values<> index out of range");
    if constexpr (I == 0)
    {
        return First;
    }
    else
    {
        return value_at<I - 1>(values<Rest...>{});
    }
}

template <auto V, class List>
inline constexpr bool contains_value = false;

template <auto V, auto... Vs>
inline constexpr bool contains_value<V, values<Vs...>> = ((V == Vs) || ...);

template <auto V, auto... Vs>
[[nodiscard]] consteval std::size_t index_of_value(values<Vs...>) noexcept
{
    static_assert(((V == Vs) || ...), "quiver: V is not in this values<> list");
    constexpr bool hits[]{(V == Vs)...};
    for (std::size_t i = 0; i < sizeof...(Vs); ++i)
    {
        if (hits[i])
        {
            return i;
        }
    }
    return sizeof...(Vs);  // unreachable: the static_assert above fired
}

// ----------------------------------------------------------------------------
// Entity handle
//
// 8 bytes: a slot index plus a generation that increments every time the slot
// is recycled, so handles to dead entities are detectable. Two plain fields,
// readable in any debugger. A default-constructed entity equals no_entity.
// ----------------------------------------------------------------------------

namespace detail
{
inline constexpr std::uint32_t npos32 = 0xFFFFFFFFu;
// The default 32+32 handle layout has four mechanisms welded to it; under
// entity TRAITS those welds become the documented per-traits contract:
//   1. one index bit above index_bits marks provisional handles issued by
//      command_buffer::spawn() (the traits concept RESERVES that spare bit),
//      so real slot indices stay below 2^index_bits;
//   2. provisional handles carry their buffer's nonce in the generation
//      field (world::apply refuses foreign/pre-clear provisionals through
//      it; narrower generation types narrow the nonce, which is misuse
//      detection, not security);
//   3. entity_table::max_slots is therefore 2^index_bits - 1;
//   4. bits() packs index and generation into one stable 64-bit encoding
//      for hashing and serialization (the traits concept caps the total at
//      64 bits).
inline constexpr std::uint32_t provisional_bit = 0x80000000u;
}  // namespace detail

// The handle layout contract. Widths choose field TYPES, not shift math —
// a basic_entity stays two plain fields, readable in any debugger, at every
// width. The defaults are quiver's classic 31-bit slots + 32-bit
// generations in 8 bytes.
struct default_entity_traits
{
    using index_type = std::uint32_t;
    using generation_type = std::uint32_t;
    static constexpr std::uint32_t index_bits = 31;
};

template <class Traits>
concept entity_traits =
    std::unsigned_integral<typename Traits::index_type> &&
    std::unsigned_integral<typename Traits::generation_type> && (Traits::index_bits >= 1) &&
    // One spare index bit is RESERVED for the provisional flag (weld #1).
    (Traits::index_bits <
     static_cast<std::uint32_t>(std::numeric_limits<typename Traits::index_type>::digits)) &&
    // bits() must pack losslessly into 64 (weld #4).
    (Traits::index_bits + static_cast<std::uint32_t>(
                              std::numeric_limits<typename Traits::generation_type>::digits) <=
     64);

namespace detail
{
// The per-traits constants the welds generalize to. For the default traits
// these reproduce npos32 / provisional_bit / 2^31-1 exactly.
template <entity_traits Traits>
struct entity_limits
{
    using index_type = typename Traits::index_type;
    static constexpr index_type npos = std::numeric_limits<index_type>::max();
    static constexpr index_type provisional_bit = index_type{1} << Traits::index_bits;
    static constexpr index_type max_slots = provisional_bit - 1;
};
}  // namespace detail

template <entity_traits Traits>
class basic_entity
{
public:
    using traits_type = Traits;
    using index_type = typename Traits::index_type;
    using generation_type = typename Traits::generation_type;

    static constexpr std::uint32_t index_bits = Traits::index_bits;  // above: provisional flag
    static constexpr std::uint32_t generation_bits = static_cast<std::uint32_t>(
        std::numeric_limits<generation_type>::digits);  // carries the buffer nonce in provisionals

    constexpr basic_entity() noexcept = default;  // == the null handle

    constexpr basic_entity(index_type index, generation_type generation) noexcept
        : index_(index),
          generation_(generation)
    {
    }

    [[nodiscard]] constexpr index_type index() const noexcept { return index_; }
    [[nodiscard]] constexpr generation_type generation() const noexcept { return generation_; }

    // Stable 64-bit encoding, handy for hashing and serialization.
    [[nodiscard]] constexpr std::uint64_t bits() const noexcept
    {
        return (static_cast<std::uint64_t>(index_) << generation_bits) |
               static_cast<std::uint64_t>(generation_);
    }

    // True for any handle other than the null handle (says nothing about
    // liveness; ask world::alive for that).
    constexpr explicit operator bool() const noexcept
    {
        return index_ != detail::entity_limits<Traits>::npos;
    }

    friend constexpr bool operator==(basic_entity, basic_entity) noexcept = default;
    friend constexpr auto operator<=>(basic_entity, basic_entity) noexcept = default;

private:
    index_type index_ = detail::entity_limits<Traits>::npos;
    generation_type generation_ = 0;
};

// The classic handle: 8 bytes, 31-bit slots, 32-bit generations. Everything
// in this header that is not yet basic_-templated runs on this alias.
using entity = basic_entity<default_entity_traits>;

inline constexpr entity no_entity{};

// The alias-identity pins: the traits machinery must reproduce the historic
// layout and encoding bit for bit.
static_assert(sizeof(entity) == 8);
static_assert(entity::index_bits == 31 && entity::generation_bits == 32);
static_assert(entity{5, 7}.bits() == ((std::uint64_t{5} << 32) | 7));
static_assert(!entity{} && entity{0, 0}.index() == 0);
static_assert(detail::entity_limits<default_entity_traits>::npos == detail::npos32);
static_assert(detail::entity_limits<default_entity_traits>::provisional_bit ==
              detail::provisional_bit);

namespace detail
{
template <entity_traits Traits>
[[nodiscard]] constexpr bool is_provisional(basic_entity<Traits> e) noexcept
{
    return (e.index() & entity_limits<Traits>::provisional_bit) != 0 &&
           e.index() != entity_limits<Traits>::npos;
}
}  // namespace detail

// ----------------------------------------------------------------------------
// Storage policies
//
//   packed  - parallel dense arrays, swap-remove. Fastest iteration; pointers
//             and references are invalidated by add/remove on the same pool.
//             The default.
//   stable  - components live in fixed chunks that never move; pointers stay
//             valid until that component is removed (or the pool is purged /
//             the world reset). One extra indirection on access.
//   tag     - membership only, no per-entity data. Automatic for empty types.
//
// Selection, in precedence order:
//   1. explicit specialization of quiver::storage_policy<T>
//   2. a `static constexpr auto quiver_storage = quiver::storage::...;` member
//   3. tag, if T is an empty type
//   4. packed
// The policy must be identical in every translation unit and must not change
// after a world has first used T.
// ----------------------------------------------------------------------------

enum class storage : std::uint8_t
{
    packed,
    stable,
    tag,
};

namespace detail
{
template <class T>
concept has_storage_member = requires { T::quiver_storage; };

template <class T>
consteval storage default_storage()
{
    if constexpr (has_storage_member<T>)
    {
        static_assert(std::convertible_to<decltype(T::quiver_storage), storage>,
                      "quiver: T::quiver_storage must be a quiver::storage value");
        return T::quiver_storage;
    }
    else if constexpr (std::is_empty_v<T>)
    {
        return storage::tag;
    }
    else
    {
        return storage::packed;
    }
}
}  // namespace detail

// Specialize for types you do not own:
//   template <> inline constexpr quiver::storage quiver::storage_policy<Name>
//       = quiver::storage::stable;
template <class T>
inline constexpr storage storage_policy = detail::default_storage<T>();

namespace detail
{
template <class T>
concept has_chunk_member = requires {
    { T::quiver_chunk_items } -> std::convertible_to<std::size_t>;
};

template <class T>
consteval std::size_t default_chunk_items()
{
    if constexpr (has_chunk_member<T>)
    {
        return T::quiver_chunk_items;
    }
    else
    {
        return std::clamp<std::size_t>(4096 / sizeof(T), 4, 1024);
    }
}
}  // namespace detail

// Stable-storage tuning: components per chunk. Defaults to ~4 KiB chunks;
// override for pools you know are huge (fewer allocations) or near-singleton
// (less slack), in the same shape as storage selection — a trait
// specialization or a `static constexpr std::size_t quiver_chunk_items`
// member. Must be identical in every TU, like storage_policy.
template <class T>
inline constexpr std::size_t chunk_capacity = detail::default_chunk_items<T>();

// ----------------------------------------------------------------------------
// Component concept
//
// Deliberately does NOT require movability: stable storage constructs and
// destroys components in place, so pinned types (atomics, mutexes,
// self-referential objects) are legal there. The operations that genuinely
// move components re-impose the requirement with named static_asserts:
// packed storage (swap-remove, dense reallocation) and command_buffer
// payloads (moved into the world at apply()).
// ----------------------------------------------------------------------------

template <class T>
concept component = std::is_object_v<T> && !std::is_array_v<T> && !std::is_const_v<T> &&
                    !std::is_volatile_v<T> && std::is_destructible_v<T>;

namespace detail
{
template <class T>
using bare = std::remove_const_t<T>;

template <class T>
inline constexpr bool is_tag_v = storage_policy<bare<T>> == storage::tag;
}  // namespace detail

// ----------------------------------------------------------------------------
// Compile-time component names (for the inspector and diagnostics; no RTTI)
// ----------------------------------------------------------------------------

namespace detail
{
template <class T>
constexpr std::string_view raw_type_string() noexcept
{
#if defined(_MSC_VER) && !defined(__clang__)
    return {__FUNCSIG__};
#else
    return {__PRETTY_FUNCTION__};
#endif
}

constexpr std::string_view strip_type_keyword(std::string_view name) noexcept
{
    for (std::string_view keyword : {"struct ", "class ", "enum ", "union "})
    {
        if (name.starts_with(keyword))
        {
            name.remove_prefix(keyword.size());
            break;
        }
    }
    return name;
}

constexpr std::string_view extract_type_name(std::string_view raw) noexcept
{
    // gcc:   "... raw_type_string() [with T = Foo; ...]"
    // clang: "... raw_type_string() [T = Foo]"
    // msvc:  "... raw_type_string<struct Foo>(void) ..."
    if (std::size_t begin = raw.find("[with T = "); begin != std::string_view::npos)
    {
        begin += 10;
        return raw.substr(begin, raw.find_first_of(";]", begin) - begin);
    }
    if (std::size_t begin = raw.find("[T = "); begin != std::string_view::npos)
    {
        begin += 5;
        return raw.substr(begin, raw.find_first_of(";]", begin) - begin);
    }
    if (std::size_t begin = raw.find("raw_type_string<"); begin != std::string_view::npos)
    {
        begin += 16;
        return strip_type_keyword(raw.substr(begin, raw.rfind('>') - begin));
    }
    return "unknown-type";
}
}  // namespace detail

// User-pinned component identity. The compiler-derived spelling of T is
// toolchain-specific; pinning a label makes name_of<T>() — and therefore
// hash_of<T>(), the key archives persist — portable across compilers and
// rename refactors. Two equivalent forms, the member preferred for types you
// own:
//
//   struct Transform { ...
//       static constexpr std::string_view quiver_label = "game.Transform"; };
//
//   template <> inline constexpr std::string_view
//   quiver::component_label<foreign::Vec3> = "foreign.Vec3";
//
// Namespace your labels ("game.Transform", not "Transform") so they can
// never collide with another type's compiler-derived spelling — collisions
// are only caught when both pools register in one checked world. Labels are
// INHERITED: a strong typedef (`struct RagdollTransform : Transform`) must
// re-pin its own quiver_label or it shares the base's archive key. Must be
// unique per world (the registration-time hash-collision check covers them
// like any other name) and non-empty.
template <class T>
inline constexpr std::string_view component_label{};  // empty: compiler-derived name

namespace detail
{
template <class T>
concept has_label_member = requires {
    { T::quiver_label } -> std::convertible_to<std::string_view>;
};
}  // namespace detail

// Human-readable, persistable name of T, e.g. "game::Transform". Computed at
// compile time; overridable (see component_label above).
template <class T>
[[nodiscard]] constexpr std::string_view name_of() noexcept
{
    if constexpr (detail::has_label_member<T>)
    {
        static_assert(
            requires { typename std::integral_constant<std::size_t, T::quiver_label.size()>; },
            "quiver: quiver_label must be a constexpr std::string_view");
        static_assert(!std::string_view{T::quiver_label}.empty(),
                      "quiver: quiver_label must not be empty (omit it for the "
                      "compiler-derived name)");
        return T::quiver_label;
    }
    else if constexpr (!component_label<T>.empty())
    {
        return component_label<T>;
    }
    else
    {
        constexpr std::string_view name = detail::extract_type_name(detail::raw_type_string<T>());
        return name;
    }
}

namespace detail
{
constexpr std::uint64_t fnv1a(std::string_view text) noexcept
{
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const char c : text)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ull;
    }
    return hash;
}
}  // namespace detail

// Stable, RTTI-free identity for T: a 64-bit hash of name_of<T>(). Unlike the
// internal dense type ids (first-use-order, never persist them), this is
// stable across runs and build configurations of the same toolchain — use it
// as the component key in snapshots and tools. Caveat: without a pinned
// label it follows the compiler's spelling of the name, so anonymous-
// namespace types differ per TU and different compilers may disagree; pin a
// quiver_label / component_label (namespaced, so it cannot alias another
// type's spelling) for keys that cross toolchains. Checked builds report a
// collision if two registered components ever hash alike — within one world.
template <class T>
[[nodiscard]] constexpr std::uint64_t hash_of() noexcept
{
    constexpr std::uint64_t hash = detail::fnv1a(name_of<T>());
    return hash;
}

// ----------------------------------------------------------------------------
// Component type ids
//
// Dense process-wide ids handed out on first use (thread-safe). Ids depend on
// first-use order, so they are not stable across runs or modules; quiver never
// persists them. Keep one world inside one module (DLL boundaries may disagree
// about ids).
// ----------------------------------------------------------------------------

namespace detail
{
inline std::uint32_t next_type_id() noexcept
{
    static std::atomic<std::uint32_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

template <class T>
inline std::uint32_t type_id() noexcept
{
    static const std::uint32_t id = next_type_id();
    return id;
}

// Process-unique stamp for command buffers; carried in the generation field of
// provisional handles so apply() can tell a foreign (or pre-clear) provisional
// handle from one of its own.
inline std::uint32_t next_buffer_nonce() noexcept
{
    static std::atomic<std::uint32_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace detail

// ----------------------------------------------------------------------------
// Faults (cold-path error reporting for validate() and restore_entity())
// ----------------------------------------------------------------------------

enum class fault_code : std::uint8_t
{
    bad_handle,           // restore_entity: null or provisional handle
    slot_occupied,        // restore_entity: target slot already holds a live entity
    table_out_of_sync,    // entity table bookkeeping broken
    sparse_dense_desync,  // a pool's index and its dense array disagree
    dense_entity_dead,    // a pool holds an entity the table says is dead
    slot_map_broken,      // a stable pool's slot bookkeeping broken
    links_broken,         // parent/child links inconsistent
    bond_broken,          // a bonded pair's mirrored partition inconsistent
    archive_mismatch,     // unpack/graft: the stream disagrees with the types
    world_not_empty,      // unpack requires a fresh world
    globals_broken,       // more than one globals entity
};

struct fault
{
    fault_code code;
    std::string_view pool;  // offending pool name, if any
    const char* note = "";
};

// One row of the pool inspector (see world::each_pool).
struct pool_info
{
    std::string_view name;
    std::uint32_t id;               // dense process-local type id; never persist
    std::uint64_t name_hash;        // hash_of<T>(): stable across runs, persistable
    std::size_t size;               // live components
    std::size_t capacity;           // allocated component slots
    std::size_t bytes_per_item;     // 0 for tag pools
    std::size_t index_bytes;        // the paged sparse index (16 KiB pages add up)
    std::size_t bookkeeping_bytes;  // dense entity array, slot maps, free lists
    storage kind;
};

// Result of world::apply.
struct apply_result
{
    std::uint32_t applied = 0;
    std::uint32_t skipped = 0;  // ops whose target was dead at apply time
};

// Result of world::duplicate.
struct duplicate_result
{
    entity clone;               // no_entity when the source was dead
    std::uint32_t copied = 0;   // components copy-constructed onto the clone
    std::uint32_t skipped = 0;  // components whose type is not copy-constructible
};

// Result of world::footprint — truthful budget numbers for a stats HUD.
struct memory_footprint
{
    std::size_t entity_table_bytes = 0;
    std::size_t component_bytes = 0;    // payload capacity across all pools
    std::size_t index_bytes = 0;        // paged sparse indices
    std::size_t bookkeeping_bytes = 0;  // dense arrays, slot maps, free lists

    [[nodiscard]] std::size_t total() const noexcept
    {
        return entity_table_bytes + component_bytes + index_bytes + bookkeeping_bytes;
    }
};

// Marks the world's globals entity (see world::globals()). Public so that
// archives can round-trip it (pack<globals_mark, ...>) and broad tools can
// except<globals_mark> it out.
struct globals_mark
{
};

// ----------------------------------------------------------------------------
// Reactive hooks
//
// Per-pool callbacks fired on structural changes, for invariant maintenance
// (attach a physics body when a Collider appears, free a GPU handle when a
// Sprite dies):
//
//   auto token = world.on_add<Collider>(
//       [](quiver::world& w, quiver::entity e, void*) { ... });
//   ...
//   world.unhook(token);
//
// Cost model: pools with no hooks pay one pointer test per structural op;
// dispatch happens under the pool's iteration lock, so a hook that
// structurally mutates ITS OWN pool is a reported violation instead of UB
// (other pools are fair game — chain reactions work). on_remove fires before
// the component is destroyed, so the hook may read it. Hooks do not fire
// during world destruction, and must not throw. The user pointer passes
// through untouched.
//
// Connection forms (each returns a hook_token; scoped_hook wraps one in RAII):
//   world.on_add<T>(fn, user)              — function pointer + user data
//   world.on_add<T, &on_collider>()        — free function: (world&, entity) or (entity)
//   world.on_add<T, &Phys::body>(&phys)    — member function on an instance
// ----------------------------------------------------------------------------

using component_hook = void (*)(world&, entity, void* user);

struct hook_token
{
    std::uint32_t pool = 0xFFFFFFFFu;  // type id of the hooked pool
    std::uint32_t id = 0;              // serial within that pool; 0 = empty token

    constexpr explicit operator bool() const noexcept { return id != 0; }
};

namespace detail
{
// Adapts a compile-time callable (free function, capture-less function
// object) to the component_hook convention. Candidates may take
// (world&, entity) or just (entity).
template <auto Candidate>
consteval component_hook free_hook_thunk()
{
    if constexpr (std::invocable<decltype(Candidate), world&, entity>)
    {
        return +[](world& w, entity e, void*) { std::invoke(Candidate, w, e); };
    }
    else
    {
        static_assert(std::invocable<decltype(Candidate), entity>,
                      "quiver: a hook candidate must be callable as (world&, entity) or "
                      "(entity)");
        return +[](world&, entity e, void*) { std::invoke(Candidate, e); };
    }
}

// Adapts a member function (or any callable taking the instance first) plus
// an instance pointer. Constness flows through Inst: connecting through a
// const instance only compiles for const-invocable candidates.
template <auto Candidate, class Inst>
consteval component_hook bound_hook_thunk()
{
    if constexpr (std::invocable<decltype(Candidate), Inst&, world&, entity>)
    {
        return +[](world& w, entity e, void* user)
        { std::invoke(Candidate, *static_cast<Inst*>(user), w, e); };
    }
    else
    {
        static_assert(std::invocable<decltype(Candidate), Inst&, entity>,
                      "quiver: a bound hook candidate must be callable as "
                      "(Inst&, world&, entity) or (Inst&, entity)");
        return +[](world&, entity e, void* user)
        { std::invoke(Candidate, *static_cast<Inst*>(user), e); };
    }
}
}  // namespace detail

// ----------------------------------------------------------------------------
// detail: paged sparse index (entity slot index -> dense position)
// ----------------------------------------------------------------------------

namespace detail
{
// Doubles a vector's capacity when it is full, so the push_back that follows
// cannot throw. Used to sequence "allocate bookkeeping first, construct the
// component last" in the structural paths.
template <class Vector>
void grow_if_full(Vector& v)
{
    if (v.size() == v.capacity())
    {
        v.reserve(std::max<std::size_t>(8, v.capacity() * 2));
    }
}

class sparse_index
{
public:
    static constexpr std::uint32_t page_size = 4096;  // entries; 16 KiB per page

    explicit sparse_index(std::pmr::memory_resource* memory) noexcept
        : memory_(memory),
          pages_(memory)
    {
    }

    sparse_index(const sparse_index&) = delete;
    sparse_index& operator=(const sparse_index&) = delete;
    sparse_index(sparse_index&&) = delete;  // pools never move
    sparse_index& operator=(sparse_index&&) = delete;

    ~sparse_index() { clear(); }

    // O(1): page lookup. npos32 when the key is absent.
    [[nodiscard]] std::uint32_t get(std::uint32_t key) const noexcept
    {
        const std::size_t p = key / page_size;
        if (p >= pages_.size() || pages_[p] == nullptr)
        {
            return npos32;
        }
        return pages_[p][key % page_size];
    }

    void set(std::uint32_t key, std::uint32_t value)
    {
        page(key / page_size)[key % page_size] = value;
    }

    // Pre-allocates the page for key, so a following set(key, ...) cannot throw.
    void ensure(std::uint32_t key) { page(key / page_size); }

    // Write for a key whose page is known to exist (the key is already mapped).
    void set_existing(std::uint32_t key, std::uint32_t value) noexcept
    {
        pages_[key / page_size][key % page_size] = value;
    }

    // Honest accounting: allocated pages plus the page-table vector itself.
    [[nodiscard]] std::size_t bytes() const noexcept
    {
        std::size_t pages = 0;
        for (const auto& page : pages_)
        {
            pages += page != nullptr ? 1 : 0;
        }
        return (pages * page_size * sizeof(std::uint32_t)) +
               (pages_.capacity() * sizeof(pages_[0]));
    }

    void erase(std::uint32_t key) noexcept
    {
        const std::size_t p = key / page_size;
        if (p < pages_.size() && pages_[p] != nullptr)
        {
            pages_[p][key % page_size] = npos32;
        }
    }

    void clear() noexcept
    {
        for (std::uint32_t* page : pages_)
        {
            if (page != nullptr)
            {
                memory_->deallocate(
                    page, page_size * sizeof(std::uint32_t), alignof(std::uint32_t));
            }
        }
        pages_.clear();
    }

    // Visits every present (key, value) pair; cold path (validation).
    template <class F>
    void visit(F&& fn) const
    {
        for (std::size_t p = 0; p < pages_.size(); ++p)
        {
            if (pages_[p] == nullptr)
            {
                continue;
            }
            for (std::uint32_t i = 0; i < page_size; ++i)
            {
                if (pages_[p][i] != npos32)
                {
                    fn(static_cast<std::uint32_t>((p * page_size) + i), pages_[p][i]);
                }
            }
        }
    }

private:
    std::uint32_t* page(std::size_t p)
    {
        if (p >= pages_.size())
        {
            pages_.resize(p + 1, nullptr);
        }
        if (pages_[p] == nullptr)
        {
            auto* fresh = static_cast<std::uint32_t*>(
                memory_->allocate(page_size * sizeof(std::uint32_t), alignof(std::uint32_t)));
            std::fill_n(fresh, page_size, npos32);
            pages_[p] = fresh;
        }
        return pages_[p];
    }

    std::pmr::memory_resource* memory_;
    std::pmr::vector<std::uint32_t*> pages_;
};

// ----------------------------------------------------------------------------
// detail: entity table
//
// Per slot: a generation and a free flag. Free slots are recycled through a
// stack with lazy invalidation: restore_entity may claim a slot that is still
// recorded on the stack, so spawn discards stack entries whose flag says the
// slot is no longer free. Amortized O(1), and no free-list surgery on restore.
// ----------------------------------------------------------------------------

template <entity_traits Traits>
class basic_entity_table
{
public:
    // World-side slot plumbing is 32-bit; wider HANDLES exist standalone
    // (basic_entity puts no ceiling on index_type), but a table minting them
    // is the staged remainder of the traits conversion.
    static_assert(std::numeric_limits<typename Traits::index_type>::digits <= 32,
                  "quiver: worlds support entity index types up to 32 bits");

    using entity = quiver::basic_entity<Traits>;
    using index_type = typename Traits::index_type;
    using generation_type = typename Traits::generation_type;

    static constexpr std::uint32_t max_slots =
        static_cast<std::uint32_t>(entity_limits<Traits>::max_slots);

    explicit basic_entity_table(std::pmr::memory_resource* memory) noexcept
        : generation_(memory),
          free_flag_(memory),
          free_stack_(memory)
    {
    }

    // Moved-from tables are empty and reusable (scalar counters reset too).
    basic_entity_table(basic_entity_table&& other) noexcept
        : generation_(std::move(other.generation_)),
          free_flag_(std::move(other.free_flag_)),
          free_stack_(std::move(other.free_stack_)),
          live_(std::exchange(other.live_, 0))
    {
    }

    // Not noexcept: assigning between tables on different memory resources
    // falls back to element-wise moves, which may allocate.
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor,bugprone-exception-escape)
    basic_entity_table& operator=(basic_entity_table&& other)
    {
        if (this != &other)
        {
            generation_ = std::move(other.generation_);
            free_flag_ = std::move(other.free_flag_);
            free_stack_ = std::move(other.free_stack_);
            live_ = std::exchange(other.live_, 0);
            other.generation_.clear();
            other.free_flag_.clear();
            other.free_stack_.clear();
        }
        return *this;
    }

    basic_entity_table(const basic_entity_table&) = delete;
    basic_entity_table& operator=(const basic_entity_table&) = delete;
    ~basic_entity_table() = default;

    // O(1) amortized. Never touches component pools (that is what makes bare
    // spawn() legal during iteration).
    entity create()
    {
        while (!free_stack_.empty())
        {
            const std::uint32_t index = free_stack_.back();
            free_stack_.pop_back();
            if (free_flag_[index] != 0)
            {
                // Pop + claim leaves size+live unchanged: slack is preserved.
                free_flag_[index] = 0;
                ++live_;
                return entity(static_cast<index_type>(index), generation_[index]);
            }
            // Stale entry: the slot was claimed by restore() after being freed.
        }
        const auto index = static_cast<std::uint32_t>(generation_.size());
        if (index >= max_slots)  // unconditional: the provisional index bit is reserved
        {
            violate("entity table is full (2^index_bits - 1 slots)");
            std::abort();  // fatal even with a returning handler: no valid handle exists
        }
        generation_.push_back(0);
        free_flag_.push_back(0);
        ++live_;
        ensure_destroy_slack();  // pay for the future destroy here, where we allocate anyway
        return entity(static_cast<index_type>(index), 0);
    }

    // O(1). The generation bump is what invalidates outstanding handles.
    // Never allocates: create()/restore() maintain enough free-stack slack for
    // every live entity, which is what makes kill() noexcept end-to-end.
    // NOLINTNEXTLINE(bugprone-exception-escape) -- nofail push_back: slack invariant above
    void destroy(std::uint32_t index) noexcept
    {
        ++generation_[index];  // wraps mod 2^generation_bits by design
        free_flag_[index] = 1;
        free_stack_.push_back(index);
        --live_;
    }

    [[nodiscard]] bool alive(entity e) const noexcept
    {
        const std::uint32_t index = e.index();
        return index < generation_.size() && free_flag_[index] == 0 &&
               generation_[index] == e.generation();
    }

    [[nodiscard]] bool occupied(std::uint32_t index) const noexcept
    {
        return index < generation_.size() && free_flag_[index] == 0;
    }

    [[nodiscard]] generation_type generation_at(std::uint32_t index) const noexcept
    {
        return generation_[index];
    }

    // Claims an exact slot at an exact generation (snapshot restore). The slot
    // may sit anywhere in the free stack; its stack entries go stale and are
    // discarded lazily by create().
    std::expected<entity, fault> restore(entity e)
    {
        if (!e || is_provisional(e) || e.index() >= max_slots)
        {
            return std::unexpected(
                fault{fault_code::bad_handle, {}, "restore_entity: invalid handle"});
        }
        const std::uint32_t index = e.index();
        while (generation_.size() <= index)
        {
            const auto fresh = static_cast<std::uint32_t>(generation_.size());
            generation_.push_back(0);
            free_flag_.push_back(1);
            free_stack_.push_back(fresh);
        }
        if (free_flag_[index] == 0)
        {
            return std::unexpected(
                fault{fault_code::slot_occupied, {}, "restore_entity: slot holds a live entity"});
        }
        free_flag_[index] = 0;
        generation_[index] = e.generation();
        ++live_;
        ensure_destroy_slack();
        return e;
    }

    // Frees every live slot, bumping generations so old handles die. Keeps
    // capacity. O(slots).
    void destroy_all() noexcept
    {
        for (std::uint32_t index = 0; index < generation_.size(); ++index)
        {
            if (free_flag_[index] == 0)
            {
                destroy(index);
            }
        }
    }

    void reserve(std::size_t n)
    {
        generation_.reserve(n);
        free_flag_.reserve(n);
        free_stack_.reserve(n);
    }

    // Drops stale free-stack entries and returns slack memory.
    void shrink()
    {
        std::erase_if(free_stack_, [this](std::uint32_t index) { return free_flag_[index] == 0; });
        // A slot can sit on the stack twice (freed, restored, freed again);
        // keep only the newest entry of each.
        std::pmr::vector<std::uint8_t> seen(generation_.size(), 0, generation_.get_allocator());
        std::pmr::vector<std::uint32_t> kept(free_stack_.get_allocator());
        kept.reserve(free_stack_.size());
        for (std::size_t i = free_stack_.size(); i-- > 0;)  // newest entry of each slot wins
        {
            const std::uint32_t index = free_stack_[i];
            if (seen[index] == 0)
            {
                seen[index] = 1;
                kept.push_back(index);
            }
        }
        std::ranges::reverse(kept);
        free_stack_ = std::move(kept);
        generation_.shrink_to_fit();
        free_flag_.shrink_to_fit();
        ensure_destroy_slack();  // the rebuilt stack must keep destroy() nofail
    }

    [[nodiscard]] std::size_t slots() const noexcept { return generation_.size(); }
    [[nodiscard]] std::size_t live() const noexcept { return live_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return generation_.capacity(); }

    [[nodiscard]] std::size_t bytes() const noexcept
    {
        return (generation_.capacity() * sizeof(generation_type)) + free_flag_.capacity() +
               (free_stack_.capacity() * sizeof(std::uint32_t));
    }

    [[nodiscard]] std::expected<void, fault> check() const
    {
        if (free_flag_.size() != generation_.size())
        {
            return std::unexpected(
                fault{fault_code::table_out_of_sync, {}, "flag/generation size"});
        }
        std::size_t free_count = 0;
        for (const std::uint8_t flag : free_flag_)
        {
            free_count += flag;
        }
        if (live_ + free_count != generation_.size())
        {
            return std::unexpected(fault{fault_code::table_out_of_sync, {}, "live count"});
        }
        std::vector<std::uint8_t> reachable(generation_.size(), 0);
        for (const std::uint32_t index : free_stack_)
        {
            if (index >= generation_.size())
            {
                return std::unexpected(
                    fault{fault_code::table_out_of_sync, {}, "free stack out of range"});
            }
            reachable[index] = 1;
        }
        for (std::uint32_t index = 0; index < generation_.size(); ++index)
        {
            if (free_flag_[index] != 0 && reachable[index] == 0)
            {
                return std::unexpected(
                    fault{fault_code::table_out_of_sync, {}, "free slot not recyclable"});
            }
        }
        return {};
    }

private:
    friend struct quiver::test_access;

    // Invariant: free_stack_ has reserved room for one entry per live entity,
    // so destroy() never reallocates. Re-established by every operation that
    // raises live_ (create, restore) and by shrink().
    void ensure_destroy_slack()
    {
        const std::size_t needed = free_stack_.size() + live_;
        if (free_stack_.capacity() < needed)
        {
            free_stack_.reserve(std::max(needed, free_stack_.capacity() * 2));
        }
    }

    std::pmr::vector<generation_type> generation_;
    std::pmr::vector<std::uint8_t> free_flag_;  // 1 = free
    std::pmr::vector<std::uint32_t> free_stack_;
    std::size_t live_ = 0;
};

using entity_table = basic_entity_table<default_entity_traits>;

// Invariant 3 of the handle layout (see the comment at provisional_bit).
static_assert(entity_table::max_slots == provisional_bit - 1);

// ----------------------------------------------------------------------------
// detail: component pools
//
// pool_base owns the membership data (sparse index + dense entity array) so
// that iteration and filtering never make a virtual call. The virtual surface
// is cold-path only: kill / purge / reset / shrink / inspect / validate.
// ----------------------------------------------------------------------------

class pool_base;

// The default sorting algorithm, injectable through world::sort's Algorithm
// parameter. Replacements must permute the random-access range exactly as a
// comparison sort over cmp would (the cycle-walk below consumes a gather
// permutation): std::stable_sort qualifies, a partial sort does not.
struct std_sort
{
    template <class It, class Cmp>
    void operator()(It first, It last, Cmp cmp) const
    {
        std::sort(first, last, cmp);
    }
};

// One bonded group (world::bond<Ts...>): every owner keeps the N-way
// intersection mirror-partitioned at dense positions [0, paired) — the same
// entity at the same position in ALL owners. Heap-stable, owned by the
// world; unbond() tombstones (owner_count = 0, paired = 0) so stored bonded
// views degrade to empty instead of dangling. The owner array is bounded so
// the object stays allocation-free and debugger-plain; pairs (N = 2) are
// the common case and generate exactly the historical two-pool work.
struct group_core
{
    static constexpr std::uint32_t max_owners = 8;

    std::array<pool_base*, max_owners> owners{};
    std::uint32_t owner_count = 0;
    std::uint32_t paired = 0;  // k: dense_[0, k) is the mirrored intersection

    [[nodiscard]] bool owns(const pool_base* p) const noexcept
    {
        for (std::uint32_t i = 0; i < owner_count; ++i)
        {
            if (owners[i] == p)
            {
                return true;
            }
        }
        return false;
    }
};

class pool_base
{
public:
    pool_base(std::pmr::memory_resource* memory,
              std::string_view name,
              std::uint64_t name_hash,
              storage kind,
              std::size_t item_bytes) noexcept
        : sparse_(memory),
          dense_(memory),
          memory_(memory),
          name_(name),
          name_hash_(name_hash),
          kind_(kind),
          item_bytes_(item_bytes)
    {
    }

    virtual ~pool_base() = default;
    pool_base(const pool_base&) = delete;
    pool_base& operator=(const pool_base&) = delete;

    // --- hot, non-virtual membership ---

    [[nodiscard]] bool contains(std::uint32_t index) const noexcept
    {
        return sparse_.get(index) != npos32;
    }

    [[nodiscard]] std::uint32_t position_of(std::uint32_t index) const noexcept
    {
        return sparse_.get(index);
    }

    [[nodiscard]] std::size_t size() const noexcept { return dense_.size(); }
    [[nodiscard]] entity entity_at(std::size_t pos) const noexcept { return dense_[pos]; }

    // --- cold, type-erased operations ---

    // Copies the component at src_index onto dst (false when T cannot be
    // copy-constructed). Cold path, drives world::duplicate.
    virtual bool copy_item(std::uint32_t src_index, entity dst) = 0;

    virtual void erase_if_present(std::uint32_t index) noexcept = 0;
    virtual void wipe() noexcept = 0;  // destroy all components, keep the pool object
    virtual void compact() = 0;

    // Type-erased payload access for tooling (pool_ref::raw): the component
    // bytes at a dense position, or null for tag pools. Pair with
    // pool_info::bytes_per_item; the pointer obeys the pool's invalidation
    // rules like any component reference.
    [[nodiscard]] virtual void* item_address(std::uint32_t /*pos*/) noexcept { return nullptr; }

    [[nodiscard]] virtual std::size_t item_capacity() const noexcept = 0;
    [[nodiscard]] virtual std::size_t extra_bookkeeping_bytes() const noexcept { return 0; }
    [[nodiscard]] virtual std::expected<void, fault> check(const entity_table& table) const = 0;

    [[nodiscard]] pool_info info() const noexcept
    {
        return pool_info{name_,
                         id_,
                         name_hash_,
                         dense_.size(),
                         item_capacity(),
                         item_bytes_,
                         sparse_.bytes(),
                         (dense_.capacity() * sizeof(entity)) + extra_bookkeeping_bytes(),
                         kind_};
    }

    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] std::uint64_t name_hash() const noexcept { return name_hash_; }
    [[nodiscard]] bool locked() const noexcept { return locks_ != 0; }

    // Structural changes on a bonded pool can mirror-swap every co-owner, so
    // any co-owner's iteration couples into this pool's structural refusals.
    [[nodiscard]] bool bond_locked() const noexcept
    {
        if (locked())
        {
            return true;
        }
        if (bond_ != nullptr)
        {
            for (std::uint32_t i = 0; i < bond_->owner_count; ++i)
            {
                if (bond_->owners[i] != this && bond_->owners[i]->locked())
                {
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] const group_core* bond() const noexcept { return bond_; }

    // --- reactive hooks (world::on_add/on_remove/on_replace connect here) ---

    enum class hook_kind : std::uint8_t
    {
        add,
        remove,
        replace,
    };

    std::uint32_t connect_hook(hook_kind kind, component_hook fn, void* user)
    {
        if (!hooks_)
        {
            hooks_ = std::make_unique<hook_lists>();
        }
        const std::uint32_t id = hooks_->next_id++;
        list_for(kind).push_back(hook_entry{fn, user, id});
        return id;
    }

    bool disconnect_hook(std::uint32_t id) noexcept
    {
        if (!hooks_)
        {
            return false;
        }
        const auto drop = [id](std::vector<hook_entry>& list)
        { return std::erase_if(list, [id](const hook_entry& h) { return h.id == id; }) > 0; };
        return drop(hooks_->on_add) || drop(hooks_->on_remove) || drop(hooks_->on_replace);
    }

    // One pointer test on the common (hook-free) path.
    void fire_add(entity e) { dispatch_if(hooks_ ? &hooks_->on_add : nullptr, e); }
    void fire_remove(entity e) { dispatch_if(hooks_ ? &hooks_->on_remove : nullptr, e); }
    void fire_replace(entity e) { dispatch_if(hooks_ ? &hooks_->on_replace : nullptr, e); }

    void fire_remove_all()
    {
        if (hooks_ && !hooks_->on_remove.empty())
        {
            // Index loop, like dispatch_if: stays valid even if a misbehaving
            // hook grows dense_ through the report-then-proceed add path.
            // NOLINTNEXTLINE(modernize-loop-convert)
            for (std::size_t i = 0; i < dense_.size(); ++i)
            {
                dispatch_if(&hooks_->on_remove, dense_[i]);
            }
        }
    }

protected:
    // Swaps two dense positions INCLUDING the derived pool's payload mirror.
    // The cross-pool half of bond maintenance (and the bond build pass) runs
    // through this; cold, and compiled per pool type.
    virtual void mirror_swap(std::uint32_t a, std::uint32_t b) noexcept = 0;

    // Membership insert; the caller appends payload first (so a throwing
    // component constructor leaves the pool untouched).
    void attach(entity e)
    {
        dense_.push_back(e);
        sparse_.set(e.index(), static_cast<std::uint32_t>(dense_.size() - 1));
        if (bond_ != nullptr)
        {
            bond_on_attach(e);
        }
    }

    // Membership swap-remove. Returns the dense position that was vacated;
    // the caller mirrors the same swap in its payload arrays.
    std::uint32_t detach(std::uint32_t index) noexcept
    {
        if (bond_ != nullptr)
        {
            bond_on_detach(index);  // moves the entity out of the partition first
        }
        const std::uint32_t pos = sparse_.get(index);
        const auto last = static_cast<std::uint32_t>(dense_.size() - 1);
        if (pos != last)
        {
            dense_[pos] = dense_[last];
            sparse_.set_existing(dense_[pos].index(), pos);  // page exists: member entity
        }
        dense_.pop_back();
        sparse_.erase(index);
        return pos;
    }

    // This pool just attached e at the back. If EVERY other owner holds e
    // too, the entity enters the intersection: swap e to slot `paired` in
    // ALL owners. Each owner's prior position is >= paired (e was not
    // intersecting), so the mirrored prefix is untouched and gains e at the
    // same index everywhere. O(owners).
    void bond_on_attach(entity e) noexcept
    {
        for (std::uint32_t i = 0; i < bond_->owner_count; ++i)
        {
            pool_base* other = bond_->owners[i];
            if (other != this && !other->contains(e.index()))
            {
                return;
            }
        }
        const std::uint32_t k = bond_->paired;
        for (std::uint32_t i = 0; i < bond_->owner_count; ++i)
        {
            pool_base* owner = bond_->owners[i];
            const std::uint32_t at = owner->sparse_.get(e.index());
            if (at != k)
            {
                owner->mirror_swap(k, at);
            }
        }
        bond_->paired = k + 1;
    }

    // This pool is about to swap-remove the entity at slot `index`. If it is
    // paired, move it (mirrored in every owner) to the partition edge first
    // so the removal's own swap stays entirely in the unpaired tail. The
    // entity remains in the other owners, correctly unpaired. O(owners).
    void bond_on_detach(std::uint32_t index) noexcept
    {
        const std::uint32_t pos = sparse_.get(index);
        if (pos >= bond_->paired)
        {
            return;
        }
        if constexpr (checks_enabled)
        {
            for (std::uint32_t i = 0; i < bond_->owner_count; ++i)
            {
                if (bond_->owners[i]->sparse_.get(index) != pos)  // the mirror invariant
                {
                    violate_pool("bonded partition out of mirror; pool", name_);
                    break;
                }
            }
        }
        const std::uint32_t edge = bond_->paired - 1;
        if (pos != edge)
        {
            for (std::uint32_t i = 0; i < bond_->owner_count; ++i)
            {
                bond_->owners[i]->mirror_swap(pos, edge);
            }
        }
        bond_->paired = edge;
    }

    // Swaps two dense positions and repairs both sparse links; payload
    // mirroring is the derived pool's job.
    void swap_dense(std::uint32_t a, std::uint32_t b) noexcept
    {
        std::swap(dense_[a], dense_[b]);
        sparse_.set_existing(dense_[a].index(), a);
        sparse_.set_existing(dense_[b].index(), b);
    }

    // Sorts the dense order by a position comparator, then applies the
    // permutation with cycle-walking swaps (swap_payload(a, b) mirrors each
    // swap into the derived pool's arrays). Cold path: one O(n) scratch
    // allocation (from the pool's resource) plus the O(n log n) sort.
    template <class PosCompare, class SwapPayload, class Algo = std_sort>
    void sort_dense_impl(PosCompare cmp, SwapPayload swap_payload, Algo&& algo = {})
    {
        const auto n = static_cast<std::uint32_t>(dense_.size());
        std::pmr::vector<std::uint32_t> perm(n, memory_);
        for (std::uint32_t i = 0; i < n; ++i)
        {
            perm[i] = i;
        }
        algo(perm.begin(), perm.end(), cmp);  // gather: perm[new] = old position
        // Apply the gather permutation in place, cycle by cycle: position i
        // receives the element that was at perm[i].
        for (std::uint32_t i = 0; i < n; ++i)
        {
            std::uint32_t curr = i;
            std::uint32_t next = perm[curr];
            while (next != i)
            {
                swap_dense(curr, next);
                swap_payload(curr, next);
                perm[curr] = curr;
                curr = next;
                next = perm[curr];
            }
            perm[curr] = curr;
        }
    }

    [[nodiscard]] std::expected<void, fault> check_membership(const entity_table& table) const
    {
        for (std::size_t pos = 0; pos < dense_.size(); ++pos)
        {
            const entity e = dense_[pos];
            if (!table.alive(e))
            {
                return std::unexpected(fault{fault_code::dense_entity_dead, name_, "dead entity"});
            }
            if (sparse_.get(e.index()) != pos)
            {
                return std::unexpected(
                    fault{fault_code::sparse_dense_desync, name_, "dense -> sparse"});
            }
        }
        std::expected<void, fault> result{};
        sparse_.visit(
            [&](std::uint32_t key, std::uint32_t value)
            {
                if (!result)
                {
                    return;
                }
                if (value >= dense_.size() || dense_[value].index() != key)
                {
                    result = std::unexpected(
                        fault{fault_code::sparse_dense_desync, name_, "sparse -> dense"});
                }
            });
        return result;
    }

    friend class quiver::world;
    friend class single_pool_lock;
    friend class quiver::runtime_selection;
    template <class Excludes, class... Ts>
    friend class quiver::selection_t;
    template <class Excludes, class... Ts>
    friend class quiver::bonded_view_t;
    friend struct quiver::test_access;  // declared unconditionally; defined only under checks

    struct hook_entry
    {
        component_hook fn;
        void* user;
        std::uint32_t id;
    };

    struct hook_lists
    {
        std::vector<hook_entry> on_add;
        std::vector<hook_entry> on_remove;
        std::vector<hook_entry> on_replace;
        std::uint32_t next_id = 1;
    };

    [[nodiscard]] std::vector<hook_entry>& list_for(hook_kind kind) noexcept
    {
        switch (kind)
        {
            case hook_kind::add:
                return hooks_->on_add;
            case hook_kind::remove:
                return hooks_->on_remove;
            case hook_kind::replace:
                return hooks_->on_replace;
        }
        std::unreachable();  // the enum is exhaustive
    }

    // Defined after single_pool_lock (dispatch runs under it).
    void dispatch_if(const std::vector<hook_entry>* list, entity e);

    sparse_index sparse_;
    std::pmr::vector<entity> dense_;
    std::unique_ptr<hook_lists> hooks_;  // null until the first hook connects
    group_core* bond_ = nullptr;         // null unless this pool is owned by a bond
    quiver::world* owner_ = nullptr;     // set at registration; hooks receive it
    std::pmr::memory_resource* memory_;  // scaling allocations route through this
    std::string_view name_;
    std::uint64_t name_hash_;
    storage kind_;
    std::size_t item_bytes_;
    std::uint32_t id_ = 0;             // global type id, set by the owning world
    mutable std::uint32_t locks_ = 0;  // running iterations that include this pool
};

// RAII iteration lock over one pool (children_of and friends). Counter
// maintenance is compiled out with QUIVER_CHECKS off, like selection locks.
class single_pool_lock
{
public:
    explicit single_pool_lock(const pool_base* pool) noexcept
        : pool_(pool)
    {
        if constexpr (checks_enabled)
        {
            ++pool_->locks_;
        }
    }

    ~single_pool_lock()
    {
        if constexpr (checks_enabled)
        {
            --pool_->locks_;
        }
    }

    single_pool_lock(const single_pool_lock&) = delete;
    single_pool_lock& operator=(const single_pool_lock&) = delete;

private:
    const pool_base* pool_;
};

// Hook dispatch runs under the pool's iteration lock: a hook structurally
// mutating its own pool is a reported violation, not UB. Hooks must not throw
// (several call sites are noexcept).
inline void pool_base::dispatch_if(const std::vector<hook_entry>* list, entity e)
{
    if (list == nullptr || list->empty())
    {
        return;
    }
    const single_pool_lock lock(this);
    // Index loop: connect/disconnect on a locked pool is refused, but stay
    // robust against a hook that ignores the violation and proceeds.
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (std::size_t i = 0; i < list->size(); ++i)
    {
        const hook_entry h = (*list)[i];
        h.fn(*owner_, e, h.user);
    }
}

// Packed: components in one dense vector parallel to the entity array.
template <component T>
class packed_pool : public pool_base
{
public:
    static_assert(std::is_move_constructible_v<T>,
                  "quiver: packed storage moves components on add/remove; give T a move "
                  "constructor or select quiver::storage::stable for it");

    explicit packed_pool(std::pmr::memory_resource* memory) noexcept
        : pool_base(memory, name_of<T>(), hash_of<T>(), storage::packed, sizeof(T)),
          items_(memory)
    {
    }

    ~packed_pool() override = default;

    template <class... Args>
    T& emplace(entity e, Args&&... args)
    {
        // Bookkeeping capacity first: once it is secured, neither a throwing
        // T constructor (vector strong guarantee) nor attach() can leave the
        // pool half-updated.
        grow_if_full(items_);
        grow_if_full(dense_);
        sparse_.ensure(e.index());
        items_.emplace_back(std::forward<Args>(args)...);
        attach(e);
        fire_add(e);
        // Re-read the position last: a bond fix-up in attach() (or one caused
        // by a misbehaving hook) may have relocated the new item.
        return items_[sparse_.get(e.index())];
    }

    [[nodiscard]] T* at(std::uint32_t index) noexcept
    {
        const std::uint32_t pos = sparse_.get(index);
        return pos == npos32 ? nullptr : items_.data() + pos;
    }

    [[nodiscard]] const T* at(std::uint32_t index) const noexcept
    {
        const std::uint32_t pos = sparse_.get(index);
        return pos == npos32 ? nullptr : items_.data() + pos;
    }

    [[nodiscard]] const T& at_pos(std::uint32_t pos) const noexcept { return items_[pos]; }
    [[nodiscard]] T& at_pos(std::uint32_t pos) noexcept { return items_[pos]; }

    [[nodiscard]] void* item_address(std::uint32_t pos) noexcept override { return &items_[pos]; }

    template <class PosCompare, class Algo = std_sort>
    void sort_dense(PosCompare cmp, Algo&& algo = {})
    {
        static_assert(std::is_swappable_v<T>,
                      "quiver: sorting a packed pool swaps components; T must be swappable "
                      "(stable storage sorts without touching payloads)");
        sort_dense_impl(
            cmp,
            [this](std::uint32_t a, std::uint32_t b) { std::swap(items_[a], items_[b]); },
            std::forward<Algo>(algo));
    }

    bool copy_item(std::uint32_t src_index, entity dst) override
    {
        if constexpr (std::copy_constructible<T>)
        {
            // Copy first: emplace may reallocate items_, which would dangle a
            // reference into the pool's own payload vector.
            T detached(items_[sparse_.get(src_index)]);
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
        static_assert(std::is_swappable_v<T>,
                      "quiver: reordering a packed pool swaps components; T must be swappable "
                      "(stable storage reorders without touching payloads)");
        swap_dense(a, b);
        std::swap(items_[a], items_[b]);
    }

    // The if constexpr is load-bearing: virtuals instantiate with the class,
    // and non-swappable packed pools exist legally as long as they are never
    // bonded (bond<A, B>() statically walls off packed-derived pools of
    // non-swappable components — derived_from, so custom pools are covered).
    void mirror_swap(std::uint32_t a, std::uint32_t b) noexcept override
    {
        if constexpr (std::is_swappable_v<T>)
        {
            swap_positions(a, b);
        }
        else
        {
            std::abort();  // unreachable: bond() refuses this pool at compile time
        }
    }

    void erase_if_present(std::uint32_t index) noexcept override
    {
        if (!contains(index))
        {
            return;
        }
        fire_remove(dense_[sparse_.get(index)]);  // before destruction: hooks may read it
        const std::uint32_t pos = detach(index);
        const auto last = static_cast<std::uint32_t>(items_.size() - 1);
        if (pos != last)
        {
            if constexpr (std::is_move_assignable_v<T>)
            {
                items_[pos] = std::move(items_[last]);
            }
            else
            {
                std::destroy_at(&items_[pos]);
                std::construct_at(&items_[pos], std::move(items_[last]));
            }
        }
        items_.pop_back();
    }

    void wipe() noexcept override
    {
        fire_remove_all();
        items_.clear();
        dense_.clear();
        sparse_.clear();
    }

    void compact() override
    {
        items_.shrink_to_fit();
        dense_.shrink_to_fit();
    }

    void reserve(std::size_t n)
    {
        items_.reserve(n);
        dense_.reserve(n);
    }

    [[nodiscard]] std::size_t item_capacity() const noexcept override { return items_.capacity(); }

    [[nodiscard]] std::expected<void, fault> check(const entity_table& table) const override
    {
        if (items_.size() != dense_.size())
        {
            return std::unexpected(fault{fault_code::sparse_dense_desync, name_, "item count"});
        }
        return check_membership(table);
    }

private:
    std::pmr::vector<T> items_;
};

// Tag: membership only.
template <component T>
class tag_pool : public pool_base
{
public:
    explicit tag_pool(std::pmr::memory_resource* memory) noexcept
        : pool_base(memory, name_of<T>(), hash_of<T>(), storage::tag, 0)
    {
    }

    ~tag_pool() override = default;

    void emplace(entity e)
    {
        grow_if_full(dense_);
        sparse_.ensure(e.index());
        attach(e);
        fire_add(e);
    }

    template <class PosCompare, class Algo = std_sort>
    void sort_dense(PosCompare cmp, Algo&& algo = {})
    {
        sort_dense_impl(
            cmp,
            [](std::uint32_t, std::uint32_t) {},  // membership only
            std::forward<Algo>(algo));
    }

    bool copy_item(std::uint32_t, entity dst) override
    {
        emplace(dst);  // membership is the whole payload
        return true;
    }

    void swap_positions(std::uint32_t a, std::uint32_t b) noexcept { swap_dense(a, b); }

    void mirror_swap(std::uint32_t a, std::uint32_t b) noexcept override { swap_dense(a, b); }

    void erase_if_present(std::uint32_t index) noexcept override
    {
        if (contains(index))
        {
            fire_remove(dense_[sparse_.get(index)]);
            detach(index);
        }
    }

    void wipe() noexcept override
    {
        fire_remove_all();
        dense_.clear();
        sparse_.clear();
    }

    void compact() override { dense_.shrink_to_fit(); }

    void reserve(std::size_t n) { dense_.reserve(n); }

    [[nodiscard]] std::size_t item_capacity() const noexcept override { return dense_.capacity(); }

    [[nodiscard]] std::expected<void, fault> check(const entity_table& table) const override
    {
        return check_membership(table);
    }
};

// Stable: components in fixed chunks that never move. The dense entity array
// is mirrored by a dense slot array; a free list recycles vacated slots.
template <component T>
class stable_pool : public pool_base
{
public:
    // Per-component via quiver::chunk_capacity<T>; defaults to ~4 KiB chunks.
    static constexpr std::size_t chunk_items = chunk_capacity<T>;
    static_assert(chunk_items > 0, "quiver: chunk_capacity<T> must be at least 1");

    explicit stable_pool(std::pmr::memory_resource* memory) noexcept
        : pool_base(memory, name_of<T>(), hash_of<T>(), storage::stable, sizeof(T)),
          slot_of_(memory),
          chunks_(memory),
          free_slots_(memory)
    {
    }

    ~stable_pool() override
    {
        release_all();
        release_chunks();
    }

    template <class... Args>
    T& emplace(entity e, Args&&... args)
    {
        // Bookkeeping capacity first, component construction last: a throwing
        // step leaves the pool exactly as it was.
        grow_if_full(dense_);
        grow_if_full(slot_of_);
        sparse_.ensure(e.index());
        const std::uint32_t slot = peek_free_slot();  // may allocate a chunk
        T* item = std::construct_at(slot_ptr(slot), std::forward<Args>(args)...);
        commit_free_slot(slot);  // nofail; only after construction succeeded
        slot_of_.push_back(slot);
        attach(e);
        fire_add(e);
        return *item;  // stable: hooks cannot have moved it
    }

    [[nodiscard]] T* at(std::uint32_t index) noexcept
    {
        const std::uint32_t pos = sparse_.get(index);
        return pos == npos32 ? nullptr : slot_ptr(slot_of_[pos]);
    }

    [[nodiscard]] const T* at(std::uint32_t index) const noexcept
    {
        const std::uint32_t pos = sparse_.get(index);
        return pos == npos32 ? nullptr : slot_ptr(slot_of_[pos]);
    }

    [[nodiscard]] const T& at_pos(std::uint32_t pos) const noexcept
    {
        return *slot_ptr(slot_of_[pos]);
    }

    [[nodiscard]] T& at_pos(std::uint32_t pos) noexcept { return *slot_ptr(slot_of_[pos]); }

    [[nodiscard]] void* item_address(std::uint32_t pos) noexcept override
    {
        return slot_ptr(slot_of_[pos]);
    }

    // Payloads never move: only the dense/slot mirrors permute, so stable
    // pointers survive sorting (worth choosing stable storage for).
    template <class PosCompare, class Algo = std_sort>
    void sort_dense(PosCompare cmp, Algo&& algo = {})
    {
        sort_dense_impl(
            cmp,
            [this](std::uint32_t a, std::uint32_t b) { std::swap(slot_of_[a], slot_of_[b]); },
            std::forward<Algo>(algo));
    }

    bool copy_item(std::uint32_t src_index, entity dst) override
    {
        if constexpr (std::copy_constructible<T>)
        {
            emplace(dst, *slot_ptr(slot_of_[sparse_.get(src_index)]));
            return true;
        }
        else
        {
            return false;
        }
    }

    void swap_positions(std::uint32_t a, std::uint32_t b) noexcept
    {
        swap_dense(a, b);
        std::swap(slot_of_[a], slot_of_[b]);  // payloads stay put
    }

    void mirror_swap(std::uint32_t a, std::uint32_t b) noexcept override { swap_positions(a, b); }

    [[nodiscard]] std::size_t extra_bookkeeping_bytes() const noexcept override
    {
        return (slot_of_.capacity() * sizeof(std::uint32_t)) +
               (free_slots_.capacity() * sizeof(std::uint32_t)) +
               (chunks_.capacity() * sizeof(std::byte*));
    }

    // add_chunk pre-reserves one free-list entry per slot, so the push_back
    // below can never reallocate (which is what the lint cannot see).
    // NOLINTNEXTLINE(bugprone-exception-escape)
    void erase_if_present(std::uint32_t index) noexcept override
    {
        if (!contains(index))
        {
            return;
        }
        fire_remove(dense_[sparse_.get(index)]);  // before destruction: hooks may read it
        const std::uint32_t pos = detach(index);
        const std::uint32_t slot = slot_of_[pos];
        std::destroy_at(slot_ptr(slot));
        free_slots_.push_back(slot);
        const auto last = static_cast<std::uint32_t>(slot_of_.size() - 1);
        if (pos != last)
        {
            slot_of_[pos] = slot_of_[last];
        }
        slot_of_.pop_back();
    }

    void wipe() noexcept override
    {
        fire_remove_all();
        release_all();
        dense_.clear();
        sparse_.clear();
        slot_of_.clear();
        free_slots_.clear();
        release_chunks();
        slot_count_ = 0;
    }

    // Chunks are deliberately never freed by compact(): that is the pointer
    // stability contract. Only bookkeeping slack is returned.
    void compact() override
    {
        dense_.shrink_to_fit();
        slot_of_.shrink_to_fit();
        free_slots_.shrink_to_fit();
    }

    void reserve(std::size_t n)
    {
        dense_.reserve(n);
        slot_of_.reserve(n);
        while (chunks_.size() * chunk_items < n)
        {
            add_chunk();
        }
    }

    [[nodiscard]] std::size_t item_capacity() const noexcept override
    {
        return chunks_.size() * chunk_items;
    }

    [[nodiscard]] std::expected<void, fault> check(const entity_table& table) const override
    {
        if (slot_of_.size() != dense_.size())
        {
            return std::unexpected(fault{fault_code::slot_map_broken, name_, "slot count"});
        }
        if (dense_.size() + free_slots_.size() != slot_count_)
        {
            return std::unexpected(fault{fault_code::slot_map_broken, name_, "slot accounting"});
        }
        std::vector<std::uint8_t> used(slot_count_, 0);
        for (const std::uint32_t slot : slot_of_)
        {
            if (slot >= slot_count_ || used[slot] != 0)
            {
                return std::unexpected(fault{fault_code::slot_map_broken, name_, "dense slots"});
            }
            used[slot] = 1;
        }
        for (const std::uint32_t slot : free_slots_)
        {
            if (slot >= slot_count_ || used[slot] != 0)
            {
                return std::unexpected(fault{fault_code::slot_map_broken, name_, "free slots"});
            }
            used[slot] = 1;
        }
        return check_membership(table);
    }

private:
    [[nodiscard]] T* slot_ptr(std::uint32_t slot) noexcept
    {
        return reinterpret_cast<T*>(chunks_[slot / chunk_items]) + (slot % chunk_items);
    }

    [[nodiscard]] const T* slot_ptr(std::uint32_t slot) const noexcept
    {
        return reinterpret_cast<const T*>(chunks_[slot / chunk_items]) + (slot % chunk_items);
    }

    [[nodiscard]] std::uint32_t peek_free_slot()
    {
        if (!free_slots_.empty())
        {
            return free_slots_.back();
        }
        if (slot_count_ == chunks_.size() * chunk_items)
        {
            add_chunk();
        }
        return static_cast<std::uint32_t>(slot_count_);
    }

    void commit_free_slot(std::uint32_t slot) noexcept
    {
        if (!free_slots_.empty() && free_slots_.back() == slot)
        {
            free_slots_.pop_back();
        }
        else
        {
            ++slot_count_;
        }
    }

    void add_chunk()
    {
        if (chunks_.size() == chunks_.capacity())
        {
            chunks_.reserve(std::max<std::size_t>(4, chunks_.capacity() * 2));
        }
        // erase_if_present is noexcept (a virtual contract), so the free list
        // must never need to grow there: one reserved entry per slot.
        free_slots_.reserve((chunks_.size() + 1) * chunk_items);
        auto* raw = static_cast<std::byte*>(memory_->allocate(chunk_items * sizeof(T), alignof(T)));
        chunks_.push_back(raw);  // nofail: reserved above
    }

    void release_all() noexcept
    {
        for (const std::uint32_t slot : slot_of_)
        {
            std::destroy_at(slot_ptr(slot));
        }
    }

    void release_chunks() noexcept
    {
        for (std::byte* chunk : chunks_)
        {
            memory_->deallocate(chunk, chunk_items * sizeof(T), alignof(T));
        }
        chunks_.clear();
    }

    std::pmr::vector<std::uint32_t> slot_of_;  // parallel to dense_
    std::pmr::vector<std::byte*> chunks_;      // fixed blocks, never relocated
    std::pmr::vector<std::uint32_t> free_slots_;
    std::size_t slot_count_ = 0;  // slots handed out so far (used + free)
};

template <component T>
using pool_for = std::conditional_t<
    storage_policy<T> == storage::tag,
    tag_pool<T>,
    std::conditional_t<storage_policy<T> == storage::stable, stable_pool<T>, packed_pool<T>>>;
}  // namespace detail

// ----------------------------------------------------------------------------
// Custom storage backends — the pool_of seam
//
// Every internal pool lookup goes through pool_of<T>, which by default
// selects the built-in pool for T's storage_policy. Specialize it to supply
// your own storage for one component type — instrumentation, history
// tracking, exotic layouts — and selections, hooks, bonds, sorting, and
// archives keep working unchanged:
//
//   struct telemetry_pool : quiver::packed_pool_of<Telemetry> {
//       using base = quiver::packed_pool_of<Telemetry>;
//       using base::base;                                // (memory_resource*)
//       void erase_if_present(std::uint32_t i) noexcept override { ++erases; ... }
//   };
//   template <> struct quiver::pool_of<Telemetry> { using type = telemetry_pool; };
//
// The contract (checked at registration by the component_pool concept):
// derive from THE built-in pool matching storage_policy<T> —
// packed_pool_of / stable_pool_of / tag_pool_of carry the typed surface the
// world drives (emplace, at, at_pos, sort_dense, swap_positions, reserve) —
// or, for a from-scratch backend, from quiver::basic_pool implementing that
// surface, its cold virtuals (erase_if_present, wipe, compact, copy_item,
// mirror_swap, item_capacity, item_address, check), and the basic_pool
// constructor arguments (memory, name_of<T>(), hash_of<T>(), kind, size).
// Construct from a std::pmr::memory_resource* that outlives the world.
// Overriders of the structural virtuals must uphold their noexcept contracts
// and call (or reproduce) the base behavior — membership bookkeeping is not
// optional, and validate() audits membership, not whether your override did
// its advertised job. The typed surface is NON-virtual: a shadowed emplace
// is called on the world's add paths but bypassed by base-internal ones
// (copy_item -> duplicate) — keep per-add side effects in on_add hooks
// (which fire on every path) or override copy_item too. Like every trait
// here, the specialization must be identical in every TU (ODR). Storage
// TUNING still belongs in storage_policy<T> / chunk_capacity<T>, not here.
// ----------------------------------------------------------------------------

using basic_pool = detail::pool_base;

template <component T>
using packed_pool_of = detail::packed_pool<T>;
template <component T>
using stable_pool_of = detail::stable_pool<T>;
template <component T>
using tag_pool_of = detail::tag_pool<T>;

template <class T>
struct pool_of
{
    using type = detail::pool_for<T>;
};

template <class T>
using pool_of_t = pool_of<T>::type;  // alias-declarations are typename-optional in C++20

// What world registration requires of a pool_of specialization.
template <class P>
concept component_pool =
    std::derived_from<P, basic_pool> && std::constructible_from<P, std::pmr::memory_resource*>;

namespace detail
{
// select<>'s static_asserts, expressed over the public compile-time toolkit
// (the library runs on its own abstractions).

template <class T, class... Us>
inline constexpr bool type_among = contains_type<bare<T>, types<bare<Us>...>>;

template <class... Ts>
inline constexpr bool all_distinct = distinct_t<types<bare<Ts>...>>::size == sizeof...(Ts);
}  // namespace detail

// ----------------------------------------------------------------------------
// Filters
// ----------------------------------------------------------------------------

// Exclude filter for select()/each(): entities holding any of these components
// do not match.   world.select<A, B>(quiver::except<C>{})
template <class... Ts>
struct except
{
};

// Optional-component marker for select(): a maybe<T> never filters and never
// drives iteration; the callback receives T* (nullptr when the entity lacks
// it), const-propagated like everything else.
//   world.select<Transform, quiver::maybe<Tint>>().each(
//       [](Transform& t, Tint* tint) { ... });
template <class T>
struct maybe
{
};

// OR-alternatives inside select(): an any_of<A, B, ...> element matches
// entities holding AT LEAST ONE alternative. The callback receives one
// (const-propagated) pointer per non-tag alternative, of which at least one
// is non-null per row; tag alternatives filter without contributing an
// argument. One type-level combinator, not a query language: alternatives
// are plain (possibly const) component types — no nesting, no negation.
//   world.select<Transform, quiver::any_of<Cat, Dog>>().each(
//       [](Transform& t, Cat* c, Dog* d) { /* c or d is non-null */ });
template <class... Alternatives>
struct any_of
{
};

namespace detail
{
template <class T>
struct maybe_traits
{
    static constexpr bool is_maybe = false;
    using inner = T;
};

template <class T>
struct maybe_traits<maybe<T>>
{
    static constexpr bool is_maybe = true;
    using inner = T;
};

template <class T>
inline constexpr bool is_maybe_v = maybe_traits<T>::is_maybe;

template <class T>
using maybe_inner = maybe_traits<T>::inner;

// any_of introspection: width is the number of pool slots an element
// occupies in a selection's flattened include array (1 for everything else).
template <class T>
struct any_of_traits
{
    static constexpr bool is_group = false;
    static constexpr std::size_t width = 1;
    using alts = types<T>;
};

template <class... As>
struct any_of_traits<any_of<As...>>
{
    static constexpr bool is_group = true;
    static constexpr std::size_t width = sizeof...(As);
    using alts = types<As...>;
};

template <class T>
inline constexpr bool is_any_of_v = any_of_traits<T>::is_group;

// Per-group validity, reported with named static_asserts at the use site.
template <class T>
struct group_ok : std::true_type
{
};

template <class... As>
struct group_ok<any_of<As...>> : std::true_type
{
    static_assert(sizeof...(As) >= 2, "quiver: any_of<> needs at least two alternatives");
    static_assert((component<bare<As>> && ...),
                  "quiver: any_of<> alternatives must be plain component types "
                  "(const-qualified is fine)");
    static_assert((!is_maybe_v<bare<As>> && ...) && (!is_any_of_v<bare<As>> && ...),
                  "quiver: any_of<> alternatives cannot nest maybe<> or any_of<>");
    static_assert(all_distinct<bare<As>...>, "quiver: duplicate alternative in any_of<>");
};

// The flattened inner component list one element contributes (for global
// distinctness and exclude-overlap checks).
template <class T>
struct flat_inners
{
    using type = types<bare<maybe_inner<T>>>;
};

template <class... As>
struct flat_inners<any_of<As...>>
{
    using type = types<bare<As>...>;
};

template <class List>
struct list_distinct;

template <class... Us>
struct list_distinct<types<Us...>>
{
    static constexpr bool value = all_distinct<Us...>;
};

template <class List, class... Xs>
struct list_none_among;

template <class... Us, class... Xs>
struct list_none_among<types<Us...>, Xs...>
{
    static constexpr bool value = (!type_among<Us, Xs...> && ...);
};

// The callback-argument tuple fragment contributed by one select<> element:
// nothing for tags, a (const-propagated) pointer for maybe<T>, one pointer
// per non-tag alternative for any_of<>, a reference otherwise. Shared by
// each() and the range iterator's value_type.
template <class T>
struct sel_part
{
    using type = std::conditional_t<
        is_maybe_v<T>,
        std::tuple<std::conditional_t<std::is_const_v<maybe_inner<T>>,
                                      const bare<maybe_inner<T>>*,
                                      bare<maybe_inner<T>>*>>,
        std::conditional_t<
            is_tag_v<T>,
            std::tuple<>,
            std::tuple<std::conditional_t<std::is_const_v<T>, const bare<T>&, bare<T>&>>>>;
};

template <class... As>
struct sel_part<any_of<As...>>
{
    using type = decltype(std::tuple_cat(
        std::declval<std::conditional_t<
            is_tag_v<bare<As>>,
            std::tuple<>,
            std::tuple<
                std::conditional_t<std::is_const_v<As>, const bare<As>*, bare<As>*>>>>()...));
};

template <class T>
using sel_part_t = sel_part<T>::type;

// Const-world mapping: plain types gain const, maybe wrappers gain it
// inside, any_of alternatives each gain it.
template <class T>
struct as_const_part
{
    using type = const bare<T>;
};

template <class T>
struct as_const_part<maybe<T>>
{
    using type = maybe<const bare<T>>;
};

template <class... As>
struct as_const_part<any_of<As...>>
{
    using type = any_of<const bare<As>...>;
};
}  // namespace detail

// ----------------------------------------------------------------------------
// Selections
//
// A selection is quiver's query object: a handful of raw pool pointers, cheap
// to copy, allocation-free. Build one with world::select. A selection kept
// across frames *is* the compiled query: pools are created once per world and
// never move or die before the world does, so there is no per-frame setup.
//
// Iteration drives from the smallest included pool (re-chosen every call, one
// size comparison per pool) and probes the other pools' indices per entity.
//
// Component types may be const-qualified for read-only access; a selection
// obtained from a const world is all-const. Tag components filter but are not
// passed to callbacks.
// ----------------------------------------------------------------------------

namespace detail
{
// Callback shapes accepted by each(): with or without the leading entity, and
// optionally returning something bool-like (false = stop iterating).
template <class F, class Tuple>
struct callback_traits;

template <class F, class... Refs>
struct callback_traits<F, std::tuple<Refs...>>
{
    static constexpr bool with_entity = std::invocable<F&, entity, Refs...>;
    static constexpr bool without_entity = std::invocable<F&, Refs...>;
};
}  // namespace detail

template <class Excludes, class... Ts>
class selection_t;  // primary; only the except<...> specialization exists

template <class... Ts, class... Xs>
class selection_t<except<Xs...>, Ts...>
{
    static_assert(sizeof...(Ts) > 0, "quiver: select() needs at least one component type");
    static_assert(((detail::is_any_of_v<detail::bare<detail::maybe_inner<Ts>>> ||
                    component<detail::bare<detail::maybe_inner<Ts>>>) &&
                   ...),
                  "quiver: select() component types must be plain object types "
                  "(no references, pointers-to-const, arrays, or cv-qualified types)");
    static_assert((detail::group_ok<detail::bare<detail::maybe_inner<Ts>>>::value && ...));
    static_assert((!(detail::is_maybe_v<Ts> &&
                     detail::is_any_of_v<detail::bare<detail::maybe_inner<Ts>>>) &&
                   ...),
                  "quiver: maybe<any_of<...>> is meaningless — any_of alternatives already "
                  "arrive as nullable pointers");
    static_assert((!(detail::is_maybe_v<Ts> && detail::is_tag_v<detail::maybe_inner<Ts>>) && ...),
                  "quiver: maybe<T> of a tag component carries no data to point at; tags are "
                  "filter-only — include the tag directly or use has<T>");
    static_assert(!(detail::is_maybe_v<Ts> && ...),
                  "quiver: a selection needs at least one required (non-maybe) component to "
                  "drive iteration");
    static_assert((component<Xs> && ...),
                  "quiver: except<> types must be plain, non-const component types");
    static_assert(
        detail::list_distinct<joined_t<
            typename detail::flat_inners<detail::bare<detail::maybe_inner<Ts>>>::type...>>::value,
        "quiver: duplicate component type in select<...> (any_of alternatives count)");
    static_assert(detail::all_distinct<Xs...>, "quiver: duplicate component type in except<...>");
    static_assert(
        detail::list_none_among<
            joined_t<typename detail::flat_inners<detail::bare<detail::maybe_inner<Ts>>>::type...>,
            Xs...>::value,
        "quiver: a component type appears in both select<...> and except<...> "
        "(any_of alternatives count)");

    // Per-element flags/positions, indexable at runtime alongside includes_.
    static constexpr std::array<bool, sizeof...(Ts)> optional_include{detail::is_maybe_v<Ts>...};
    static constexpr std::array<bool, sizeof...(Ts)> group_include{
        detail::is_any_of_v<detail::bare<detail::maybe_inner<Ts>>>...};
    static constexpr std::size_t group_count =
        (std::size_t{0} + ... +
         std::size_t{detail::is_any_of_v<detail::bare<detail::maybe_inner<Ts>>>});
    static constexpr bool has_plain_include =
        ((!detail::is_maybe_v<Ts> && !detail::is_any_of_v<detail::bare<detail::maybe_inner<Ts>>>) ||
         ...);

    // The flattened include array: an any_of element contributes one pool
    // slot per alternative; everything else contributes one slot. With no
    // any_of in the selection, the mapping is the identity and every loop
    // below collapses to the historical single-slot-per-element shape.
    static constexpr std::array<std::size_t, sizeof...(Ts)> element_width{
        detail::any_of_traits<detail::bare<detail::maybe_inner<Ts>>>::width...};
    static constexpr std::size_t flat_count =
        (std::size_t{0} + ... +
         detail::any_of_traits<detail::bare<detail::maybe_inner<Ts>>>::width);

    static consteval std::array<std::size_t, sizeof...(Ts)> build_offsets()
    {
        std::array<std::size_t, sizeof...(Ts)> out{};
        std::size_t at = 0;
        for (std::size_t i = 0; i < sizeof...(Ts); ++i)
        {
            out[i] = at;
            at += element_width[i];
        }
        return out;
    }

    static constexpr std::array<std::size_t, sizeof...(Ts)> flat_offset = build_offsets();

    static consteval std::size_t find_first_required()
    {
        for (std::size_t i = 0; i < sizeof...(Ts); ++i)
        {
            if (!optional_include[i] && !group_include[i])
            {
                return i;
            }
        }
        return sizeof...(Ts);  // pure-any_of selection: no plain element
    }

    static constexpr std::size_t first_required = find_first_required();

    static consteval std::size_t find_first_group()
    {
        for (std::size_t i = 0; i < sizeof...(Ts); ++i)
        {
            if (group_include[i])
            {
                return i;
            }
        }
        return sizeof...(Ts);
    }

    static constexpr std::size_t first_group = find_first_group();

    // Slot of T among Ts... (matching through const and maybe<> spellings);
    // sizeof...(Ts) when absent. Drives driven_by's static checks.
    template <class T>
    static consteval std::size_t driver_slot_of()
    {
        constexpr std::array<bool, sizeof...(Ts)> match{
            std::same_as<detail::bare<detail::maybe_inner<Ts>>, detail::bare<T>>...};
        for (std::size_t i = 0; i < match.size(); ++i)
        {
            if (match[i])
            {
                return i;
            }
        }
        return sizeof...(Ts);
    }

    // Locks every included pool for the duration of a user-callback loop.
    // Lock counters exist only in checked builds; release builds write nothing
    // here, which is what makes concurrent const iteration safe there.
    struct iteration_lock
    {
        explicit iteration_lock(const std::array<detail::pool_base*, flat_count>& pools) noexcept
            : pools_(pools)
        {
            if constexpr (checks_enabled)
            {
                for (detail::pool_base* pool : pools_)
                {
                    if (pool != nullptr)  // unregistered maybe<> pools
                    {
                        ++pool->locks_;
                    }
                }
            }
        }

        ~iteration_lock()
        {
            if constexpr (checks_enabled)
            {
                for (detail::pool_base* pool : pools_)
                {
                    if (pool != nullptr)
                    {
                        --pool->locks_;
                    }
                }
            }
        }

        iteration_lock(const iteration_lock&) = delete;
        iteration_lock& operator=(const iteration_lock&) = delete;

        const std::array<detail::pool_base*, flat_count>& pools_;
    };

public:
    // Introspection for generic code over selections: the include/exclude
    // lists as toolkit types<>, markers (const, maybe<>) preserved as
    // spelled. Pairs with for_each_type / mapped_t for system wrappers that
    // adapt to any selection.
    using included = types<Ts...>;
    using excluded = types<Xs...>;

    selection_t() = default;  // empty selection; matches nothing

    // Invokes fn for every matching entity. fn is called as fn(entity, refs...)
    // or fn(refs...) — entity-first wins when both compile. refs are the
    // non-tag components in declaration order, const where the type was const.
    // If fn returns something bool-like, returning false stops the loop.
    // O(size of smallest included pool) pool probes; no allocation.
    template <class F>
    void each(F&& fn) const
    {
        run(fn,
            [this]<class G>(G& f, entity e, std::uint32_t index)
            { return this->invoke_with_refs(f, e, index); });
    }

    // Entity-only iteration with the same matching and early-exit rules.
    template <class F>
    void entities(F&& fn) const
    {
        run(fn,
            []<class G>(G& f, entity e, std::uint32_t)
            {
                if constexpr (std::predicate<G&, entity>)
                {
                    return static_cast<bool>(f(e));
                }
                else
                {
                    static_assert(
                        std::invocable<G&, entity>,
                        "quiver: entities() callback must be callable with (quiver::entity)");
                    f(e);
                    return true;
                }
            });
    }

    // O(1) for a single included pool without excludes; otherwise walks the
    // driver pool (or the alternative union when one drives).
    [[nodiscard]] std::size_t count() const noexcept
    {
        if constexpr (group_count == 0)
        {
            const detail::pool_base* driver = smallest();
            if (driver == nullptr)
            {
                return 0;
            }
            if constexpr (sizeof...(Ts) == 1 && sizeof...(Xs) == 0)
            {
                return driver->size();
            }
            else
            {
                std::size_t n = 0;
                for (std::size_t pos = 0; pos < driver->size(); ++pos)
                {
                    n += matches_rest(driver->entity_at(pos).index(), driver) ? 1 : 0;
                }
                return n;
            }
        }
        else
        {
            std::size_t n = 0;
            entities([&](entity) { ++n; });
            return n;
        }
    }

    [[nodiscard]] bool empty() const noexcept
    {
        if constexpr (group_count == 0)
        {
            const detail::pool_base* driver = smallest();
            if (driver == nullptr || driver->size() == 0)
            {
                return true;
            }
            if constexpr (sizeof...(Ts) == 1 && sizeof...(Xs) == 0)
            {
                return false;
            }
            else
            {
                for (std::size_t pos = 0; pos < driver->size(); ++pos)
                {
                    if (matches_rest(driver->entity_at(pos).index(), driver))
                    {
                        return false;
                    }
                }
                return true;
            }
        }
        else
        {
            bool any = false;
            entities(
                [&](entity)
                {
                    any = true;
                    return false;
                });
            return !any;
        }
    }

    // True if e is alive and matches the selection.
    [[nodiscard]] bool contains(entity e) const noexcept
    {
        // Pools only ever hold live entities, and a slot index maps to at most
        // one live entity, so an exact dense match doubles as a liveness test.
        if constexpr (first_required < sizeof...(Ts))
        {
            detail::pool_base* first = includes_[flat_offset[first_required]];
            if (first == nullptr)
            {
                return false;
            }
            const std::uint32_t pos = first->position_of(e.index());
            if (pos == detail::npos32 || first->entity_at(pos) != e)
            {
                return false;
            }
            return matches_rest(e.index(), first);
        }
        else
        {
            // Pure-any_of selection: the exact dense match may sit in ANY
            // alternative of the first group.
            const std::size_t off = flat_offset[first_group];
            for (std::size_t k = 0; k < element_width[first_group]; ++k)
            {
                detail::pool_base* alt = includes_[off + k];
                if (alt == nullptr)
                {
                    continue;
                }
                const std::uint32_t pos = alt->position_of(e.index());
                if (pos != detail::npos32 && alt->entity_at(pos) == e)
                {
                    return matches_rest(e.index(), alt);
                }
            }
            return false;
        }
    }

    // The first matching entity, or no_entity — the singleton-query idiom:
    //   entity player = world.select<PlayerTag>().first();
    [[nodiscard]] entity first() const noexcept
    {
        entity result = no_entity;
        entities(
            [&](entity e)
            {
                result = e;
                return false;
            });
        return result;
    }

    // Returns a copy of the selection that drives iteration from T's pool
    // instead of the smallest required include. The override changes WHICH
    // pool walks (and therefore visit order and probe count), never the match
    // set; each(), entities(), range(), split(), count(), and first() all
    // honor it. Pair with sort<T> for ordered multi-component iteration:
    //
    //   world.sort<Order>(cmp);
    //   world.select<Order, Sprite>().driven_by<Order>().each(...);
    //
    // T must be a required (non-maybe) include, spelled with or without
    // const. A missing T pool (const world, never registered) yields the
    // empty selection, like any missing required include.
    template <class T>
    [[nodiscard]] selection_t driven_by() const noexcept
    {
        constexpr std::size_t slot = driver_slot_of<T>();
        static_assert(slot < sizeof...(Ts),
                      "quiver: driven_by<T> needs T to be one of the selection's component "
                      "types (const-qualified spelling is fine)");
        if constexpr (slot < sizeof...(Ts))
        {
            static_assert(!optional_include[slot],
                          "quiver: driven_by<T> cannot drive from a maybe<> component — "
                          "optional pools never drive iteration");
            static_assert(!group_include[slot],
                          "quiver: driven_by<T> cannot name an any_of<> group — union driving "
                          "is automatic; force a PLAIN include instead");
        }
        selection_t out = *this;
        out.driver_override_ = static_cast<std::uint8_t>(flat_offset[slot]);
        return out;
    }

    // Range-for iteration with the same matching, constness, tag, and lock
    // rules as each(). The returned object owns the iteration lock for its
    // whole lifetime (break/return/exceptions unwind it exactly like the
    // callback path) and is deliberately pinned in place — bind it directly:
    //
    //   for (auto [e, t, v] : sel.range()) { ... }
    //
    // Rows are std::tuple<entity, components&...> (maybe<> elements appear as
    // pointers, tags do not appear).
    //
    // (A template so it can hold the enclosing selection by value; always
    // instantiated as range_t via range().)
    template <class Self = selection_t>
    class basic_range
    {
    public:
        explicit basic_range(const Self& s) noexcept
            : self_(s),
              driver_(self_.smallest()),
              size_(driver_ != nullptr ? driver_->size() : 0)
        {
            if constexpr (checks_enabled)
            {
                if (driver_ != nullptr)
                {
                    for (detail::pool_base* pool : self_.includes_)
                    {
                        if (pool != nullptr)
                        {
                            ++pool->locks_;
                        }
                    }
                }
            }
        }

        ~basic_range()
        {
            if constexpr (checks_enabled)
            {
                if (driver_ != nullptr)
                {
                    for (detail::pool_base* pool : self_.includes_)
                    {
                        if (pool != nullptr)
                        {
                            --pool->locks_;
                        }
                    }
                }
            }
        }

        basic_range(const basic_range&) = delete;
        basic_range& operator=(const basic_range&) = delete;

        // One row per matching entity, materialized on dereference.
        using row = decltype(std::tuple_cat(std::declval<std::tuple<entity>>(),
                                            std::declval<detail::sel_part_t<Ts>>()...));

        class iterator
        {
        public:
            using value_type = row;
            using difference_type = std::ptrdiff_t;
            using iterator_concept = std::input_iterator_tag;

            iterator() = default;

            explicit iterator(const basic_range* owner) noexcept
                : owner_(owner)
            {
                settle();
            }

            [[nodiscard]] row operator*() const
            {
                const entity e = owner_->driver_->entity_at(pos_);
                return std::tuple_cat(std::tuple<entity>{e}, owner_->self_.value_refs(e.index()));
            }

            iterator& operator++() noexcept
            {
                ++pos_;
                settle();
                return *this;
            }

            void operator++(int) noexcept { ++*this; }

            friend bool operator==(const iterator& it, std::default_sentinel_t) noexcept
            {
                return it.at_end();
            }

        private:
            // A member (unlike the hidden friend above) may reach the
            // enclosing range's private size_.
            [[nodiscard]] bool at_end() const noexcept { return pos_ >= owner_->size_; }

            void settle() noexcept
            {
                while (pos_ < owner_->size_ &&
                       !owner_->self_.matches_rest(owner_->driver_->entity_at(pos_).index(),
                                                   owner_->driver_))
                {
                    ++pos_;
                }
            }

            const basic_range* owner_ = nullptr;
            std::size_t pos_ = 0;
        };

        [[nodiscard]] iterator begin() const noexcept { return iterator(this); }
        [[nodiscard]] static std::default_sentinel_t end() noexcept { return {}; }

    private:
        Self self_;  // cheap pointer bundle; keeps the range self-contained
        detail::pool_base* driver_ = nullptr;
        std::size_t size_ = 0;  // sound to cache: locked pools cannot change size
    };

    using range_t = basic_range<>;  // nameable for stored ranges; see range()

    [[nodiscard]] auto range() const noexcept
    {
        static_assert(group_count == 0 || has_plain_include,
                      "quiver: range() needs at least one plain include to drive; iterate "
                      "pure any_of selections with each()/entities()");
        return basic_range<>(*this);
    }

    // ------------------------------------------------------------- splitting
    //
    // split(n) carves the driver pool's dense order into n near-equal slices
    // for parallel const iteration. The split object owns the iteration locks
    // (taken and released on the constructing thread — lock counters are not
    // atomic); the parts are passive value handles a worker can copy. Parts
    // are views INTO the split: the split object must outlive every part.
    //
    //   auto work = sel.split(threads.size());
    //   parallel-for i: work.part(i).each([](Transform& t, const Velocity& v) { ... });
    //
    // Worker contract: only write through the references handed to the
    // callback (parts never share an entity, so they never share a component
    // object); no structural verbs, no spawning, no apply() — record into
    // per-worker command buffers and apply after joining. A violation raised
    // on a worker reaches the process-global handler, which must then be
    // thread-safe.

    template <class Self = selection_t>
    class basic_split;

    class part_t
    {
    public:
        part_t() = default;  // empty part

        template <class F>
        void each(F&& fn) const
        {
            if (owner_self_ == nullptr)
            {
                return;
            }
            const selection_t& s = *owner_self_;
            s.run_span(
                fn,
                [&s]<class G>(G& f, entity e, std::uint32_t index)
                { return s.invoke_with_refs(f, e, index); },
                driver_,
                begin_,
                end_);
        }

        template <class F>
        void entities(F&& fn) const
        {
            if (owner_self_ == nullptr)
            {
                return;
            }
            owner_self_->run_span(
                fn,
                []<class G>(G& f, entity e, std::uint32_t)
                {
                    if constexpr (std::predicate<G&, entity>)
                    {
                        return static_cast<bool>(f(e));
                    }
                    else
                    {
                        static_assert(std::invocable<G&, entity>,
                                      "quiver: entities() callback must be callable with "
                                      "(quiver::entity)");
                        f(e);
                        return true;
                    }
                },
                driver_,
                begin_,
                end_);
        }

    private:
        template <class Self>
        friend class basic_split;

        part_t(const selection_t* self,
               detail::pool_base* driver,
               std::size_t begin,
               std::size_t end) noexcept
            : owner_self_(self),
              driver_(driver),
              begin_(begin),
              end_(end)
        {
        }

        const selection_t* owner_self_ = nullptr;
        detail::pool_base* driver_ = nullptr;
        std::size_t begin_ = 0;
        std::size_t end_ = 0;
    };

    template <class Self>
    class basic_split
    {
    public:
        basic_split(const Self& s, std::size_t parts) noexcept
            : self_(s),
              driver_(self_.smallest()),
              lock_(self_.includes_),
              size_(driver_ != nullptr ? driver_->size() : 0),
              parts_(parts == 0 ? 1 : parts)
        {
        }

        basic_split(const basic_split&) = delete;  // pinned: parts point at it
        basic_split& operator=(const basic_split&) = delete;

        [[nodiscard]] std::size_t parts() const noexcept { return parts_; }

        [[nodiscard]] part_t part(std::size_t i) const noexcept
        {
            if (i >= parts_ || size_ == 0)
            {
                return {};
            }
            return part_t(&self_, driver_, (i * size_) / parts_, ((i + 1) * size_) / parts_);
        }

    private:
        Self self_;
        detail::pool_base* driver_;
        iteration_lock lock_;  // references self_.includes_; member order matters
        std::size_t size_;
        std::size_t parts_;
    };

    [[nodiscard]] auto split(std::size_t parts) const noexcept
    {
        static_assert(group_count == 0 || has_plain_include,
                      "quiver: split() needs at least one plain include to carve; iterate "
                      "pure any_of selections with each()/entities()");
        return basic_split<selection_t>(*this, parts);
    }

private:
    friend class world;
    friend struct test_access;

    explicit selection_t(std::array<detail::pool_base*, flat_count> includes,
                         std::array<detail::pool_base*, sizeof...(Xs)> excludes) noexcept
        : includes_(includes),
          excludes_(excludes)
    {
    }

    // The PLAIN driver pool: the smallest required non-group include — or
    // the driven_by<T> choice when set; null when a plain include pool is
    // missing (const world) or when the selection has no plain includes at
    // all (pure any_of: the union drives instead, in run()). maybe<> pools
    // never drive and a missing one never disqualifies the selection.
    [[nodiscard]] detail::pool_base* smallest() const noexcept
    {
        if (driver_override_ != no_driver_override)
        {
            return includes_[driver_override_];  // null = empty selection
        }
        detail::pool_base* best = nullptr;
        for (std::size_t i = 0; i < sizeof...(Ts); ++i)
        {
            if (optional_include[i] || group_include[i])
            {
                continue;
            }
            detail::pool_base* pool = includes_[flat_offset[i]];
            if (pool == nullptr)
            {
                return nullptr;
            }
            if (best == nullptr || pool->size() < best->size())
            {
                best = pool;
            }
        }
        return best;
    }

    [[nodiscard]] bool matches_rest(std::uint32_t index,
                                    const detail::pool_base* driver) const noexcept
    {
        return matches_from(index, driver, sizeof...(Ts));
    }

    // The full predicate, optionally skipping one element (the union-driving
    // group satisfies itself by construction).
    [[nodiscard]] bool matches_from(std::uint32_t index,
                                    const detail::pool_base* driver,
                                    std::size_t skip_element) const noexcept
    {
        for (std::size_t i = 0; i < sizeof...(Ts); ++i)
        {
            if (optional_include[i] || i == skip_element)
            {
                continue;  // maybe<> never filters
            }
            if constexpr (group_count > 0)
            {
                if (group_include[i])
                {
                    bool satisfied = false;
                    const std::size_t off = flat_offset[i];
                    for (std::size_t k = 0; k < element_width[i]; ++k)
                    {
                        detail::pool_base* alt = includes_[off + k];
                        if (alt != nullptr && (alt == driver || alt->contains(index)))
                        {
                            satisfied = true;
                            break;
                        }
                    }
                    if (!satisfied)
                    {
                        return false;
                    }
                    continue;
                }
            }
            detail::pool_base* pool = includes_[flat_offset[i]];
            if (pool != driver && (pool == nullptr || !pool->contains(index)))
            {
                return false;
            }
        }
        for (detail::pool_base* pool : excludes_)
        {
            if (pool != nullptr && pool->contains(index))
            {
                return false;
            }
        }
        return true;
    }

    template <class F, class Invoke>
    void run(F& fn, Invoke&& invoke) const
    {
        if constexpr (group_count == 0)
        {
            detail::pool_base* driver = smallest();
            if (driver == nullptr)
            {
                return;
            }
            const iteration_lock lock(includes_);
            // Locked pools cannot change size, so a forward walk visits every
            // matching entity exactly once.
            run_span(fn, invoke, driver, 0, driver->size());
        }
        else
        {
            // A missing PLAIN include pool is an empty selection, as ever.
            detail::pool_base* driver = smallest();
            if constexpr (has_plain_include)
            {
                if (driver == nullptr)
                {
                    return;
                }
            }
            // The cheapest union among the groups (a group whose every
            // alternative pool is missing or empty matches nothing).
            std::size_t best_union = static_cast<std::size_t>(-1);
            std::size_t best_group_elem = sizeof...(Ts);
            for (std::size_t i = 0; i < sizeof...(Ts); ++i)
            {
                if (!group_include[i])
                {
                    continue;
                }
                std::size_t sum = 0;
                const std::size_t off = flat_offset[i];
                for (std::size_t k = 0; k < element_width[i]; ++k)
                {
                    sum += includes_[off + k] != nullptr ? includes_[off + k]->size() : 0;
                }
                if (sum == 0)
                {
                    return;  // unsatisfiable group: nothing matches
                }
                if (sum < best_union)
                {
                    best_union = sum;
                    best_group_elem = i;
                }
            }
            // Pick the cheaper walk: the plain driver or the smallest union
            // (a driven_by override always forces the plain pool).
            if (driver != nullptr &&
                (driver_override_ != no_driver_override || driver->size() <= best_union))
            {
                const iteration_lock lock(includes_);
                run_span(fn, invoke, driver, 0, driver->size());
                return;
            }
            // Union drive: walk each alternative's dense array; an entity
            // already seen through an EARLIER alternative is skipped (O(1)
            // sparse probes — exact dedup, since pools hold each live entity
            // at most once).
            const iteration_lock lock(includes_);
            const std::size_t off = flat_offset[best_group_elem];
            for (std::size_t k = 0; k < element_width[best_group_elem]; ++k)
            {
                detail::pool_base* alt = includes_[off + k];
                if (alt == nullptr)
                {
                    continue;
                }
                for (std::size_t pos = 0; pos < alt->size(); ++pos)
                {
                    const entity e = alt->entity_at(pos);
                    bool seen_earlier = false;
                    for (std::size_t j = 0; j < k; ++j)
                    {
                        detail::pool_base* prev = includes_[off + j];
                        if (prev != nullptr && prev->contains(e.index()))
                        {
                            seen_earlier = true;
                            break;
                        }
                    }
                    if (seen_earlier || !matches_from(e.index(), nullptr, best_group_elem))
                    {
                        continue;
                    }
                    if (!invoke(fn, e, e.index()))
                    {
                        return;
                    }
                }
            }
        }
    }

    // The bounded loop body of run(); split parts call it with the umbrella
    // object already holding the locks.
    template <class F, class Invoke>
    void run_span(
        F& fn, Invoke&& invoke, detail::pool_base* driver, std::size_t begin, std::size_t end) const
    {
        for (std::size_t pos = begin; pos < end; ++pos)
        {
            const entity e = driver->entity_at(pos);
            if (!matches_rest(e.index(), driver))
            {
                continue;
            }
            if (!invoke(fn, e, e.index()))
            {
                break;
            }
        }
    }

    // Must produce exactly detail::sel_part_t<T> so the range iterator's
    // value_type stays in lockstep with each()'s callback arguments.
    template <std::size_t I, class T>
    [[nodiscard]] detail::sel_part_t<T> ref_tuple(std::uint32_t index) const
    {
        if constexpr (detail::is_any_of_v<detail::bare<detail::maybe_inner<T>>>)
        {
            return group_parts<detail::bare<detail::maybe_inner<T>>>(flat_offset[I], index);
        }
        else if constexpr (detail::is_maybe_v<T>)
        {
            using inner = detail::bare<detail::maybe_inner<T>>;
            auto* pool = static_cast<pool_of_t<inner>*>(includes_[flat_offset[I]]);
            return detail::sel_part_t<T>{pool == nullptr ? nullptr : pool->at(index)};
        }
        else if constexpr (detail::is_tag_v<T>)
        {
            return std::tuple<>{};
        }
        else
        {
            auto* pool = static_cast<pool_of_t<detail::bare<T>>*>(includes_[flat_offset[I]]);
            return detail::sel_part_t<T>{*pool->at(index)};
        }
    }

    // One pointer per non-tag alternative, each fetched independently (the
    // matching machinery already guaranteed at least one is non-null).
    template <class Group>
    [[nodiscard]] detail::sel_part_t<Group> group_parts(std::size_t off, std::uint32_t index) const
    {
        return [&]<std::size_t... Ks, class... As>(std::index_sequence<Ks...>, types<As...>)
        { return std::tuple_cat(this->template alternative_part<As>(off + Ks, index)...); }(
            std::make_index_sequence<detail::any_of_traits<Group>::width>{},
            typename detail::any_of_traits<Group>::alts{});
    }

    template <class A>
    [[nodiscard]] auto alternative_part(std::size_t slot, std::uint32_t index) const
    {
        if constexpr (detail::is_tag_v<detail::bare<A>>)
        {
            return std::tuple<>{};
        }
        else
        {
            using pointer =
                std::conditional_t<std::is_const_v<A>, const detail::bare<A>*, detail::bare<A>*>;
            auto* pool = static_cast<pool_of_t<detail::bare<A>>*>(includes_[slot]);
            return std::tuple<pointer>(pool == nullptr ? nullptr : pool->at(index));
        }
    }

    [[nodiscard]] auto value_refs(std::uint32_t index) const
    {
        return [this, index]<std::size_t... Is>(std::index_sequence<Is...>)
        {
            return std::tuple_cat(this->template ref_tuple<Is, Ts>(index)...);
        }(std::index_sequence_for<Ts...>{});
    }

    template <class F>
    bool invoke_with_refs(F& fn, entity e, std::uint32_t index) const
    {
        auto refs = value_refs(index);
        using traits = detail::callback_traits<F, decltype(refs)>;
        if constexpr (traits::with_entity)
        {
            return finish(fn, std::tuple_cat(std::tuple<entity>{e}, std::move(refs)));
        }
        else if constexpr (traits::without_entity)
        {
            return finish(fn, std::move(refs));
        }
        else
        {
            // Terminal branch: nothing below instantiates, so this message is
            // the only error the user sees.
            static_assert(traits::with_entity || traits::without_entity,
                          "quiver: each() callback must be callable as (entity, components&...) "
                          "or (components&...). Components from a const world or marked const in "
                          "select<...> are passed as const&. Tag components are filter-only and "
                          "are not passed at all.");
            return false;
        }
    }

    template <class F, class Args>
    static bool finish(F& fn, Args&& args)
    {
        using result = decltype(std::apply(fn, std::forward<Args>(args)));
        if constexpr (std::convertible_to<result, bool>)
        {
            return static_cast<bool>(std::apply(fn, std::forward<Args>(args)));
        }
        else
        {
            std::apply(fn, std::forward<Args>(args));
            return true;
        }
    }

    static constexpr std::uint8_t no_driver_override = 0xFF;
    static_assert(flat_count < no_driver_override,
                  "quiver: selections cap at 254 flattened include slots");

    std::array<detail::pool_base*, flat_count> includes_{};
    std::array<detail::pool_base*, sizeof...(Xs)> excludes_{};
    std::uint8_t driver_override_ = no_driver_override;
};

namespace detail
{
template <class T>
struct is_except : std::false_type
{
};

template <class... Xs>
struct is_except<except<Xs...>> : std::true_type
{
};
}  // namespace detail

// The public spelling: selection<A, const B>, or selection<except<C>, A, B>
// when an exclude filter is part of the type. world::select deduces this for
// you; spell it out only when storing a selection as a member.
template <class First, class... Rest>
using selection = std::conditional_t<detail::is_except<First>::value,
                                     selection_t<First, Rest...>,
                                     selection_t<except<>, First, Rest...>>;

// ----------------------------------------------------------------------------
// Bonded pools — mirrored partitions, 2..N owners
//
// world.bond<Ts...>() keeps the N-way intersection mirror-partitioned at the
// front of EVERY owner pool — the same entity at the same dense position in
// all of them — and the world maintains the partition with at most one O(1)
// mirrored swap per owner on every add/remove. bonded<Ts...>() then iterates
// the intersection as N parallel arrays: zero sparse probes, the layout
// EnTT-style owning groups buy, without claiming any pool's order beyond the
// partition or coupling pools outside the owned set.
//
//   world.bond<Transform, Velocity>();                  // the common pair
//   world.bond<Transform, Velocity, Burning>();         // or any set up to 8
//   world.bonded<Transform, Velocity>().each([](Transform& t, Velocity& v) { ... });
//
// The price (everything but the co-owner-lock refusals holds in EVERY build):
// one bond per pool — owned sets may not overlap or nest (that conflict
// matrix is exactly why owning groups were long rejected here; one partition
// owner per pool keeps every refusal explainable in one sentence). Structural
// changes on an owned pool are also refused while ANY co-owner iterates
// (checked builds); a hook on one owner may not structurally touch a
// co-owner (use a command buffer); sort<T> and sort_along<T, ...> on owned
// pools are refused outright. Tag pools may be owned — a bonded tag is "fast
// filtered iteration" of the other components.
// ----------------------------------------------------------------------------

namespace detail
{
// Invocability over a tuple's element types, checked WITHOUT naming
// std::apply (whose noexcept specification hard-errors outside the immediate
// context on some standard libraries).
template <class F, class Tuple>
inline constexpr bool row_invocable = false;
template <class F, class... Es>
inline constexpr bool row_invocable<F, std::tuple<Es...>> = std::invocable<F&, Es...>;

template <class F, class Tuple>
inline constexpr bool row_stops = false;  // bool-returning callback = early exit
template <class F, class... Es>
    requires std::invocable<F&, Es...>
inline constexpr bool row_stops<F, std::tuple<Es...>> =
    std::same_as<std::invoke_result_t<F&, Es...>, bool>;
}  // namespace detail

template <class Excludes, class... Ts>
class bonded_view_t;  // primary; only the except<...> specialization exists

template <class... Ts, class... Xs>  // const-qualify elements for read-only payload access
class bonded_view_t<except<Xs...>, Ts...>
{
    static_assert(sizeof...(Ts) >= 2, "quiver: a bonded view spans at least two pools");
    static_assert((!detail::is_any_of_v<detail::bare<detail::maybe_inner<Ts>>> && ...),
                  "quiver: bonds own pools, not alternatives — any_of<> belongs in select");
    static_assert((component<detail::bare<detail::maybe_inner<Ts>>> && ...),
                  "quiver: bonded view component types must be plain object types");
    static_assert((!(detail::is_maybe_v<Ts> && detail::is_tag_v<detail::maybe_inner<Ts>>) && ...),
                  "quiver: maybe<T> of a tag component carries no data to point at; probe "
                  "has<T> in the callback body instead");
    static_assert(detail::all_distinct<detail::maybe_inner<Ts>...>,
                  "quiver: duplicate component type in bonded<...>");
    static_assert((component<Xs> && ...),
                  "quiver: except<> types must be plain, non-const component types");
    static_assert(detail::all_distinct<Xs...>, "quiver: duplicate component type in except<...>");
    static_assert((!detail::type_among<detail::maybe_inner<Ts>, Xs...> && ...),
                  "quiver: a component type appears in both bonded<...> and except<...>");

    // Plain-listed types are the OWNED set (the partition's identity);
    // maybe<>-marked types are OBSERVED — probed per row, pointer parts,
    // null when absent, never part of the group identity.
    static constexpr std::array<bool, sizeof...(Ts)> observed_part{detail::is_maybe_v<Ts>...};

    static consteval std::size_t find_first_owned()
    {
        for (std::size_t i = 0; i < sizeof...(Ts); ++i)
        {
            if (!observed_part[i])
            {
                return i;
            }
        }
        return 0;  // unreachable: the owned-count static_assert below
    }

    static constexpr std::size_t first_owned = find_first_owned();
    static constexpr std::size_t owned_count =
        sizeof...(Ts) - (std::size_t{0} + ... + std::size_t{detail::is_maybe_v<Ts>});
    static_assert(owned_count >= 2,
                  "quiver: a bonded view lists the standing owned set plain (at least two "
                  "non-maybe types); observed extras are maybe<>");

    // Tags contribute nothing to callback arguments and rows; observed parts
    // probe by entity INDEX (their pools do not share the partition layout).
    template <class T>
    [[nodiscard]] auto part_at(std::size_t slot,
                               std::uint32_t pos,
                               std::uint32_t index) const noexcept
    {
        if constexpr (detail::is_maybe_v<T>)
        {
            using inner = detail::bare<detail::maybe_inner<T>>;
            using pointer =
                std::conditional_t<std::is_const_v<detail::maybe_inner<T>>, const inner*, inner*>;
            auto* pool = static_cast<pool_of_t<inner>*>(bases_[slot]);
            return std::tuple<pointer>(pool == nullptr ? nullptr : pool->at(index));
        }
        else if constexpr (detail::is_tag_v<detail::bare<T>>)
        {
            (void)pos;
            return std::tuple<>{};
        }
        else
        {
            auto* pool = static_cast<pool_of_t<detail::bare<T>>*>(bases_[slot]);
            if constexpr (std::is_const_v<T>)
            {
                return std::tuple<const detail::bare<T>&>(std::as_const(*pool).at_pos(pos));
            }
            else
            {
                return std::tuple<detail::bare<T>&>(pool->at_pos(pos));
            }
        }
    }

    // One row's payload parts: owners in lockstep at the partition position,
    // observed pools probed by the row entity's index.
    [[nodiscard]] auto row_parts(std::uint32_t pos, std::uint32_t index) const noexcept
    {
        return [this, pos, index]<std::size_t... Is>(std::index_sequence<Is...>)
        {
            return std::tuple_cat(this->template part_at<Ts>(Is, pos, index)...);
        }(std::index_sequence_for<Ts...>{});
    }

    // Row filter: every except<> pool must lack the entity. Null pools
    // (never registered) exclude nothing.
    [[nodiscard]] bool passes(std::uint32_t index) const noexcept
    {
        for (detail::pool_base* pool : excludes_)
        {
            if (pool != nullptr && pool->contains(index))
            {
                return false;
            }
        }
        return true;
    }

    // Locks every owner pool for a user-callback loop; tolerates null pools
    // (the default-constructed empty view). Holds the array by value so
    // range objects can own a copy of the view safely.
    struct view_locks
    {
        explicit view_locks(const std::array<detail::pool_base*, sizeof...(Ts)>& pools) noexcept
            : pools_(pools)
        {
            if constexpr (checks_enabled)
            {
                for (detail::pool_base* pool : pools_)
                {
                    if (pool != nullptr)
                    {
                        ++pool->locks_;
                    }
                }
            }
        }

        ~view_locks()
        {
            if constexpr (checks_enabled)
            {
                for (detail::pool_base* pool : pools_)
                {
                    if (pool != nullptr)
                    {
                        --pool->locks_;
                    }
                }
            }
        }

        view_locks(const view_locks&) = delete;
        view_locks& operator=(const view_locks&) = delete;

        std::array<detail::pool_base*, sizeof...(Ts)> pools_;
    };

public:
    // Introspection, as spelled (const and maybe<> markers preserved):
    // `owned` is the plain-listed partition identity, `parts` the full list.
    using parts = types<Ts...>;
    using owned = joined_t<std::conditional_t<detail::is_maybe_v<Ts>, types<>, types<Ts>>...>;
    using excluded = types<Xs...>;
    using pair = owned;  // the historical 2-ary name, kept for manifests

    bonded_view_t() = default;  // empty: matches nothing

    // O(1) without excludes; with excludes the partition is walked and
    // filtered (observed parts never filter).
    [[nodiscard]] std::size_t count() const noexcept
    {
        if (bond_ == nullptr)
        {
            return 0;
        }
        if constexpr (sizeof...(Xs) == 0)
        {
            return bond_->paired;
        }
        else
        {
            std::size_t n = 0;
            for (std::uint32_t pos = 0; pos < bond_->paired; ++pos)
            {
                n += passes(bases_[first_owned]->entity_at(pos).index()) ? 1 : 0;
            }
            return n;
        }
    }

    [[nodiscard]] bool empty() const noexcept { return count() == 0; }

    [[nodiscard]] entity entity_at(std::size_t pos) const noexcept
    {
        return bases_[first_owned]->entity_at(pos);
    }

    [[nodiscard]] bool contains(entity e) const noexcept
    {
        if (bond_ == nullptr || bond_->paired == 0)
        {
            return false;
        }
        const std::uint32_t pos = bases_[first_owned]->position_of(e.index());
        return pos < bond_->paired && bases_[first_owned]->entity_at(pos) == e && passes(e.index());
    }

    // fn(entity, comps&...) or fn(comps&...); tags are filter-only; observed
    // maybe<> parts arrive as pointers (null when absent); bool return stops
    // the walk. Entity-first wins when both shapes bind. Locks EVERY listed
    // pool (mutation rules apply to all of them).
    template <class F>
    void each(F&& fn) const
    {
        using parts_row = decltype(std::declval<const bonded_view_t&>().row_parts(0, 0));
        using full_row =
            decltype(std::tuple_cat(std::declval<std::tuple<entity>>(), std::declval<parts_row>()));
        constexpr bool with_entity = detail::row_invocable<F, full_row>;
        static_assert(with_entity || detail::row_invocable<F, parts_row>,
                      "quiver: bonded callback must take (entity, comps&...) or (comps&...); "
                      "tags contribute no argument, observed maybe<T> parts arrive as T*");
        if (bond_ == nullptr || bond_->paired == 0)
        {
            return;
        }
        const view_locks locks(bases_);
        const std::uint32_t n = bond_->paired;
        for (std::uint32_t pos = 0; pos < n; ++pos)
        {
            const entity e = bases_[first_owned]->entity_at(pos);
            if (!passes(e.index()))
            {
                continue;
            }
            if constexpr (with_entity)
            {
                const auto row = std::tuple_cat(std::tuple<entity>(e), row_parts(pos, e.index()));
                if constexpr (detail::row_stops<F, full_row>)
                {
                    if (!std::apply(fn, row))
                    {
                        break;
                    }
                }
                else
                {
                    std::apply(fn, row);
                }
            }
            else
            {
                const auto row = row_parts(pos, e.index());
                if constexpr (detail::row_stops<F, parts_row>)
                {
                    if (!std::apply(fn, row))
                    {
                        break;
                    }
                }
                else
                {
                    std::apply(fn, row);
                }
            }
        }
    }

    // fn(entity) over the (filtered) intersection; bool return stops. A
    // direct walk: works for every part shape, including all-value rows.
    template <class F>
    void entities(F&& fn) const
    {
        if (bond_ == nullptr || bond_->paired == 0)
        {
            return;
        }
        const view_locks locks(bases_);
        const std::uint32_t n = bond_->paired;
        for (std::uint32_t pos = 0; pos < n; ++pos)
        {
            const entity e = bases_[first_owned]->entity_at(pos);
            if (!passes(e.index()))
            {
                continue;
            }
            if constexpr (std::predicate<F&, entity>)
            {
                if (!fn(e))
                {
                    break;
                }
            }
            else
            {
                fn(e);
            }
        }
    }

    // Reorders the partition itself — mirrored into EVERY owner, visible to
    // every view of this bond and to the owners' dense prefixes. cmp compares
    // entities (entity, entity); the sort<T> form compares an OWNED
    // component's values. The optional algorithm follows world::sort's
    // contract. Packed owners swap payloads (outstanding T* into the
    // partition die); stable owners permute bookkeeping only. Refused while
    // any listed pool iterates; a tombstoned or empty view is a no-op.
    template <class Compare, class Algorithm = detail::std_sort>
        requires std::invocable<Compare&, entity, entity>
    void sort(Compare cmp, Algorithm&& algo = {})
    {
        sort_partition(
            [&](std::uint32_t a, std::uint32_t b)
            {
                return static_cast<bool>(
                    cmp(bases_[first_owned]->entity_at(a), bases_[first_owned]->entity_at(b)));
            },
            std::forward<Algorithm>(algo));
    }

    template <class T, class Compare, class Algorithm = detail::std_sort>
    void sort(Compare cmp, Algorithm&& algo = {})
    {
        constexpr std::size_t slot = sort_slot_of<T>();
        static_assert(slot < sizeof...(Ts),
                      "quiver: bonded sort<T> needs T to be one of the view's OWNED component "
                      "types (observed maybe<> parts have no partition order)");
        if constexpr (slot < sizeof...(Ts))
        {
            static_assert(!observed_part[slot],
                          "quiver: bonded sort<T> cannot order by an observed maybe<> part");
            static_assert(!detail::is_tag_v<detail::bare<T>>,
                          "quiver: T uses tag storage and carries no values to compare; sort by "
                          "(entity, entity) instead");
            auto* pool = static_cast<pool_of_t<detail::bare<T>>*>(bases_[slot]);
            sort_partition([&](std::uint32_t a, std::uint32_t b)
                           { return static_cast<bool>(cmp(pool->at_pos(a), pool->at_pos(b))); },
                           std::forward<Algorithm>(algo));
        }
    }

    // Lock-owning range for structured bindings; pinned like selection ranges.
    template <class Self = bonded_view_t>
    class basic_range
    {
    public:
        using row = decltype(std::tuple_cat(std::declval<std::tuple<entity>>(),
                                            std::declval<const Self&>().row_parts(0, 0)));

        explicit basic_range(const Self& view) noexcept
            : view_(view),
              locks_(view_.bases_),
              size_(view_.bond_ != nullptr ? view_.bond_->paired : 0)
        {
        }

        basic_range(const basic_range&) = delete;
        basic_range& operator=(const basic_range&) = delete;

        class iterator
        {
        public:
            using value_type = row;
            using difference_type = std::ptrdiff_t;

            iterator() = default;

            iterator(const Self* view, std::uint32_t pos) noexcept
                : view_(view),
                  pos_(pos)
            {
            }

            [[nodiscard]] row operator*() const
            {
                const entity e = view_->bases_[Self::first_owned]->entity_at(pos_);
                return std::tuple_cat(std::tuple<entity>(e), view_->row_parts(pos_, e.index()));
            }

            iterator& operator++() noexcept
            {
                ++pos_;
                seek();
                return *this;
            }

            void operator++(int) noexcept
            {
                ++pos_;
                seek();
            }

            [[nodiscard]] friend bool operator==(const iterator& it,
                                                 std::default_sentinel_t) noexcept
            {
                return it.pos_ >= it.end_;
            }

        private:
            friend class basic_range;

            // Advance past rows the excludes filter out.
            void seek() noexcept
            {
                if constexpr (sizeof...(Xs) > 0)
                {
                    while (
                        pos_ < end_ &&
                        !view_->passes(view_->bases_[Self::first_owned]->entity_at(pos_).index()))
                    {
                        ++pos_;
                    }
                }
            }

            const Self* view_ = nullptr;
            std::uint32_t pos_ = 0;
            std::uint32_t end_ = 0;
        };

        [[nodiscard]] iterator begin() const noexcept
        {
            iterator it(&view_, 0);
            it.end_ = static_cast<std::uint32_t>(size_);
            it.seek();
            return it;
        }

        [[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }

    private:
        Self view_;
        view_locks locks_;
        std::size_t size_;
    };

    [[nodiscard]] auto range() const noexcept { return basic_range<>(*this); }

private:
    friend class world;
    template <class Excludes2, class... Us>
    friend class bonded_view_t;

    // Slot of T among the listed types (matching through const), for sort<T>.
    template <class T>
    static consteval std::size_t sort_slot_of()
    {
        constexpr std::array<bool, sizeof...(Ts)> match{
            std::same_as<detail::bare<detail::maybe_inner<Ts>>, detail::bare<T>>...};
        for (std::size_t i = 0; i < match.size(); ++i)
        {
            if (match[i])
            {
                return i;
            }
        }
        return sizeof...(Ts);
    }

    // The shared sort engine: gather-permute [0, paired) and mirror every
    // swap into every OWNER pool (observed pools are not partition members).
    template <class PosCompare, class Algorithm>
    void sort_partition(PosCompare cmp, Algorithm&& algo)
    {
        static_assert((!std::is_const_v<detail::maybe_inner<Ts>> && ...),
                      "quiver: sort on an all-const bonded view (const world) — sorting "
                      "reorders the owners' payloads");
        if (bond_ == nullptr || bond_->paired < 2)
        {
            return;
        }
        if constexpr (checks_enabled)
        {
            for (detail::pool_base* pool : bases_)
            {
                if (pool != nullptr && pool->locked())
                {
                    detail::violate_pool("bonded sort during iteration over pool", pool->name());
                    return;
                }
            }
        }
        const std::uint32_t n = bond_->paired;
        std::pmr::vector<std::uint32_t> perm(n, bases_[first_owned]->memory_);
        for (std::uint32_t i = 0; i < n; ++i)
        {
            perm[i] = i;
        }
        algo(perm.begin(), perm.end(), cmp);  // gather: perm[new] = old position
        const auto swap_all = [&](std::uint32_t a, std::uint32_t b) noexcept
        {
            for (std::size_t slot = 0; slot < sizeof...(Ts); ++slot)
            {
                if (!observed_part[slot])
                {
                    bases_[slot]->mirror_swap(a, b);
                }
            }
        };
        for (std::uint32_t i = 0; i < n; ++i)
        {
            std::uint32_t curr = i;
            std::uint32_t next = perm[curr];
            while (next != i)
            {
                swap_all(curr, next);
                perm[curr] = curr;
                curr = next;
                next = perm[curr];
            }
            perm[curr] = curr;
        }
    }

    bonded_view_t(std::array<detail::pool_base*, sizeof...(Ts)> pools,
                  std::array<detail::pool_base*, sizeof...(Xs)> excludes,
                  const detail::group_core* bond) noexcept
        : bases_(pools),
          excludes_(excludes),
          bond_(bond)
    {
    }

    std::array<detail::pool_base*, sizeof...(Ts)> bases_{};
    std::array<detail::pool_base*, sizeof...(Xs)> excludes_{};
    const detail::group_core* bond_ = nullptr;
};

// The public spelling: bonded_view<A, B>, or bonded_view<except<C>, A, B>
// when an exclude filter is part of the type. world::bonded deduces this for
// you; spell it out only when storing a view as a member.
template <class First, class... Rest>
using bonded_view = std::conditional_t<detail::is_except<First>::value,
                                       bonded_view_t<First, Rest...>,
                                       bonded_view_t<except<>, First, Rest...>>;

// ----------------------------------------------------------------------------
// Runtime queries
//
// The type-erased layer for editors, debug consoles, and scripting bindings:
// pools are addressed by runtime ids (or stable name hashes) instead of
// compile-time types, and matching entities are enumerated by id only —
// payload access stays compile-time on purpose (tooling wants entity lists
// and pool_info rows, which it already has).
//
//   quiver::runtime_selection q;
//   q.include(world.find_pool_by_hash(saved_hash));
//   q.exclude(world.find_pool<Hidden>());
//   q.entities([&](quiver::entity e) { overlay.row(e); });
// ----------------------------------------------------------------------------

// A nullable, opaque view of one component pool.
class pool_ref
{
public:
    pool_ref() = default;

    [[nodiscard]] explicit operator bool() const noexcept { return pool_ != nullptr; }
    [[nodiscard]] std::string_view name() const noexcept
    {
        return pool_ != nullptr ? pool_->name() : std::string_view{};
    }
    [[nodiscard]] std::uint64_t name_hash() const noexcept
    {
        return pool_ != nullptr ? pool_->name_hash() : 0;
    }
    [[nodiscard]] std::size_t size() const noexcept { return pool_ != nullptr ? pool_->size() : 0; }

    [[nodiscard]] pool_info info() const noexcept
    {
        return pool_ != nullptr ? pool_->info() : pool_info{};
    }

    // Exact-handle membership (stale handles answer false). O(1).
    [[nodiscard]] bool contains(entity e) const noexcept
    {
        if (pool_ == nullptr)
        {
            return false;
        }
        const std::uint32_t pos = pool_->position_of(e.index());
        return pos != detail::npos32 && pool_->entity_at(pos) == e;
    }

    // Type-erased payload bytes of e, or null (absent entity, stale handle,
    // or a tag pool). Pair with info().bytes_per_item; the pointer obeys the
    // pool's component-pointer invalidation rules. Writable exactly when the
    // world this pool_ref came from was writable — treat it as read-only
    // otherwise. This is what generic editors and byte-level serializers
    // build on. O(1).
    [[nodiscard]] void* raw(entity e) const noexcept
    {
        if (pool_ == nullptr)
        {
            return nullptr;
        }
        const std::uint32_t pos = pool_->position_of(e.index());
        if (pos == detail::npos32 || pool_->entity_at(pos) != e)
        {
            return nullptr;
        }
        // The runtime layer stores const pointers for const-world
        // compatibility; mutability is the caller's documented contract.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        return const_cast<detail::pool_base*>(pool_)->item_address(pos);
    }

    // The same, by dense position (0 <= pos < size()): walk a whole pool
    // type-erased via entity_at + raw_at.
    [[nodiscard]] void* raw_at(std::size_t pos) const noexcept
    {
        if (pool_ == nullptr || pos >= pool_->size())
        {
            return nullptr;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        return const_cast<detail::pool_base*>(pool_)->item_address(static_cast<std::uint32_t>(pos));
    }

    // The entity at a dense position (pairs with raw_at).
    [[nodiscard]] entity entity_at(std::size_t pos) const noexcept
    {
        return pool_ != nullptr && pos < pool_->size() ? pool_->entity_at(pos) : no_entity;
    }

private:
    friend class world;
    friend class runtime_selection;

    explicit pool_ref(const detail::pool_base* pool) noexcept
        : pool_(pool)
    {
    }

    const detail::pool_base* pool_ = nullptr;
};

// A selection assembled at runtime from pool_refs. Entity-only iteration with
// the same smallest-pool driving, matching, and lock rules as selection_t.
// Owns small vectors (tooling path; building one allocates, iterating does
// not). At least one include is required; an empty pool_ref include makes the
// selection match nothing (the component was never registered, so no entity
// can have it).
class runtime_selection
{
public:
    runtime_selection() = default;

    runtime_selection& include(pool_ref pool)
    {
        if (pool.pool_ == nullptr)
        {
            impossible_ = true;
        }
        else
        {
            includes_.push_back(pool.pool_);
        }
        return *this;
    }

    runtime_selection& exclude(pool_ref pool)
    {
        if (pool.pool_ != nullptr)  // an absent pool excludes nothing
        {
            excludes_.push_back(pool.pool_);
        }
        return *this;
    }

    void clear() noexcept
    {
        includes_.clear();
        excludes_.clear();
        impossible_ = false;
    }

    // fn(entity), bool return = early exit. O(smallest include pool).
    // The pool lists are snapshotted for the walk (this is the tooling path),
    // so a callback editing the selection affects only future iterations.
    template <class F>
    void entities(F&& fn) const
    {
        const detail::pool_base* driver = smallest();
        if (driver == nullptr)
        {
            return;
        }
        const std::vector<const detail::pool_base*> includes = includes_;
        const std::vector<const detail::pool_base*> excludes = excludes_;
        const lock_all lock(includes);
        const std::size_t n = driver->size();
        for (std::size_t pos = 0; pos < n; ++pos)
        {
            const entity e = driver->entity_at(pos);
            if (!matches_lists(includes, excludes, e.index(), driver))
            {
                continue;
            }
            if constexpr (std::predicate<F&, entity>)
            {
                if (!fn(e))
                {
                    break;
                }
            }
            else
            {
                static_assert(std::invocable<F&, entity>,
                              "quiver: runtime_selection callback must be callable with "
                              "(quiver::entity)");
                fn(e);
            }
        }
    }

    // Not noexcept: the walk snapshots the pool lists (tooling path).
    [[nodiscard]] std::size_t count() const
    {
        std::size_t n = 0;
        entities(
            [&](entity)
            {
                ++n;
                return true;
            });
        return n;
    }

    [[nodiscard]] bool contains(entity e) const noexcept
    {
        if (impossible_ || includes_.empty())
        {
            return false;
        }
        const std::uint32_t pos = includes_[0]->position_of(e.index());
        if (pos == detail::npos32 || includes_[0]->entity_at(pos) != e)
        {
            return false;
        }
        return matches_rest(e.index(), includes_[0]);
    }

private:
    // Locks the SNAPSHOT taken by entities(): unlock always mirrors lock
    // exactly, even if the live selection is edited mid-iteration.
    struct lock_all
    {
        explicit lock_all(const std::vector<const detail::pool_base*>& pools) noexcept
            : pools_(pools)
        {
            if constexpr (checks_enabled)
            {
                for (const detail::pool_base* pool : pools_)
                {
                    ++pool->locks_;
                }
            }
        }

        ~lock_all()
        {
            if constexpr (checks_enabled)
            {
                for (const detail::pool_base* pool : pools_)
                {
                    --pool->locks_;
                }
            }
        }

        lock_all(const lock_all&) = delete;
        lock_all& operator=(const lock_all&) = delete;

        const std::vector<const detail::pool_base*>& pools_;
    };

    [[nodiscard]] static bool matches_lists(const std::vector<const detail::pool_base*>& includes,
                                            const std::vector<const detail::pool_base*>& excludes,
                                            std::uint32_t index,
                                            const detail::pool_base* driver) noexcept
    {
        for (const detail::pool_base* pool : includes)
        {
            if (pool != driver && !pool->contains(index))
            {
                return false;
            }
        }
        for (const detail::pool_base* pool : excludes)
        {
            if (pool->contains(index))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] const detail::pool_base* smallest() const noexcept
    {
        if (impossible_ || includes_.empty())
        {
            return nullptr;
        }
        const detail::pool_base* best = nullptr;
        for (const detail::pool_base* pool : includes_)
        {
            if (best == nullptr || pool->size() < best->size())
            {
                best = pool;
            }
        }
        return best;
    }

    [[nodiscard]] bool matches_rest(std::uint32_t index,
                                    const detail::pool_base* driver) const noexcept
    {
        for (const detail::pool_base* pool : includes_)
        {
            if (pool != driver && !pool->contains(index))
            {
                return false;
            }
        }
        for (const detail::pool_base* pool : excludes_)
        {
            if (pool->contains(index))
            {
                return false;
            }
        }
        return true;
    }

    std::vector<const detail::pool_base*> includes_;
    std::vector<const detail::pool_base*> excludes_;
    bool impossible_ = false;  // an include named a pool that does not exist
};

// ----------------------------------------------------------------------------
// detail: payload arena
//
// Bump allocation from chunks that never move once allocated, so payload
// pointers recorded by command buffers and blueprints stay valid as they
// grow. Chunks are retained and reused across reset() via the cursor;
// payloads aligned beyond bump_align get dedicated, exactly-aligned chunks.
// ----------------------------------------------------------------------------

namespace detail
{
class payload_arena
{
public:
    static constexpr std::size_t bump_align = 64;
    static constexpr std::size_t chunk_bytes = 4096;

    explicit payload_arena(std::pmr::memory_resource* memory) noexcept
        : memory_(memory)
    {
    }

    payload_arena(payload_arena&& other) noexcept
        : memory_(other.memory_),
          chunks_(std::move(other.chunks_)),
          cursor_(std::exchange(other.cursor_, 0))
    {
        other.chunks_.clear();
    }

    payload_arena& operator=(payload_arena&& other) noexcept
    {
        if (this != &other)
        {
            release();  // our chunks die via our resource...
            chunks_.swap(other.chunks_);
            memory_ = other.memory_;  // ...adopted chunks free via theirs
            cursor_ = std::exchange(other.cursor_, 0);
        }
        return *this;
    }

    payload_arena(const payload_arena&) = delete;
    payload_arena& operator=(const payload_arena&) = delete;

    ~payload_arena() { release(); }

    void* allocate(std::size_t bytes, std::size_t align)
    {
        if (align > bump_align)
        {
            // Dedicated, exactly-aligned chunks — reused after reset(), so a
            // warm buffer recording over-aligned payloads stays allocation-free.
            for (chunk& c : chunks_)
            {
                if (c.used == 0 && c.align >= align && c.capacity >= bytes)
                {
                    c.used = bytes;
                    return c.memory;
                }
            }
            reserve_chunk_slot();
            auto* memory = static_cast<std::byte*>(memory_->allocate(bytes, align));
            chunks_.push_back(chunk{memory, bytes, bytes, align});  // nofail: reserved above
            return memory;
        }
        while (cursor_ < chunks_.size())
        {
            chunk& c = chunks_[cursor_];
            const std::size_t at = (c.used + align - 1) & ~(align - 1);
            if (at + bytes <= c.capacity)
            {
                c.used = at + bytes;
                return c.memory + at;
            }
            ++cursor_;
        }
        reserve_chunk_slot();
        const std::size_t capacity = std::max(chunk_bytes, bytes);
        auto* memory = static_cast<std::byte*>(memory_->allocate(capacity, bump_align));
        chunks_.push_back(chunk{memory, capacity, bytes, bump_align});  // nofail: reserved above
        cursor_ = chunks_.size() - 1;
        return memory;
    }

    // Forgets every payload (callers destroy objects first) but keeps the
    // chunks: a warm arena re-records a steady frame without allocating.
    void reset() noexcept
    {
        for (chunk& c : chunks_)
        {
            c.used = 0;
        }
        cursor_ = 0;
    }

    void release() noexcept
    {
        for (const chunk& c : chunks_)
        {
            memory_->deallocate(c.memory, c.capacity, c.align);
        }
        chunks_.clear();
        cursor_ = 0;
    }

private:
    struct chunk
    {
        std::byte* memory;
        std::size_t capacity;
        std::size_t used;
        std::size_t align;  // the alignment it was allocated (and must be freed) with
    };

    void reserve_chunk_slot()
    {
        if (chunks_.size() == chunks_.capacity())
        {
            chunks_.reserve(std::max<std::size_t>(4, chunks_.capacity() * 2));
        }
    }

    std::pmr::memory_resource* memory_;
    std::vector<chunk> chunks_;  // records only; payload memory comes from memory_
    std::size_t cursor_ = 0;     // first chunk allocate() should try
};
}  // namespace detail

// ----------------------------------------------------------------------------
// Command buffer
//
// Records work now, mutates the world later, at a point you choose:
//
//   quiver::command_buffer cmd;
//   cmd.kill(e);
//   cmd.add<Burning>(e2);
//   quiver::entity fresh = cmd.spawn();          // provisional handle
//   cmd.add<Transform>(fresh, Transform{...});
//   world.apply(cmd);                            // the only sync point
//
// Provisional handles are valid only as targets of later ops in the same
// buffer; apply() resolves them to real entities in recording order. Every op
// is skipped (and counted) if its target is not alive at the moment it would
// run, so buffers never touch recycled slots by accident. Handles recorded in
// a buffer are only meaningful to the world whose handles they are.
// ----------------------------------------------------------------------------

class command_buffer
{
public:
    // The optional memory resource feeds the op list and the payload arena;
    // it must outlive the buffer.
    explicit command_buffer(
        std::pmr::memory_resource* memory = std::pmr::get_default_resource()) noexcept
        : ops_(memory),
          arena_(memory),
          resolved_(memory)
    {
    }

    // The moved-from buffer is equivalent to a freshly constructed one (and
    // gets a new identity, so provisional handles that moved away with the
    // ops cannot be confused with handles it issues later).
    command_buffer(command_buffer&& other) noexcept
        : ops_(std::move(other.ops_)),
          arena_(std::move(other.arena_)),
          resolved_(std::move(other.resolved_)),
          ticket_count_(std::exchange(other.ticket_count_, 0)),
          nonce_(std::exchange(other.nonce_, detail::next_buffer_nonce()))
    {
    }

    // Not noexcept: assigning between buffers on different memory resources
    // falls back to element-wise moves, which may allocate.
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor,bugprone-exception-escape)
    command_buffer& operator=(command_buffer&& other)
    {
        if (this != &other)
        {
            destroy_payloads();            // our pending payloads die before adopting other's
            ops_.clear();                  // never leave records for destroyed payloads if the
            ops_ = std::move(other.ops_);  // cross-resource assignment throws
            other.ops_.clear();
            arena_ = std::move(other.arena_);
            resolved_ = std::move(other.resolved_);
            other.resolved_.clear();
            ticket_count_ = std::exchange(other.ticket_count_, 0);
            nonce_ = std::exchange(other.nonce_, detail::next_buffer_nonce());
        }
        return *this;
    }

    command_buffer(const command_buffer&) = delete;
    command_buffer& operator=(const command_buffer&) = delete;

    ~command_buffer() { destroy_payloads(); }

    // Deferred create. The returned handle is provisional: feed it to later
    // ops in this buffer; apply() spawns the real entity. O(1).
    entity spawn()
    {
        const entity provisional{detail::provisional_bit | ticket_count_, nonce_};
        if constexpr (checks_enabled)
        {
            if (ticket_count_ >= detail::provisional_bit - 1)
            {
                detail::violate("command_buffer: provisional ticket overflow");
            }
        }
        ++ticket_count_;
        reserve_op();
        ops_.push_back(op{op_kind::spawn, provisional, nullptr, nullptr, nullptr});
        return provisional;
    }

    // Deferred world.add<T>. The component is constructed now and moved into
    // place at apply time.
    template <component T, class... Args>
    void add(entity target, Args&&... args)
    {
        record<T, op_kind::add>(target, std::forward<Args>(args)...);
    }

    // Deferred world.put<T> (add-or-replace).
    template <component T, class... Args>
    void put(entity target, Args&&... args)
    {
        record<T, op_kind::put>(target, std::forward<Args>(args)...);
    }

    // Deferred world.remove<T>.
    template <component T>
    void remove(entity target)
    {
        static_assert(!std::same_as<T, detail::kin>,
                      "quiver: parent/child links are managed via adopt/orphan/kill");
        reserve_op();
        ops_.push_back(op{op_kind::remove,
                          target,
                          [](world& w, entity e, void*) { erased_remove<T>(w, e); },
                          nullptr,
                          nullptr});
    }

    // Deferred world.kill.
    void kill(entity target)
    {
        reserve_op();
        ops_.push_back(op{op_kind::kill, target, nullptr, nullptr, nullptr});
    }

    [[nodiscard]] bool empty() const noexcept { return ops_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return ops_.size(); }

    // Destroys all unapplied payloads and resets the buffer for reuse. The
    // payload arena is retained (per-frame reuse allocates nothing once warm);
    // destroy the buffer to release it. Outstanding provisional handles from
    // before the clear go stale and are refused by apply().
    void clear() noexcept
    {
        destroy_payloads();
        ops_.clear();
        ticket_count_ = 0;
        resolved_.clear();
        arena_.reset();
        nonce_ = detail::next_buffer_nonce();
    }

private:
    friend class world;

    enum class op_kind : std::uint8_t
    {
        spawn,
        add,
        put,
        remove,
        kill,
    };

    struct op
    {
        op_kind kind;
        entity target;
        void (*apply_fn)(world&, entity, void*);
        void (*destroy_fn)(void*) noexcept;
        void* payload;
    };

    void destroy_payloads() noexcept
    {
        for (const op& o : ops_)
        {
            if (o.destroy_fn != nullptr)
            {
                o.destroy_fn(o.payload);
            }
        }
    }

    // Guarantees the next ops_.push_back cannot throw, so a constructed
    // payload is never stranded without its destroy record.
    void reserve_op()
    {
        if (ops_.size() == ops_.capacity())
        {
            ops_.reserve(std::max<std::size_t>(16, ops_.capacity() * 2));
        }
    }

    template <component T, op_kind Kind, class... Args>
    void record(entity target, Args&&... args)
    {
        reserve_op();
        if constexpr (detail::is_tag_v<T>)
        {
            static_assert(sizeof...(Args) == 0,
                          "quiver: T uses tag storage and carries no data; record add<T>(e) "
                          "with no arguments");
            ops_.push_back(op{Kind,
                              target,
                              [](world& w, entity e, void*) { erased_apply_tag<T, Kind>(w, e); },
                              nullptr,
                              nullptr});
        }
        else
        {
            static_assert(std::is_move_constructible_v<T>,
                          "quiver: command_buffer payloads are moved into the world at apply(); "
                          "add non-movable components directly via world::add");
            T* payload = std::construct_at(static_cast<T*>(arena_.allocate(sizeof(T), alignof(T))),
                                           std::forward<Args>(args)...);
            void (*destroy_fn)(void*) noexcept = nullptr;
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                destroy_fn = [](void* p) noexcept { std::destroy_at(static_cast<T*>(p)); };
            }
            // nofail: reserve_op() ran before the payload was constructed.
            ops_.push_back(op{Kind,
                              target,
                              [](world& w, entity e, void* p) { erased_apply<T, Kind>(w, e, p); },
                              destroy_fn,
                              payload});
        }
    }

    // Defined after world (they call into it).
    template <component T, op_kind Kind>
    static void erased_apply(world& w, entity target, void* payload);
    template <component T, op_kind Kind>
    static void erased_apply_tag(world& w, entity target);
    template <component T>
    static void erased_remove(world& w, entity target);

    std::pmr::vector<op> ops_;
    detail::payload_arena arena_;
    std::pmr::vector<entity> resolved_;  // scratch used by world::apply; reused across applies
    std::uint32_t ticket_count_ = 0;
    std::uint32_t nonce_ = detail::next_buffer_nonce();  // this buffer's identity
};

// ----------------------------------------------------------------------------
// Blueprint
//
// A reusable spawn recipe: record component values once, stamp them onto any
// number of fresh entities.
//
//   quiver::blueprint goblin;
//   goblin.add<Transform>(Vec2{0, 0});
//   goblin.add<Health>(30);
//   goblin.add<Hostile>();                  // tags too
//   entity g = world.spawn(goblin);         // as many times as you like
//
// Payloads are COPIED at every spawn, so blueprint components must be
// copy-constructible (move-only types cannot be stamped repeatedly — the
// static_assert says so). Blueprints are standalone, reusable, and move-only;
// composition happens at build time in game code (build a second blueprint).
// Stamping writes component pools, so world.spawn(blueprint) follows the same
// iteration rules as spawn with components.
// ----------------------------------------------------------------------------

class blueprint
{
public:
    // The optional memory resource feeds the recipe storage; it must outlive
    // the blueprint.
    explicit blueprint(
        std::pmr::memory_resource* memory = std::pmr::get_default_resource()) noexcept
        : ops_(memory),
          arena_(memory)
    {
    }

    blueprint(blueprint&& other) noexcept
        : ops_(std::move(other.ops_)),
          arena_(std::move(other.arena_))
    {
        other.ops_.clear();
    }

    // Not noexcept: assigning between blueprints on different memory
    // resources falls back to element-wise moves, which may allocate.
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor,bugprone-exception-escape)
    blueprint& operator=(blueprint&& other)
    {
        if (this != &other)
        {
            destroy_payloads();
            ops_.clear();  // no stale records if the cross-resource assignment throws
            ops_ = std::move(other.ops_);
            other.ops_.clear();
            arena_ = std::move(other.arena_);
        }
        return *this;
    }

    blueprint(const blueprint&) = delete;
    blueprint& operator=(const blueprint&) = delete;

    ~blueprint() { destroy_payloads(); }

    // Records one component for every future stamp. Constructor arguments are
    // evaluated once, here; each spawn copy-constructs from the stored value.
    template <component T, class... Args>
    void add(Args&&... args)
    {
        if constexpr (checks_enabled)
        {
            for (const op& o : ops_)
            {
                if (o.type == detail::type_id<T>())
                {
                    detail::violate_pool("blueprint already contains component", name_of<T>());
                    return;
                }
            }
        }
        reserve_op();
        if constexpr (detail::is_tag_v<T>)
        {
            static_assert(sizeof...(Args) == 0,
                          "quiver: T uses tag storage and carries no data; record add<T>() "
                          "with no arguments");
            ops_.push_back(op{detail::type_id<T>(),
                              [](world& w, entity e, const void*) { erased_stamp<T>(w, e); },
                              nullptr,
                              nullptr});
        }
        else
        {
            static_assert(std::copy_constructible<T>,
                          "quiver: blueprint components are copied at every spawn; move-only "
                          "types cannot be stamped repeatedly — add them per entity instead");
            static_assert(std::constructible_from<T, Args&&...>,
                          "quiver: T cannot be constructed from these blueprint arguments");
            T* payload = std::construct_at(static_cast<T*>(arena_.allocate(sizeof(T), alignof(T))),
                                           std::forward<Args>(args)...);
            void (*destroy_fn)(void*) noexcept = nullptr;
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                destroy_fn = [](void* p) noexcept { std::destroy_at(static_cast<T*>(p)); };
            }
            // nofail: reserve_op() ran before the payload was constructed.
            ops_.push_back(op{detail::type_id<T>(),
                              [](world& w, entity e, const void* p)
                              { erased_stamp_value<T>(w, e, p); },
                              destroy_fn,
                              payload});
        }
    }

    [[nodiscard]] bool empty() const noexcept { return ops_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return ops_.size(); }

    // Forgets the recipe; the arena is retained for re-recording.
    void clear() noexcept
    {
        destroy_payloads();
        ops_.clear();
        arena_.reset();
    }

private:
    friend class world;

    struct op
    {
        std::uint32_t type;
        void (*apply_fn)(world&, entity, const void*);
        void (*destroy_fn)(void*) noexcept;
        void* payload;
    };

    void destroy_payloads() noexcept
    {
        for (const op& o : ops_)
        {
            if (o.destroy_fn != nullptr)
            {
                o.destroy_fn(o.payload);
            }
        }
    }

    void reserve_op()
    {
        if (ops_.size() == ops_.capacity())
        {
            ops_.reserve(std::max<std::size_t>(8, ops_.capacity() * 2));
        }
    }

    // Defined after world (they call into it).
    template <component T>
    static void erased_stamp(world& w, entity e);
    template <component T>
    static void erased_stamp_value(world& w, entity e, const void* payload);

    std::pmr::vector<op> ops_;
    detail::payload_arena arena_;
};

// ----------------------------------------------------------------------------
// World
// ----------------------------------------------------------------------------

namespace detail
{
// Built-in component backing the optional parent/child links. Stable storage:
// link surgery holds pointers across sibling-list edits.
struct kin
{
    entity parent = no_entity;
    entity first_child = no_entity;
    entity prev_sibling = no_entity;
    entity next_sibling = no_entity;

    static constexpr auto quiver_storage = storage::stable;
};

// The parent/child link invariants, kept in one place: world::adopt/orphan/
// kill call in; nothing else may edit kin records.
namespace kin_links
{
// Detaches child_k from its parent's sibling list (links only; the record's
// own fields are the caller's to reset).
inline void unlink(stable_pool<kin>& pool, kin& child_k) noexcept
{
    if (child_k.prev_sibling != no_entity)
    {
        pool.at(child_k.prev_sibling.index())->next_sibling = child_k.next_sibling;
    }
    else
    {
        pool.at(child_k.parent.index())->first_child = child_k.next_sibling;
    }
    if (child_k.next_sibling != no_entity)
    {
        pool.at(child_k.next_sibling.index())->prev_sibling = child_k.prev_sibling;
    }
}

// Pre-kill surgery: detach e from its parent's list and orphan all of its
// children, so no other entity ever holds a link to the dying one.
inline void sever(stable_pool<kin>& pool, entity e) noexcept
{
    kin* k = pool.at(e.index());
    if (k == nullptr)
    {
        return;
    }
    if (k->parent != no_entity)
    {
        unlink(pool, *k);
    }
    for (entity child = k->first_child; child != no_entity;)
    {
        kin* child_k = pool.at(child.index());
        const entity next = child_k->next_sibling;
        child_k->parent = no_entity;
        child_k->prev_sibling = no_entity;
        child_k->next_sibling = no_entity;
        child = next;
    }
}

// Link-graph audit for world::validate().
[[nodiscard]] inline std::expected<void, fault> check(const stable_pool<kin>& pool,
                                                      const entity_table& table)
{
    const auto broken = [&](const char* note)
    { return std::unexpected(fault{fault_code::links_broken, pool.name(), note}); };
    for (std::size_t pos = 0; pos < pool.size(); ++pos)
    {
        const entity e = pool.entity_at(pos);
        const kin* k = pool.at(e.index());
        if (k->parent != no_entity)
        {
            if (!table.alive(k->parent) || pool.at(k->parent.index()) == nullptr)
            {
                return broken("dangling parent");
            }
            if (k->prev_sibling == no_entity && pool.at(k->parent.index())->first_child != e)
            {
                return broken("head child not referenced by parent");
            }
        }
        if (k->next_sibling != no_entity)
        {
            const kin* next_k = pool.at(k->next_sibling.index());
            if (next_k == nullptr || next_k->prev_sibling != e || next_k->parent != k->parent)
            {
                return broken("sibling chain mismatch");
            }
        }
        if (k->first_child != no_entity)
        {
            // Bounded walk: a corrupt cycle must not hang validate().
            std::size_t steps = 0;
            for (entity child = k->first_child; child != no_entity;)
            {
                const kin* child_k = pool.at(child.index());
                if (child_k == nullptr || child_k->parent != e || ++steps > pool.size())
                {
                    return broken("child list mismatch");
                }
                child = child_k->next_sibling;
            }
        }
    }
    return {};
}
}  // namespace kin_links
}  // namespace detail

class world
{
public:
    // The optional memory resource feeds everything that scales — component
    // payloads, sparse pages, dense arrays, the entity table — with zero
    // template ceremony (signatures never change). It must outlive the world.
    // Bounded structures (the pool objects themselves, hook lists) stay on
    // the global allocator.
    explicit world(std::pmr::memory_resource* memory = std::pmr::get_default_resource()) noexcept
        : table_(memory),
          pools_(memory),
          active_(memory),
          bonds_(memory),
          memory_(memory)
    {
    }

    // Move-construction carries the pools along, so selections built before
    // the move remain valid. Move-ASSIGNMENT destroys the assigned-over
    // world's pools first: selections built from the destination world dangle
    // exactly as they would on its destruction. Moved-from worlds are empty
    // and reusable.
    world(world&& other) noexcept
        : table_(std::move(other.table_)),
          pools_(std::move(other.pools_)),
          active_(std::move(other.active_)),
          bonds_(std::move(other.bonds_)),  // bond objects are heap-stable: pool
          memory_(other.memory_),           // bond_ pointers stay valid
          globals_(std::exchange(other.globals_, no_entity))
    {
        other.pools_.clear();  // guarantee the moved-from world is empty
        other.active_.clear();
        other.bonds_.clear();
        repoint_pools();
    }

    // Not noexcept: assigning between worlds on different memory resources
    // falls back to element-wise moves, which may allocate. With matching
    // resources it is O(1) pointer steals.
    // NOLINTNEXTLINE(cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor,bugprone-exception-escape)
    world& operator=(world&& other)
    {
        if (this != &other)
        {
            if constexpr (checks_enabled)
            {
                // The target's pools are about to die under any running loop.
                if (any_locked())
                {
                    detail::violate("world move-assigned over while an iteration is running");
                }
            }
            table_ = std::move(other.table_);
            active_.clear();  // if a cross-resource assignment below throws,
                              // nothing here dangles into destroyed pools
            pools_ = std::move(other.pools_);
            active_ = std::move(other.active_);
            bonds_ = std::move(other.bonds_);
            memory_ = other.memory_;  // future pools follow the adopted content
            globals_ = std::exchange(other.globals_, no_entity);
            other.pools_.clear();  // guarantee the moved-from world is empty
            other.active_.clear();
            other.bonds_.clear();
            repoint_pools();
        }
        return *this;
    }

    world(const world&) = delete;
    world& operator=(const world&) = delete;

    // Pools die with the world; components are destroyed here. Tearing a
    // world down while one of its iterations is running is reported in
    // checked builds (and is unavoidable UB afterwards — a destructor cannot
    // refuse).
    ~world()
    {
        if constexpr (checks_enabled)
        {
            if (any_locked())
            {
                detail::violate("world destroyed while an iteration is running");
            }
        }
    }

    // ------------------------------------------------------------------ life

    // O(1) amortized. Never touches component pools, which is why bare spawn()
    // is the one structural operation that is always legal during iteration.
    entity spawn() { return table_.create(); }

    // Spawn with initial components. This DOES write component pools, so it is
    // not legal while one of those pools is being iterated (checked builds
    // report it; use spawn() + command_buffer adds instead).
    template <class... Cs>
        requires(sizeof...(Cs) > 0 && (component<std::remove_cvref_t<Cs>> && ...) &&
                 (!std::same_as<std::remove_cvref_t<Cs>, blueprint> && ...))
    entity spawn(Cs&&... components)
    {
        static_assert(detail::all_distinct<std::remove_cvref_t<Cs>...>,
                      "quiver: duplicate component type in spawn(...)");
        const entity e = table_.create();
        (add_value(e, std::forward<Cs>(components)), ...);
        return e;
    }

    // Stamps a blueprint onto a fresh entity (components copy-constructed
    // from the recipe). Same iteration rules as spawn with components.
    // O(recorded components).
    entity spawn(const blueprint& recipe)
    {
        const entity e = table_.create();
        for (const blueprint::op& o : recipe.ops_)
        {
            o.apply_fn(*this, e, o.payload);
        }
        return e;
    }

    // Spawns a copy of src: every copy-constructible component is duplicated
    // (parent/child links are NOT — clones start as unlinked roots), and
    // components that cannot be copied are counted in `skipped`. Structural
    // on every pool src occupies, so it follows the same iteration rules as
    // spawn with components. O(pools this world uses).
    duplicate_result duplicate(entity src)
    {
        if (!table_.alive(src))
        {
            if constexpr (checks_enabled)
            {
                detail::violate("duplicate of a dead, stale, or null entity handle");
            }
            return {};
        }
        if (is_globals(src))
        {
            // A second marked entity would break globals() uniqueness.
            if constexpr (checks_enabled)
            {
                detail::violate("duplicate of the globals entity");
            }
            return {};
        }
        if constexpr (checks_enabled)
        {
            for (detail::pool_base* pool : active_)
            {
                if (pool->bond_locked() && pool->contains(src.index()))
                {
                    detail::violate_pool(
                        "duplicate during iteration over pool (or its bonded partner)",
                        pool->name());
                    return {};
                }
            }
        }
        duplicate_result result{table_.create()};
        const detail::pool_base* links = peek_pool<detail::kin>();
        // Index loop: on_add hooks fired by the copies may register new pools
        // (growing active_). Hooks observe a partially built clone, in pool
        // registration order; a component a hook already gave the clone is
        // not copied over it.
        // NOLINTNEXTLINE(modernize-loop-convert) -- tolerates active_ growth from hooks
        for (std::size_t i = 0; i < active_.size(); ++i)
        {
            detail::pool_base* pool = active_[i];
            if (pool == links || !pool->contains(src.index()) ||
                pool->contains(result.clone.index()))
            {
                continue;
            }
            if (pool->copy_item(src.index(), result.clone))
            {
                ++result.copied;
            }
            else
            {
                ++result.skipped;
            }
        }
        return result;
    }

    // Immediate destroy: removes every component, severs parent/child links,
    // and recycles the slot (generation bump kills outstanding handles).
    // O(pools this world uses), noexcept (the free stack pre-reserves slack;
    // violation handlers must not throw). Checked builds refuse (skip) a kill
    // while the entity belongs to any pool that is being iterated, before any
    // mutation.
    void kill(entity e) noexcept
    {
        if (!table_.alive(e) || e == dying_)  // dying_: re-entrant kill from a hook
        {
            if constexpr (checks_enabled)
            {
                detail::violate("kill on a dead, dying, stale, or null entity handle");
            }
            return;
        }
        if (is_globals(e))  // cached compare, then one mark-pool probe
        {
            if constexpr (checks_enabled)
            {
                detail::violate("kill of the globals entity (reset() clears world state)");
            }
            return;
        }
        const std::uint32_t index = e.index();
        if constexpr (checks_enabled)
        {
            // NOLINTNEXTLINE(modernize-loop-convert) -- tolerates active_ growth from hooks
            for (std::size_t i = 0; i < active_.size(); ++i)
            {
                detail::pool_base* pool = active_[i];
                if (pool->bond_locked() && pool->contains(index))
                {
                    detail::violate_pool("kill during iteration over pool (or its bonded partner)",
                                         pool->name());
                    return;
                }
            }
        }
        // Index loops here and below: an on_remove hook may register a brand
        // new component type, growing active_ under us (pool objects never
        // move, so indexing stays valid — and the new pool gets visited).
        const entity previous_dying = std::exchange(dying_, e);
        sever_links(e);
        // NOLINTNEXTLINE(modernize-loop-convert) -- tolerates active_ growth from hooks
        for (std::size_t i = 0; i < active_.size(); ++i)
        {
            active_[i]->erase_if_present(index);
        }
        dying_ = previous_dying;
        table_.destroy(index);
    }

    [[nodiscard]] bool alive(entity e) const noexcept { return table_.alive(e); }

    // The live handle occupying a slot, or no_entity. For inspectors/tools.
    [[nodiscard]] entity current_handle(std::uint32_t slot) const noexcept
    {
        if (!table_.occupied(slot))
        {
            return no_entity;
        }
        return entity{slot, table_.generation_at(slot)};
    }

    // Call-site sugar: a {world, entity} view forwarding the component verbs
    // (defined after the class). Store quiver::entity in components, never
    // an entity_ref — it is for passing through call chains, not for storage.
    [[nodiscard]] entity_ref ref(entity e) noexcept;
    [[nodiscard]] const_entity_ref ref(entity e) const noexcept;

    // The world's singleton carrier: one ordinary entity, marked with
    // globals_mark, spawned lazily here. World-scoped state is just
    // components on it — every component verb, hook, tracker, and archive
    // works unchanged:
    //
    //   world.globals().obtain<MatchClock>().elapsed += dt;
    //
    // Never store the handle (reset() wipes it like everything else; the
    // next globals() call respawns or re-finds it — after unpack it finds
    // the restored marked entity). kill()/duplicate() refuse it. It is a
    // VISIBLE live entity: live_count()/live_entities()/pack() include it,
    // so inspector numbers stay honest; except<globals_mark> filters it out
    // of broad queries if needed. (Both defined after entity_ref.)
    [[nodiscard]] entity_ref globals();
    [[nodiscard]] const_entity_ref globals() const noexcept;  // never spawns

    void reserve_entities(std::size_t n) { table_.reserve(n); }

    // ------------------------------------------------------------ components

    // Adds T; the entity must not already have it (checked builds report a
    // violation, then the duplicate is dropped and the existing component is
    // returned). Returns T& (void for tag components). O(1) amortized.
    // Invalidates pointers into T's pool if it is packed storage.
    //
    // During an iteration over T's pool, a tag add is refused after the
    // violation report; a value add must produce a reference, so it reports
    // and then PROCEEDS — at which point component references the running
    // callback already holds (including its own parameters) may dangle.
    // Record into a command_buffer instead.
    template <component T, class... Args>
    decltype(auto) add(entity e, Args&&... args)
    {
        auto& pool = ensure_pool<T>();
        // Unconditional (all builds): adding through a dead, stale, or null
        // handle would corrupt pool membership, and a reference must be
        // produced — so this is fatal, matching replace().
        if (!table_.alive(e))
        {
            if constexpr (checks_enabled)
            {
                detail::violate_pool("add on a dead, stale, or null entity; pool", pool.name());
            }
            std::abort();
        }
        if constexpr (detail::is_tag_v<T>)
        {
            static_assert(sizeof...(Args) == 0,
                          "quiver: T uses tag storage and carries no data; call add<T>(e) with "
                          "no arguments (or give T members / a non-tag storage policy)");
            if (clearing_ || e == dying_)  // a hook adding to a doomed entity
            {
                if constexpr (checks_enabled)
                {
                    detail::violate_pool("add to a dying entity or during reset; pool",
                                         pool.name());
                }
                return;
            }
            if constexpr (checks_enabled)
            {
                if (pool.bond_locked())
                {
                    detail::violate_pool("add during iteration over pool (or its bonded partner)",
                                         pool.name());
                    return;  // refusal is possible here: nothing to return
                }
            }
            if (pool.contains(e.index()))
            {
                if constexpr (checks_enabled)
                {
                    detail::violate_pool("add of a component the entity already has; pool",
                                         pool.name());
                }
                return;
            }
            pool.emplace(e);
        }
        else
        {
            static_assert(std::constructible_from<T, Args&&...>,
                          "quiver: T cannot be constructed from these add() arguments (note: "
                          "passing a T value requires T to be move-constructible; non-movable "
                          "components must be constructed in place from constructor arguments)");
            if (clearing_ || e == dying_)
            {
                // A hook adding a component to the entity being killed (or
                // during reset) would leave a pool holding a dead entity:
                // fatal in every build, like add through a dead handle.
                if constexpr (checks_enabled)
                {
                    detail::violate_pool("add to a dying entity or during reset; pool",
                                         pool.name());
                }
                std::abort();
            }
            if constexpr (checks_enabled)
            {
                if (pool.bond_locked())
                {
                    // Reported, then proceeds: a T& must be produced. The
                    // running loop itself stays safe (it re-reads the dense
                    // array each step), but previously fetched references die —
                    // and on a bonded pool the partner's iteration order shifts.
                    detail::violate_pool("add during iteration over pool (or its bonded partner)",
                                         pool.name());
                }
            }
            if (T* existing = pool.at(e.index()); existing != nullptr)
            {
                if constexpr (checks_enabled)
                {
                    detail::violate_pool("add of a component the entity already has; pool",
                                         pool.name());
                }
                return *existing;
            }
            return pool.emplace(e, std::forward<Args>(args)...);
        }
    }

    // Replaces the existing T by constructing a new value. The entity must
    // have T (checked). Pointer-stable in every storage policy (the component
    // is rebuilt in place). O(1).
    template <component T, class... Args>
    T& replace(entity e, Args&&... args)
    {
        static_assert(!detail::is_tag_v<T>,
                      "quiver: T uses tag storage and carries no data; there is nothing to "
                      "replace — use has<T>/add<T>/remove<T>");
        T* existing = lookup<T>(e);
        if (existing == nullptr)
        {
            if constexpr (checks_enabled)
            {
                detail::violate_pool("replace of a component the entity does not have; pool",
                                     name_of<T>());
            }
            std::abort();  // cannot produce a component reference
        }
        if constexpr (std::is_move_assignable_v<T>)
        {
            *existing = T(std::forward<Args>(args)...);
        }
        else
        {
            std::destroy_at(existing);
            std::construct_at(existing, std::forward<Args>(args)...);
        }
        peek_pool<T>()->fire_replace(e);  // non-null: the component exists
        return *existing;
    }

    // Mutates the existing T in place through fn(T&), then fires on_replace —
    // the funnel for OBSERVED writes (plain get<T>() writes stay invisible to
    // hooks and trackers by design; route the ones that matter through here).
    // Mirrors replace's contract: the entity must have T (checked), pointer-
    // stable in every storage policy, not structural, and therefore legal
    // during iteration of T's pool — including on the entity currently
    // visited. O(1) plus the callable.
    template <component T, class F>
        requires std::invocable<F&, T&>
    T& amend(entity e, F&& fn)
    {
        static_assert(!detail::is_tag_v<T>,
                      "quiver: T uses tag storage and carries no data; there is nothing to "
                      "amend — use has<T>/add<T>/remove<T>");
        T* existing = lookup<T>(e);
        if (existing == nullptr)
        {
            if constexpr (checks_enabled)
            {
                detail::violate_pool("amend of a component the entity does not have; pool",
                                     name_of<T>());
            }
            std::abort();  // cannot produce a component reference
        }
        std::invoke(fn, *existing);
        peek_pool<T>()->fire_replace(e);  // non-null: the component exists
        return *existing;
    }

    // Add-or-replace. Structural (and therefore iteration-locked, with add's
    // report-then-proceed caveat) only when the component is actually added.
    // O(1) amortized.
    template <component T, class... Args>
    decltype(auto) put(entity e, Args&&... args)
    {
        if constexpr (detail::is_tag_v<T>)
        {
            static_assert(sizeof...(Args) == 0,
                          "quiver: T uses tag storage and carries no data; call put<T>(e) with "
                          "no arguments");
            if (!has<T>(e))
            {
                add<T>(e);
            }
        }
        else
        {
            if (T* existing = lookup<T>(e); existing != nullptr)
            {
                if constexpr (std::is_move_assignable_v<T>)
                {
                    *existing = T(std::forward<Args>(args)...);
                }
                else
                {
                    std::destroy_at(existing);
                    std::construct_at(existing, std::forward<Args>(args)...);
                }
                peek_pool<T>()->fire_replace(e);
                return *existing;
            }
            return add<T>(e, std::forward<Args>(args)...);
        }
    }

    // Removes T if present; returns whether anything was removed. Removing
    // from a stale handle is a checked violation (and removes nothing).
    // Swap-remove: invalidates pointers into packed pools. O(1).
    template <component T>
    bool remove(entity e)
    {
        static_assert(!std::same_as<T, detail::kin>,
                      "quiver: parent/child links are managed via adopt/orphan/kill");
        if (!table_.alive(e))
        {
            if constexpr (checks_enabled)
            {
                detail::violate("remove on a dead, stale, or null entity handle");
            }
            return false;
        }
        auto* pool = peek_pool<T>();
        if (pool == nullptr || !pool->contains(e.index()))
        {
            return false;
        }
        if constexpr (checks_enabled)
        {
            if (pool->bond_locked())
            {
                detail::violate_pool("remove during iteration over pool (or its bonded partner)",
                                     pool->name());
                return false;
            }
        }
        pool->erase_if_present(e.index());
        return true;
    }

    // True when e is alive and has T. Safe on any handle. O(1).
    template <component T>
    [[nodiscard]] bool has(entity e) const noexcept
    {
        if (!table_.alive(e))
        {
            return false;
        }
        return probe<T>(e.index());
    }

    // True when e is alive and has EVERY listed component — the multi-type
    // spelling of has (which stays the one-type form). Tags are legal:
    // membership is exactly what they are. Safe on any handle; one liveness
    // check, then one pool probe per type. O(types).
    template <component T, component U, component... Rest>
    [[nodiscard]] bool has_all(entity e) const noexcept
    {
        static_assert(detail::all_distinct<T, U, Rest...>,
                      "quiver: duplicate component type in has_all<...>");
        if (!table_.alive(e))
        {
            return false;
        }
        return probe<T>(e.index()) && probe<U>(e.index()) && (probe<Rest>(e.index()) && ...);
    }

    // True when e is alive and has AT LEAST ONE listed component. Same
    // contract as has_all; short-circuits on the first hit. O(types).
    template <component T, component U, component... Rest>
    [[nodiscard]] bool has_any(entity e) const noexcept
    {
        static_assert(detail::all_distinct<T, U, Rest...>,
                      "quiver: duplicate component type in has_any<...>");
        if (!table_.alive(e))
        {
            return false;
        }
        return probe<T>(e.index()) || probe<U>(e.index()) || (probe<Rest>(e.index()) || ...);
    }

    // Checked reference access; the safe form is find<T>. O(1). One deduced-
    // this body serves both constnesses: const worlds yield const T&.
    template <component T, class Self>
    [[nodiscard]] auto& get(this Self&& self, entity e)
    {
        static_assert(!detail::is_tag_v<T>,
                      "quiver: T uses tag storage and carries no data; use has<T>(e)");
        auto* p = self.template lookup<T>(e);  // T* or const T* per Self
        if constexpr (checks_enabled)
        {
            if (p == nullptr)
            {
                detail::violate_pool(
                    "get of a missing component (dead entity or never added); "
                    "pool",
                    name_of<T>());
                std::abort();  // cannot produce a component reference
            }
        }
        return *p;
    }

    // Multi-component point lookup with structured bindings:
    //   auto [t, v] = world.get<Transform, Velocity>(e);
    // Each component keeps single-get's checked contract. O(types).
    template <component T, component U, component... Rest, class Self>
    [[nodiscard]] auto get(this Self&& self, entity e)
    {
        return std::tuple<decltype(self.template get<T>(e)),
                          decltype(self.template get<U>(e)),
                          decltype(self.template get<Rest>(e))...>{
            self.template get<T>(e), self.template get<U>(e), self.template get<Rest>(e)...};
    }

    // Pointer access: nullptr when e is dead/stale or lacks T. O(1).
    template <component T, class Self>
    [[nodiscard]] auto* find(this Self&& self, entity e) noexcept
    {
        static_assert(!detail::is_tag_v<T>,
                      "quiver: T uses tag storage and carries no data; use has<T>(e)");
        return self.template lookup<T>(e);
    }

    // Multi-component find: one liveness check, a pointer per type (all null
    // for dead handles). O(types).
    template <component T, component U, component... Rest, class Self>
    [[nodiscard]] auto find_all(this Self&& self, entity e) noexcept
    {
        using row = std::tuple<decltype(self.template find_in_pool<T>(e)),
                               decltype(self.template find_in_pool<U>(e)),
                               decltype(self.template find_in_pool<Rest>(e))...>;
        if (!self.table_.alive(e))
        {
            return row{};
        }
        return row{self.template find_in_pool<T>(e),
                   self.template find_in_pool<U>(e),
                   self.template find_in_pool<Rest>(e)...};
    }

    // Get-or-add, completing the verb matrix (add = insert, put = upsert,
    // replace = update, obtain = get-or-insert): returns the existing T, or
    // adds one constructed from args. The lazily-attached-state idiom.
    // O(1) amortized; add's iteration rules apply only when it actually adds.
    template <component T, class... Args>
    decltype(auto) obtain(entity e, Args&&... args)
    {
        if constexpr (detail::is_tag_v<T>)
        {
            static_assert(sizeof...(Args) == 0,
                          "quiver: T uses tag storage and carries no data; call obtain<T>(e) "
                          "with no arguments");
            if (!has<T>(e))
            {
                add<T>(e);
            }
        }
        else
        {
            if (T* existing = lookup<T>(e); existing != nullptr)
            {
                return *existing;
            }
            return add<T>(e, std::forward<Args>(args)...);
        }
    }

    // Destroys every T in the world. O(count of T). The pool object survives (selections stay
    // valid) but its contents and capacity may be released. Stable-storage
    // pointer stability ends here.
    template <component T>
    void purge()
    {
        static_assert(!std::same_as<T, detail::kin>,
                      "quiver: parent/child links are managed via adopt/orphan/kill");
        auto* pool = peek_pool<T>();
        if (pool == nullptr)
        {
            return;
        }
        if constexpr (checks_enabled)
        {
            if (pool->bond_locked())
            {
                detail::violate_pool("purge during iteration over pool (or its bonded partner)",
                                     pool->name());
                return;
            }
        }
        pool->wipe();
        if (pool->bond_ != nullptr)
        {
            pool->bond_->paired = 0;  // the intersection emptied with the pool
        }
    }

    template <component T>
    void reserve(std::size_t n)
    {
        ensure_pool<T>().reserve(n);
    }

    // Reorders T's pool — the dense order selections, snapshots, and range()
    // iterate in. cmp compares component values (const T&, const T&) or
    // entities (entity, entity); the value form wins when both bind. The
    // optional algorithm is called as algo(first, last, cmp) over a random-
    // access range of dense positions and must permute it exactly as a
    // comparison sort would (std::stable_sort qualifies; default std::sort).
    // Cold: O(n log n) plus one scratch allocation; refused while T's pool is
    // being iterated. Packed pools swap components, so outstanding T* die;
    // stable pools permute bookkeeping only, so component pointers survive
    // sorting.
    template <component T, class Compare, class Algorithm = detail::std_sort>
    void sort(Compare cmp, Algorithm&& algo = {})
    {
        auto* pool = peek_pool<T>();
        if (pool == nullptr || pool->size() < 2)
        {
            return;
        }
        // Refused in EVERY build: reordering one owner of a bond would
        // silently break the mirrored partition. Order the partition itself
        // with bonded(...).sort(...), or unbond first.
        if (pool->bond() != nullptr)
        {
            if constexpr (checks_enabled)
            {
                detail::violate_pool(
                    "sort on an owned (bonded) pool — use bonded(...).sort(...) for "
                    "in-partition order, or unbond first; pool",
                    pool->name());
            }
            return;
        }
        if constexpr (checks_enabled)
        {
            if (pool->locked())
            {
                detail::violate_pool("sort during iteration over pool", pool->name());
                return;
            }
        }
        if constexpr (std::invocable<Compare&, const T&, const T&> && !detail::is_tag_v<T>)
        {
            pool->sort_dense([&](std::uint32_t a, std::uint32_t b)
                             { return static_cast<bool>(cmp(pool->at_pos(a), pool->at_pos(b))); },
                             std::forward<Algorithm>(algo));
        }
        else if constexpr (std::invocable<Compare&, entity, entity>)
        {
            pool->sort_dense(
                [&](std::uint32_t a, std::uint32_t b)
                { return static_cast<bool>(cmp(pool->entity_at(a), pool->entity_at(b))); },
                std::forward<Algorithm>(algo));
        }
        else
        {
            static_assert(std::invocable<Compare&, entity, entity>,
                          "quiver: sort<T> comparator must be callable as (const T&, const T&) "
                          "for value sorting (non-tag components only) or as (entity, entity)");
        }
    }

    // Reorders Follow's pool so entities shared with Lead appear in Lead's
    // dense order (non-shared entities end up after them, order unspecified).
    // The opt-in locality optimization: after pairing, iterating
    // select<Lead, Follow> driving from Lead touches Follow's payload
    // near-linearly. One-shot — no standing coupling between the pools;
    // re-run after churn if a profile says so. Cold, O(size of Lead).
    // Refused while Follow's pool iterates. Packed Follow pointers die;
    // stable Follow pointers survive.
    template <component Follow, component Lead>
    void sort_along()
    {
        static_assert(!std::same_as<Follow, Lead>,
                      "quiver: sort_along needs two different component types");
        auto* follow = peek_pool<Follow>();
        const auto* lead = peek_pool<Lead>();
        if (follow == nullptr || lead == nullptr || follow->size() < 2)
        {
            return;
        }
        // Every build: reordering a bonded Follow breaks the mirror. A bonded
        // Lead is read-only here and stays legal.
        if (follow->bond() != nullptr)
        {
            if constexpr (checks_enabled)
            {
                detail::violate_pool("sort_along on a bonded pool (unbond first); pool",
                                     follow->name());
            }
            return;
        }
        if constexpr (checks_enabled)
        {
            if (follow->locked())
            {
                detail::violate_pool("sort_along during iteration over pool", follow->name());
                return;
            }
        }
        std::uint32_t front = 0;
        const std::size_t n = lead->size();
        for (std::size_t pos = 0; pos < n; ++pos)
        {
            const entity e = lead->entity_at(pos);
            const std::uint32_t at = follow->position_of(e.index());
            if (at == detail::npos32)
            {
                continue;
            }
            // Unprocessed entities always sit at or beyond the front cursor.
            if (at != front)
            {
                follow->swap_positions(front, at);
            }
            ++front;
        }
    }

    // --------------------------------------------------------- bonded pools
    //
    // See the bonded_view block for the contract. bond() builds the partition
    // over existing members (O(|first| · owners)); from then on every
    // add/remove maintains it with one O(1) mirrored swap per owner. Calling
    // bond<Ts...>() again for the same standing owned set (any order) is
    // idempotent and just returns a view. Overlapping or nested owned sets
    // are refused: one partition owner per pool, no exceptions.

    template <class... Ts>  // const-qualify for read-only payload parts
        requires(sizeof...(Ts) >= 2) && (component<detail::bare<Ts>> && ...)
    bonded_view_t<except<>, Ts...> bond()
    {
        static_assert(detail::all_distinct<detail::bare<Ts>...>,
                      "quiver: duplicate component type in bond<...>");
        static_assert((!detail::is_maybe_v<detail::bare<Ts>> && ...),
                      "quiver: maybe<> marks optional selection parts and cannot be bonded");
        static_assert((!detail::is_any_of_v<detail::bare<Ts>> && ...),
                      "quiver: bonds own pools, not alternatives — any_of<> belongs in select");
        // derived_from, not same_as: custom pools derived from the packed
        // built-in swap exactly the same way and need the same wall.
        static_assert(((!std::derived_from<pool_of_t<detail::bare<Ts>>,
                                           detail::packed_pool<detail::bare<Ts>>> ||
                        std::is_swappable_v<detail::bare<Ts>>) &&
                       ...),
                      "quiver: bonded packed pools swap components to maintain the partition; "
                      "make every packed side swappable or store it in stable storage");
        static_assert(sizeof...(Ts) <= detail::group_core::max_owners,
                      "quiver: a bond spans at most group_core::max_owners (8) pools");
        const std::array<detail::pool_base*, sizeof...(Ts)> pools{
            &ensure_pool<detail::bare<Ts>>()...};
        // Idempotent for the SAME standing owned set, in any order.
        if (detail::group_core* standing = pools[0]->bond_;
            standing != nullptr && standing->owner_count == sizeof...(Ts))
        {
            bool same = true;
            for (detail::pool_base* pool : pools)
            {
                same = same && standing->owns(pool);
            }
            if (same)
            {
                return bonded<Ts...>();
            }
        }
        for (detail::pool_base* pool : pools)
        {
            if (pool->bond_ != nullptr)
            {
                if constexpr (checks_enabled)
                {
                    detail::violate_pool("bond on an already bonded pool", pool->name());
                }
                return {};
            }
        }
        if constexpr (checks_enabled)
        {
            for (detail::pool_base* pool : pools)
            {
                if (pool->locked())
                {
                    detail::violate_pool("bond during iteration over pool", pool->name());
                    return {};
                }
            }
        }
        bonds_.push_back(std::make_unique<detail::group_core>());
        detail::group_core* bond = bonds_.back().get();
        bond->owner_count = sizeof...(Ts);
        for (std::size_t i = 0; i < pools.size(); ++i)
        {
            bond->owners[i] = pools[i];
        }
        // One-time partition build: partition the FIRST owner probing all
        // others, then mirror each remaining owner's prefix into its order.
        detail::pool_base& first = *pools[0];
        std::uint32_t k = 0;
        const auto n = static_cast<std::uint32_t>(first.size());
        for (std::uint32_t pos = 0; pos < n; ++pos)
        {
            const std::uint32_t index = first.entity_at(pos).index();
            bool in_all = true;
            for (std::size_t i = 1; i < pools.size(); ++i)
            {
                in_all = in_all && pools[i]->contains(index);
            }
            if (in_all)
            {
                if (pos != k)
                {
                    first.mirror_swap(k, pos);
                }
                ++k;
            }
        }
        for (std::size_t i = 1; i < pools.size(); ++i)
        {
            for (std::uint32_t j = 0; j < k; ++j)
            {
                const std::uint32_t there = pools[i]->position_of(first.entity_at(j).index());
                if (there != j)
                {
                    pools[i]->mirror_swap(j, there);
                }
            }
        }
        bond->paired = k;
        for (detail::pool_base* pool : pools)
        {
            pool->bond_ = bond;  // armed last: the build itself must not re-enter
        }
        return bonded<Ts...>();
    }

    // Dissolves the bond; every owner keeps its current order. Stored views
    // read as empty from here on (the bond object is tombstoned, not freed).
    // The full owned set names the group, in any order.
    template <class... Ts>
        requires(sizeof...(Ts) >= 2) && (component<detail::bare<Ts>> && ...)
    bool unbond()
    {
        const std::array<detail::pool_base*, sizeof...(Ts)> pools{peek_pool<detail::bare<Ts>>()...};
        detail::group_core* bond = standing_bond(pools);
        if (bond == nullptr)
        {
            return false;
        }
        if constexpr (checks_enabled)
        {
            for (detail::pool_base* pool : pools)
            {
                if (pool->locked())
                {
                    detail::violate_pool("unbond during iteration over pool", pool->name());
                    return false;
                }
            }
        }
        bond->owners = {};  // tombstone
        bond->owner_count = 0;
        bond->paired = 0;
        for (detail::pool_base* pool : pools)
        {
            pool->bond_ = nullptr;
        }
        return true;
    }

    // The standing bond's view. Plain-listed types must be the FULL owned
    // set, in any order; maybe<>-marked types are OBSERVED extras probed per
    // row (T* parts, null when absent — partition membership does not imply
    // observed membership); except<> filters rows. Without a standing bond
    // over exactly the plain-listed set: checked violation, empty view.
    template <class... Ts, class... Xs>
        requires(sizeof...(Ts) >= 2)
    [[nodiscard]] bonded_view_t<except<Xs...>, Ts...> bonded(except<Xs...>)
    {
        constexpr std::size_t n = sizeof...(Ts);
        constexpr std::array<bool, n> observed{detail::is_maybe_v<Ts>...};
        const std::array<detail::pool_base*, n> pools{
            peek_pool<detail::bare<detail::maybe_inner<Ts>>>()...};
        const detail::group_core* bond = nullptr;
        bool listed_exactly = true;
        std::size_t owned_listed = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (observed[i])
            {
                continue;  // observed pools may be missing: null = absent parts
            }
            ++owned_listed;
            if (pools[i] == nullptr || pools[i]->bond_ == nullptr ||
                (bond != nullptr && pools[i]->bond_ != bond))
            {
                listed_exactly = false;
                break;
            }
            bond = pools[i]->bond_;
        }
        listed_exactly = listed_exactly && bond != nullptr && bond->owner_count == owned_listed;
        if (!listed_exactly)
        {
            if constexpr (checks_enabled)
            {
                detail::violate(
                    "bonded view must plain-list the standing owned set in full (any order); "
                    "mark observed extras maybe<>, or use select for subset queries");
            }
            return {};
        }
        const std::array<detail::pool_base*, sizeof...(Xs)> excludes{peek_pool<Xs>()...};
        return bonded_view_t<except<Xs...>, Ts...>(pools, excludes, bond);
    }

    template <class... Ts>
        requires(sizeof...(Ts) >= 2)
    [[nodiscard]] bonded_view_t<except<>, Ts...> bonded()
    {
        return bonded<Ts...>(except<>{});
    }

    // Const worlds yield all-const views (observed parts become
    // maybe<const T>, hence const T* row parts).
    template <class... Ts, class... Xs>
        requires(sizeof...(Ts) >= 2)
    [[nodiscard]] auto bonded(except<Xs...> tag) const
    {
        // The view only reads through these pointers (const payload parts).
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        auto* self = const_cast<world*>(this);
        return self->template bonded<typename detail::as_const_part<Ts>::type...>(tag);
    }

    template <class... Ts>
        requires(sizeof...(Ts) >= 2)
    [[nodiscard]] auto bonded() const
    {
        return bonded<Ts...>(except<>{});
    }

    // ----------------------------------------------------------------- hooks
    //
    // See the hook_token block near the top of the header for the contract.
    // Connecting/disconnecting is structural: refused while T's pool iterates.

    template <component T>
    hook_token on_add(component_hook fn, void* user = nullptr)
    {
        return connect_hook<T>(detail::pool_base::hook_kind::add, fn, user);
    }

    // Compile-time candidate: free function or capture-less callable, taking
    // (world&, entity) or (entity).
    template <component T, auto Candidate>
    hook_token on_add()
    {
        return connect_hook<T>(
            detail::pool_base::hook_kind::add, detail::free_hook_thunk<Candidate>(), nullptr);
    }

    // Member function (or instance-first callable) bound to an instance that
    // must outlive the hook. A const instance requires a const-invocable
    // candidate — enforced at compile time.
    template <component T, auto Candidate, class Inst>
    hook_token on_add(Inst* instance)
    {
        return connect_hook<T>(
            detail::pool_base::hook_kind::add,
            detail::bound_hook_thunk<Candidate, Inst>(),
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) -- restored at dispatch
            const_cast<void*>(static_cast<const void*>(instance)));
    }

    // Fires before the component is destroyed (remove, kill, purge, reset,
    // command-buffer replay — but not world destruction).
    template <component T>
    hook_token on_remove(component_hook fn, void* user = nullptr)
    {
        return connect_hook<T>(detail::pool_base::hook_kind::remove, fn, user);
    }

    template <component T, auto Candidate>
    hook_token on_remove()
    {
        return connect_hook<T>(
            detail::pool_base::hook_kind::remove, detail::free_hook_thunk<Candidate>(), nullptr);
    }

    template <component T, auto Candidate, class Inst>
    hook_token on_remove(Inst* instance)
    {
        return connect_hook<T>(
            detail::pool_base::hook_kind::remove,
            detail::bound_hook_thunk<Candidate, Inst>(),
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) -- restored at dispatch
            const_cast<void*>(static_cast<const void*>(instance)));
    }

    // Fires after replace<T> or the replacing half of put<T>. Writing through
    // get()/find()/iteration references is invisible to hooks by design — if
    // you want observed writes, funnel them through replace/put.
    template <component T>
    hook_token on_replace(component_hook fn, void* user = nullptr)
    {
        static_assert(!detail::is_tag_v<T>,
                      "quiver: tags carry no data and are never replaced; use on_add/on_remove");
        return connect_hook<T>(detail::pool_base::hook_kind::replace, fn, user);
    }

    template <component T, auto Candidate>
    hook_token on_replace()
    {
        static_assert(!detail::is_tag_v<T>,
                      "quiver: tags carry no data and are never replaced; use on_add/on_remove");
        return connect_hook<T>(
            detail::pool_base::hook_kind::replace, detail::free_hook_thunk<Candidate>(), nullptr);
    }

    template <component T, auto Candidate, class Inst>
    hook_token on_replace(Inst* instance)
    {
        static_assert(!detail::is_tag_v<T>,
                      "quiver: tags carry no data and are never replaced; use on_add/on_remove");
        return connect_hook<T>(
            detail::pool_base::hook_kind::replace,
            detail::bound_hook_thunk<Candidate, Inst>(),
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) -- restored at dispatch
            const_cast<void*>(static_cast<const void*>(instance)));
    }

    // Returns whether the token was found. Stale or empty tokens are a no-op.
    // Structural like connect: refused while the pool iterates or dispatches.
    bool unhook(hook_token token) noexcept
    {
        if (!token || token.pool >= pools_.size() || !pools_[token.pool])
        {
            return false;
        }
        if constexpr (checks_enabled)
        {
            if (pools_[token.pool]->locked())
            {
                detail::violate_pool("hook change during iteration over pool",
                                     pools_[token.pool]->name());
                return false;
            }
        }
        return pools_[token.pool]->disconnect_hook(token.id);
    }

    // --------------------------------------------------------------- queries

    // Builds a selection. Mark types const for read-only access. On a
    // non-const world missing pools are created; on a const world the
    // selection comes back all-const, and a missing pool yields an empty
    // selection instead (no entity has that component). One deduced-this body
    // serves both.
    template <class... Ts, class Self>
    [[nodiscard]] auto select(this Self&& self)
    {
        return self.template select<Ts...>(except<>{});
    }

    template <class... Ts, class... Xs, class Self>
    [[nodiscard]] auto select(this Self&& self, except<Xs...>)
    {
        if constexpr (std::is_const_v<std::remove_reference_t<Self>>)
        {
            // The typename is required here: a dependent template argument is
            // not one of C++20's typename-optional contexts.
            // NOLINTNEXTLINE(readability-redundant-typename)
            using result = selection_t<except<Xs...>, typename detail::as_const_part<Ts>::type...>;
            // Missing include pools propagate as null -> an empty selection.
            return result{self.template flat_include_pools<Ts...>(),
                          {self.template pool_base_of<Xs>()...}};
        }
        else
        {
            using result = selection_t<except<Xs...>, Ts...>;
            // maybe<T> contributes its inner type's pool, any_of<As...> one
            // pool per alternative; registering keeps the selection's pointer
            // array complete (a never-added member stays empty).
            return result{self.template flat_include_pools<Ts...>(),
                          {&self.template ensure_pool<Xs>()...}};
        }
    }

    // Manifest forms: build the same queries from a types<> list, so
    // component sets named once (using Movers = types<Transform, Velocity>)
    // drive selections, archives, and tools alike.
    template <class... Ts, class... Xs, class Self>
    [[nodiscard]] auto select(this Self&& self, types<Ts...>, except<Xs...> x)
    {
        return self.template select<Ts...>(x);
    }

    template <class... Ts, class Self>
    [[nodiscard]] auto select(this Self&& self, types<Ts...>)
    {
        return self.template select<Ts...>(except<>{});
    }

    // One-shot iteration sugar: world.each<A, B>(fn).
    template <class... Ts, class F, class Self>
    void each(this Self&& self, F&& fn)
    {
        self.template select<Ts...>().each(std::forward<F>(fn));
    }

    template <class... Ts, class... Xs, class F, class Self>
    void each(this Self&& self, except<Xs...> x, F&& fn)
    {
        self.template select<Ts...>(x).each(std::forward<F>(fn));
    }

    template <class... Ts, class F, class Self>
    void each(this Self&& self, types<Ts...> list, F&& fn)
    {
        self.select(list).each(std::forward<F>(fn));
    }

    template <class... Ts, class... Xs, class F, class Self>
    void each(this Self&& self, types<Ts...> list, except<Xs...> x, F&& fn)
    {
        self.select(list, x).each(std::forward<F>(fn));
    }

    // -------------------------------------------------------- command buffer

    // O(recorded ops). Replays a buffer: provisional spawns become real entities first (in
    // recording order), then every op runs in recording order, each gated on
    // its target being alive at that moment (dead targets are skipped and
    // counted). The buffer is cleared and ready for reuse afterwards.
    // Checked builds refuse to apply while any iteration is running.
    apply_result apply(command_buffer& buffer)
    {
        if (!apply_allowed())
        {
            return {};
        }
        const apply_result result = apply_replay(buffer);
        buffer.clear();
        return result;
    }

    // Like apply(buffer), and additionally reports what each deferred spawn
    // became: on_spawn(provisional, real) is invoked once per cmd.spawn(), in
    // recording order, before the buffer is cleared. For rollback/replay and
    // spawn-acknowledgement systems that need the mapping.
    template <class F>
    apply_result apply(command_buffer& buffer, F&& on_spawn)
    {
        static_assert(std::invocable<F&, entity, entity>,
                      "quiver: the apply() spawn callback must be callable as "
                      "(entity provisional, entity real)");
        if (!apply_allowed())
        {
            return {};
        }
        const apply_result result = apply_replay(buffer);
        std::uint32_t ticket = 0;
        // Index loop for the same reason as apply_replay; ops recorded from
        // inside on_spawn never apply (the clear below is terminal).
        for (std::size_t i = 0; i < buffer.ops_.size(); ++i)
        {
            const command_buffer::op o = buffer.ops_[i];
            if (o.kind == command_buffer::op_kind::spawn)
            {
                on_spawn(o.target, buffer.resolved_[ticket++]);
            }
        }
        buffer.clear();
        return result;
    }

    // ---------------------------------------------------------- relationships
    //
    // Optional, minimal parent/child links: no transform propagation, no scene
    // graph. Destroying an entity unlinks it (its children become parentless
    // roots; nothing else is destroyed — use a command_buffer if you want
    // recursive destruction).

    // Makes child a child of parent (re-parenting if it already had one).
    // O(1) (checked builds add an O(depth) ancestor walk to refuse cycles).
    void adopt(entity parent, entity child)
    {
        if (!table_.alive(parent) || !table_.alive(child) || parent == child || clearing_ ||
            parent == dying_ || child == dying_)
        {
            if constexpr (checks_enabled)
            {
                detail::violate(
                    "adopt with dead/dying/stale handles, during reset, or "
                    "parent == child");
            }
            return;
        }
        if constexpr (checks_enabled)
        {
            for (entity up = parent_of(parent); up != no_entity; up = parent_of(up))
            {
                if (up == child)
                {
                    detail::violate("adopt would create a parent/child cycle");
                    return;
                }
            }
        }
        auto& pool = ensure_pool<detail::kin>();
        if constexpr (checks_enabled)
        {
            if (pool.locked())
            {
                detail::violate("adopt while children_of is iterating");
                return;
            }
        }
        detail::kin* child_k = pool.at(child.index());
        if (child_k == nullptr)
        {
            child_k = &pool.emplace(child);
        }
        else if (child_k->parent != no_entity)
        {
            detail::kin_links::unlink(pool, *child_k);
        }
        detail::kin* parent_k = pool.at(parent.index());
        if (parent_k == nullptr)
        {
            // Stable storage: emplacing the parent's record cannot move child_k.
            parent_k = &pool.emplace(parent);
        }
        child_k->parent = parent;
        child_k->prev_sibling = no_entity;
        child_k->next_sibling = parent_k->first_child;
        if (parent_k->first_child != no_entity)
        {
            pool.at(parent_k->first_child.index())->prev_sibling = child;
        }
        parent_k->first_child = child;
    }

    // Detaches child from its parent (no-op when it has none). O(1).
    void orphan(entity child)
    {
        if (!table_.alive(child))
        {
            if constexpr (checks_enabled)
            {
                detail::violate("orphan on a dead, stale, or null entity handle");
            }
            return;
        }
        auto* pool = peek_pool<detail::kin>();
        if (pool == nullptr)
        {
            return;
        }
        detail::kin* k = pool->at(child.index());
        if (k == nullptr || k->parent == no_entity)
        {
            return;
        }
        if constexpr (checks_enabled)
        {
            if (pool->locked())
            {
                detail::violate("orphan while children_of is iterating");
                return;
            }
        }
        detail::kin_links::unlink(*pool, *k);
        k->parent = no_entity;
        k->prev_sibling = no_entity;
        k->next_sibling = no_entity;
    }

    [[nodiscard]] entity parent_of(entity child) const noexcept
    {
        const auto* pool = peek_pool<detail::kin>();
        if (pool == nullptr || !table_.alive(child))
        {
            return no_entity;
        }
        const detail::kin* k = pool->at(child.index());
        return k == nullptr ? no_entity : k->parent;
    }

    // Visits direct children (newest first), O(children). fn may return bool
    // to stop early.
    // Structural link edits (adopt/orphan/kill of a linked entity) are blocked
    // while this runs.
    template <class F>
    void children_of(entity parent, F&& fn) const
    {
        const auto* pool = peek_pool<detail::kin>();
        if (pool == nullptr || !table_.alive(parent))
        {
            return;
        }
        const detail::kin* k = pool->at(parent.index());
        if (k == nullptr)
        {
            return;
        }
        const detail::single_pool_lock lock(pool);  // unwinds on early exit and exceptions
        for (entity child = k->first_child; child != no_entity;)
        {
            const entity next = pool->at(child.index())->next_sibling;
            if constexpr (std::predicate<F&, entity>)
            {
                if (!fn(child))
                {
                    break;
                }
            }
            else
            {
                fn(child);
            }
            child = next;
        }
    }

    // O(children); symmetry with the selection count vocabulary.
    [[nodiscard]] std::size_t child_count(entity parent) const noexcept
    {
        std::size_t n = 0;
        children_of(parent, [&](entity) { ++n; });
        return n;
    }

    // -------------------------------------------------------------- snapshot
    //
    // quiver does not serialize; it gives you deterministic primitives to
    // build save/load, rollback, or replay on top:
    //   - live_entities() walks live handles in slot order (stable),
    //   - selections iterate in dense order (stable between structural edits),
    //   - restore_entity() re-creates an exact handle in a fresh/loading world,
    //     after which add<T> repopulates components.

    // Visits every live entity in increasing slot order, O(slots). fn may return bool to
    // stop early.
    template <class F>
    void live_entities(F&& fn) const
    {
        for (std::uint32_t slot = 0; slot < table_.slots(); ++slot)
        {
            if (!table_.occupied(slot))
            {
                continue;
            }
            const entity e{slot, table_.generation_at(slot)};
            if constexpr (std::predicate<F&, entity>)
            {
                if (!fn(e))
                {
                    break;
                }
            }
            else
            {
                fn(e);
            }
        }
    }

    // Re-creates an entity with an exact index and generation. Errors (rather
    // than asserts: this is the load path, and load data is untrusted) when
    // the handle is malformed or the slot is occupied by a live entity.
    // O(1) amortized, plus table growth up to the requested index — validate
    // indices against your expected world size before feeding hostile data in.
    // Restoring a generation lower than the slot has seen is the rollback
    // semantic: handles issued after the snapshot may then alias new entities,
    // so discard them.
    std::expected<entity, fault> restore_entity(entity e) { return table_.restore(e); }

    // ------------------------------------------------------------- world ops

    // Destroys all components and entities, O(everything). Generations are preserved and
    // bumped, so pre-reset handles read as dead rather than aliasing new
    // entities. Pools and selections built on them remain usable (empty).
    // Hooks and their tokens survive; on_remove fires per live component
    // during the reset, and hook ADDS during it are refused (the entities
    // are about to die).
    void reset()
    {
        if constexpr (checks_enabled)
        {
            if (any_locked())
            {
                detail::violate("reset while an iteration is running");
                return;
            }
        }
        // Partitions zero BEFORE the wipes: every intersection is doomed, and
        // an on_remove hook fired mid-reset that reads a bonded view must see
        // it empty, not a stale partition over an already-wiped partner.
        // Bonds survive reset, like hooks; repopulation re-pairs.
        for (const auto& bond : bonds_)
        {
            bond->paired = 0;
        }
        // Index loop (hooks may register new pools); clearing_ refuses hook
        // adds that would otherwise outlive the entities reset is destroying.
        clearing_ = true;
        // NOLINTNEXTLINE(modernize-loop-convert) -- tolerates active_ growth from hooks
        for (std::size_t i = 0; i < active_.size(); ++i)
        {
            active_[i]->wipe();
        }
        clearing_ = false;
        table_.destroy_all();
    }

    // Returns slack memory. Stable-storage pointers survive (chunks are never
    // freed), but packed pools may reallocate their dense arrays: treat every
    // outstanding pointer into a packed pool as invalidated. O(everything).
    void shrink()
    {
        if constexpr (checks_enabled)
        {
            if (any_locked())
            {
                detail::violate("shrink while an iteration is running");
                return;
            }
        }
        for (detail::pool_base* pool : active_)
        {
            pool->compact();
        }
        table_.shrink();
    }

    // Total slots ever allocated (live + recycled). For "how many entities
    // exist", use live_count().
    [[nodiscard]] std::size_t slot_count() const noexcept { return table_.slots(); }
    [[nodiscard]] std::size_t live_count() const noexcept { return table_.live(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return table_.capacity(); }

    // Visits a pool_info per registered pool, O(pools) — enough for an editor overlay or
    // a stats HUD, with no RTTI and no reflection.
    template <class F>
    void each_pool(F&& fn) const
    {
        for (const detail::pool_base* pool : active_)
        {
            fn(pool->info());
        }
    }

    // The other inspector axis: which components does THIS entity have?
    // Visits a pool_info per pool containing e, O(pools this world uses).
    // Feeds entity-inspector overlays and debug dumps.
    template <class F>
    void components_of(entity e, F&& fn) const
    {
        if (!table_.alive(e))
        {
            return;
        }
        for (const detail::pool_base* pool : active_)
        {
            if (pool->contains(e.index()))
            {
                fn(pool->info());
            }
        }
    }

    // Aggregated memory accounting, O(pools). Counts capacity (allocated),
    // not size (used) — that is what a budget dashboard needs.
    [[nodiscard]] memory_footprint footprint() const noexcept
    {
        memory_footprint f{};
        f.entity_table_bytes = table_.bytes();
        for (const detail::pool_base* pool : active_)
        {
            const pool_info info = pool->info();
            f.component_bytes += info.capacity * info.bytes_per_item;
            f.index_bytes += info.index_bytes;
            f.bookkeeping_bytes += info.bookkeeping_bytes;
        }
        return f;
    }

    // --- runtime pool addressing (feeds runtime_selection and tooling) ---

    // By compile-time type: the typed bridge into the runtime layer.
    template <component T>
    [[nodiscard]] pool_ref find_pool() const noexcept
    {
        return pool_ref{peek_pool<T>()};
    }

    // By dense type id (as seen in pool_info::id). O(1).
    [[nodiscard]] pool_ref find_pool(std::uint32_t type_id) const noexcept
    {
        return pool_ref{type_id < pools_.size() ? pools_[type_id].get() : nullptr};
    }

    // By stable name hash (hash_of<T>(), as seen in pool_info::name_hash) —
    // the form save files and tools should persist. O(pools this world uses).
    [[nodiscard]] pool_ref find_pool_by_hash(std::uint64_t name_hash) const noexcept
    {
        for (const detail::pool_base* pool : active_)
        {
            if (pool->name_hash() == name_hash)
            {
                return pool_ref{pool};
            }
        }
        return {};
    }

    // Full consistency audit: entity table bookkeeping, every pool's
    // sparse/dense mapping, stable pools' slot accounting, parent/child link
    // integrity. O(everything); meant for tests, asserts, and tools.
    [[nodiscard]] std::expected<void, fault> validate() const
    {
        if (auto r = table_.check(); !r)
        {
            return r;
        }
        for (const detail::pool_base* pool : active_)
        {
            if (auto r = pool->check(table_); !r)
            {
                return r;
            }
        }
        if (auto r = check_bonds(); !r)
        {
            return r;
        }
        if (const auto* marks = peek_pool<globals_mark>(); marks != nullptr && marks->size() > 1)
        {
            return std::unexpected(
                fault{fault_code::globals_broken, marks->name(), "more than one globals entity"});
        }
        return check_links();
    }

private:
    friend struct test_access;
    friend class scoped_hook;  // resolves its pool anchor through the token

    // The pool a hook token belongs to; null for empty/stale tokens. Pools
    // are heap-stable for the world's life, so the pointer survives world
    // moves (the connections move with the pools).
    [[nodiscard]] detail::pool_base* pool_for_token(hook_token token) noexcept
    {
        if (!token || token.pool >= pools_.size() || !pools_[token.pool])
        {
            return nullptr;
        }
        return pools_[token.pool].get();
    }

    // Membership-based, not cache-based: an unpack-restored globals entity is
    // protected before the first globals() call repopulates the cache.
    [[nodiscard]] bool is_globals(entity e) const noexcept
    {
        if (e == globals_)
        {
            return true;
        }
        const auto* marks = peek_pool<globals_mark>();
        return marks != nullptr && marks->contains(e.index());
    }

    template <component T>
    pool_of_t<T>& ensure_pool()
    {
        static_assert(component_pool<pool_of_t<T>>,
                      "quiver: pool_of<T> specializations must derive quiver::basic_pool (via "
                      "packed_pool_of / stable_pool_of / tag_pool_of, or from scratch) and "
                      "construct from a std::pmr::memory_resource* — see the pool_of seam "
                      "block");
        const std::uint32_t id = detail::type_id<T>();
        if (id >= pools_.size())
        {
            pools_.resize(id + 1);
        }
        if (!pools_[id])
        {
            auto pool = std::make_unique<pool_of_t<T>>(memory_);
            pool->id_ = id;
            pool->owner_ = this;
            if constexpr (checks_enabled)
            {
                // Cold, once per type: a name-hash collision would corrupt any
                // snapshot keyed on hash_of, so make it loud immediately.
                for (const detail::pool_base* existing : active_)
                {
                    if (existing->name_hash() == pool->name_hash())
                    {
                        detail::violate_pool("component name hash collision with pool",
                                             existing->name());
                    }
                }
            }
            active_.push_back(pool.get());
            pools_[id] = std::move(pool);
        }
        return static_cast<pool_of_t<T>&>(*pools_[id]);
    }

    // Never registers; null when the world has not seen T. Safe on const
    // worlds and in hot paths.
    template <component T>
    [[nodiscard]] pool_of_t<T>* peek_pool() const noexcept
    {
        const std::uint32_t id = detail::type_id<T>();
        if (id >= pools_.size())
        {
            return nullptr;
        }
        return static_cast<pool_of_t<T>*>(pools_[id].get());
    }

    template <component T>
    [[nodiscard]] detail::pool_base* pool_base_of() const noexcept
    {
        return peek_pool<T>();
    }

    // The post-liveness-check half of has, for has_all/has_any's single
    // liveness gate. An unregistered pool counts as absent.
    template <component T>
    [[nodiscard]] bool probe(std::uint32_t index) const noexcept
    {
        const auto* pool = peek_pool<T>();
        return pool != nullptr && pool->contains(index);
    }

    // Builds a selection's flattened include array: one pool per element,
    // except any_of elements which contribute one pool per alternative.
    // Non-const worlds register pools; const worlds leave missing ones null.
    template <class... Es, class Self>
    [[nodiscard]] auto flat_include_pools(this Self&& self)
    {
        constexpr std::size_t n =
            (std::size_t{0} + ... +
             detail::any_of_traits<detail::bare<detail::maybe_inner<Es>>>::width);
        std::array<detail::pool_base*, n> out{};
        std::size_t at = 0;
        (self.template fill_flat<Es>(out, at), ...);
        return out;
    }

    template <class E, std::size_t N, class Self>
    void fill_flat(this Self&& self, std::array<detail::pool_base*, N>& out, std::size_t& at)
    {
        using core = detail::bare<detail::maybe_inner<E>>;
        constexpr bool from_const = std::is_const_v<std::remove_reference_t<Self>>;
        if constexpr (detail::is_any_of_v<core>)
        {
            [&]<class... As>(types<As...>)
            {
                if constexpr (from_const)
                {
                    ((out[at++] = self.template pool_base_of<detail::bare<As>>()), ...);
                }
                else
                {
                    ((out[at++] = &self.template ensure_pool<detail::bare<As>>()), ...);
                }
            }(typename detail::any_of_traits<core>::alts{});
        }
        else
        {
            if constexpr (from_const)
            {
                out[at++] = self.template pool_base_of<core>();
            }
            else
            {
                out[at++] = &self.template ensure_pool<core>();
            }
        }
    }

    // The group whose owned set is EXACTLY the given pools (any order); null
    // when any pool is missing, unbonded, or the sets differ.
    template <std::size_t N>
    [[nodiscard]] static detail::group_core* standing_bond(
        const std::array<detail::pool_base*, N>& pools) noexcept
    {
        for (detail::pool_base* pool : pools)
        {
            if (pool == nullptr)
            {
                return nullptr;
            }
        }
        detail::group_core* bond = pools[0]->bond_;
        if (bond == nullptr || bond->owner_count != N)
        {
            return nullptr;
        }
        for (detail::pool_base* pool : pools)
        {
            if (!bond->owns(pool))
            {
                return nullptr;
            }
        }
        return bond;
    }

    // Constness flows from the world: const Self yields const T*.
    template <component T, class Self>
    [[nodiscard]] auto* lookup(this Self&& self, entity e) noexcept
    {
        using pointer =
            std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const T*, T*>;
        if (!self.table_.alive(e))
        {
            return pointer{nullptr};
        }
        auto* pool = self.template peek_pool<T>();
        return pool == nullptr ? pointer{nullptr} : pointer{pool->at(e.index())};
    }

    // The post-liveness-check half of lookup, for find_all's single check.
    template <component T, class Self>
    [[nodiscard]] auto* find_in_pool(this Self&& self, entity e) noexcept
    {
        static_assert(!detail::is_tag_v<T>,
                      "quiver: T uses tag storage and carries no data; use has<T>(e)");
        using pointer =
            std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const T*, T*>;
        auto* pool = self.template peek_pool<T>();
        return pool == nullptr ? pointer{nullptr} : pointer{pool->at(e.index())};
    }

    template <class C>
    void add_value(entity e, C&& value)
    {
        using T = std::remove_cvref_t<C>;
        if constexpr (detail::is_tag_v<T>)
        {
            add<T>(e);
        }
        else
        {
            add<T>(e, std::forward<C>(value));
        }
    }

    [[nodiscard]] bool any_locked() const noexcept
    {
        for (const detail::pool_base* pool : active_)
        {
            if (pool->locked())
            {
                return true;
            }
        }
        return false;
    }

    // Hooks receive the owning world; moves must re-aim the back-pointers.
    void repoint_pools() noexcept
    {
        for (detail::pool_base* pool : active_)
        {
            pool->owner_ = this;
        }
    }

    template <component T>
    hook_token connect_hook(detail::pool_base::hook_kind kind, component_hook fn, void* user)
    {
        auto& pool = ensure_pool<T>();
        if constexpr (checks_enabled)
        {
            if (fn == nullptr)
            {
                detail::violate_pool("null hook for pool", pool.name());
                return {};
            }
            if (pool.locked())
            {
                detail::violate_pool("hook change during iteration over pool", pool.name());
                return {};
            }
        }
        return hook_token{detail::type_id<T>(), pool.connect_hook(kind, fn, user)};
    }

    // The shared pre-check for both apply overloads: refused (with the buffer
    // left intact for a retry) while any iteration is running.
    [[nodiscard]] bool apply_allowed() const
    {
        if constexpr (checks_enabled)
        {
            for (const detail::pool_base* pool : active_)
            {
                if (pool->locked())
                {
                    detail::violate_pool("apply while iterating pool", pool->name());
                    return false;
                }
            }
        }
        return true;
    }

    // Replays the buffer without clearing it: every op runs in recording
    // order, gated on a generation-checked alive(target); provisional spawns
    // resolve to real entities on encounter (recording order guarantees a
    // spawn precedes every op that uses its handle, and tickets are issued
    // in append order, so lazy resolution stays exact). Callers clear()
    // afterwards.
    apply_result apply_replay(command_buffer& buffer)
    {
        buffer.resolved_.clear();
        apply_result result{};
        // Index loop, ops copied out by value: a hook fired by an applied op
        // may legally record MORE ops into this same buffer (the documented
        // escape hatch for structural work from hooks) — they replay in this
        // same pass, and the caller's terminal clear() destroys exactly what
        // ran. Payload pointers stay valid throughout (the arena never
        // relocates).
        for (std::size_t i = 0; i < buffer.ops_.size(); ++i)
        {
            const command_buffer::op o = buffer.ops_[i];
            entity target = o.target;
            if (o.kind == command_buffer::op_kind::spawn)
            {
                buffer.resolved_.push_back(table_.create());
                ++result.applied;
                continue;
            }
            if (detail::is_provisional(target))
            {
                // The nonce stamped into the handle must match the applying
                // buffer: handles from another buffer, or recorded before a
                // clear(), are refused rather than silently mis-resolved.
                const std::uint32_t ticket = target.index() & ~detail::provisional_bit;
                if (target.generation() != buffer.nonce_ || ticket >= buffer.resolved_.size())
                {
                    if constexpr (checks_enabled)
                    {
                        detail::violate(
                            "command_buffer: provisional handle from another buffer "
                            "or from before clear()");
                    }
                    ++result.skipped;
                    continue;
                }
                target = buffer.resolved_[ticket];
            }
            if (!table_.alive(target))
            {
                ++result.skipped;
                continue;
            }
            if (o.kind == command_buffer::op_kind::kill)
            {
                kill(target);
            }
            else
            {
                o.apply_fn(*this, target, o.payload);
            }
            ++result.applied;
        }
        return result;
    }

    // Thin shims over detail::kin_links so call sites read as policy.
    void sever_links(entity e) noexcept
    {
        if (auto* pool = peek_pool<detail::kin>())
        {
            detail::kin_links::sever(*pool, e);
        }
    }

    [[nodiscard]] std::expected<void, fault> check_bonds() const
    {
        for (const auto& bond : bonds_)
        {
            if (bond->owner_count == 0)  // tombstoned by unbond
            {
                continue;
            }
            const detail::pool_base& first = *bond->owners[0];
            for (std::uint32_t i = 0; i < bond->owner_count; ++i)
            {
                if (bond->paired > bond->owners[i]->size())
                {
                    return std::unexpected(fault{fault_code::bond_broken,
                                                 bond->owners[i]->name(),
                                                 "partition exceeds pool size"});
                }
            }
            // Mirror equality: the same entity at the same position in every
            // owner, across the whole partition.
            for (std::uint32_t pos = 0; pos < bond->paired; ++pos)
            {
                const entity e = first.entity_at(pos);
                for (std::uint32_t i = 1; i < bond->owner_count; ++i)
                {
                    if (bond->owners[i]->entity_at(pos) != e)
                    {
                        return std::unexpected(fault{fault_code::bond_broken,
                                                     bond->owners[i]->name(),
                                                     "partition out of mirror"});
                    }
                }
            }
            // Completeness: no intersection member may sit beyond the
            // partition. Walk the smallest owner's tail probing all others.
            const detail::pool_base* smallest = &first;
            for (std::uint32_t i = 1; i < bond->owner_count; ++i)
            {
                if (bond->owners[i]->size() < smallest->size())
                {
                    smallest = bond->owners[i];
                }
            }
            for (std::size_t pos = bond->paired; pos < smallest->size(); ++pos)
            {
                const std::uint32_t index = smallest->entity_at(pos).index();
                bool in_all = true;
                for (std::uint32_t i = 0; i < bond->owner_count; ++i)
                {
                    in_all =
                        in_all && (bond->owners[i] == smallest || bond->owners[i]->contains(index));
                }
                if (in_all)
                {
                    return std::unexpected(fault{fault_code::bond_broken,
                                                 smallest->name(),
                                                 "intersection member outside the partition"});
                }
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, fault> check_links() const
    {
        const auto* pool = peek_pool<detail::kin>();
        return pool == nullptr ? std::expected<void, fault>{}
                               : detail::kin_links::check(*pool, table_);
    }

    detail::entity_table table_;
    std::pmr::vector<std::unique_ptr<detail::pool_base>> pools_;  // indexed by type_id (holes ok)
    // Dense, creation-order view of the same pools. The pools_ vector is sized
    // by the largest process-global type id this world has touched, so worlds
    // in multi-world processes carry null holes there; every O(pools) walk
    // (kill, apply, reset, shrink, each_pool, validate) uses this list instead.
    std::pmr::vector<detail::pool_base*> active_;
    // Bond objects are heap-stable for the life of the world; unbond
    // tombstones instead of freeing so stored bonded_views degrade to empty.
    std::pmr::vector<std::unique_ptr<detail::group_core>> bonds_;
    std::pmr::memory_resource* memory_;
    // Guards against hooks resurrecting state on doomed entities (see add()):
    entity dying_;           // the entity kill() is currently sweeping
    bool clearing_ = false;  // reset() is wiping pools
    entity globals_;         // cached globals() carrier; revalidated on use
};

// ----------------------------------------------------------------------------
// Scoped hooks
//
// RAII over a hook_token: disconnects on destruction, so a system object can
// own its hook connections without manual teardown. Move-only. Anchored to
// the POOL (heap-stable for the world's life), not the world object — so the
// connection releases correctly even after the world is moved elsewhere.
// Releasing while the pool iterates (including from inside the hook's own
// dispatch) is refused with a violation and the token is KEPT: release again
// after the iteration ends, or let a later destructor do it.
// ----------------------------------------------------------------------------

class scoped_hook
{
public:
    scoped_hook() = default;

    scoped_hook(world& w, hook_token token) noexcept
        : pool_(w.pool_for_token(token)),
          token_(token)
    {
    }

    scoped_hook(scoped_hook&& other) noexcept
        : pool_(std::exchange(other.pool_, nullptr)),
          token_(std::exchange(other.token_, hook_token{}))
    {
    }

    scoped_hook& operator=(scoped_hook&& other) noexcept
    {
        if (this != &other)
        {
            release();
            pool_ = std::exchange(other.pool_, nullptr);
            token_ = std::exchange(other.token_, hook_token{});
        }
        return *this;
    }

    scoped_hook(const scoped_hook&) = delete;
    scoped_hook& operator=(const scoped_hook&) = delete;

    ~scoped_hook() { release(); }

    // Disconnect now; idempotent. Refused (token kept) while the pool
    // iterates, so a retry stays possible.
    void release() noexcept
    {
        if (pool_ == nullptr || !token_)
        {
            pool_ = nullptr;
            token_ = hook_token{};
            return;
        }
        if constexpr (checks_enabled)
        {
            if (pool_->locked())
            {
                detail::violate_pool("hook change during iteration over pool", pool_->name());
                return;  // still connected; the token survives for a retry
            }
        }
        pool_->disconnect_hook(token_.id);
        pool_ = nullptr;
        token_ = hook_token{};
    }

    [[nodiscard]] hook_token token() const noexcept { return token_; }
    explicit operator bool() const noexcept { return static_cast<bool>(token_); }

private:
    detail::pool_base* pool_ = nullptr;
    hook_token token_{};
};

// ----------------------------------------------------------------------------
// Trackers
//
// A tracker collects the entities whose T changed since the last drain —
// added, replaced (via replace/put), removed — deduplicated, in event order.
// The drain pattern, run once per frame by whatever system cares:
//
//   quiver::tracker<Health> hurt(world);          // connect once
//   ...
//   for (quiver::entity e : hurt.replaced()) { ui.popup(e); }
//   hurt.clear();
//
// removed() may hold dead handles (kill destroys the entity after the hook
// fires); that is information, not an error — alive()-filter if needed. An
// entity added then removed before a drain appears in both lists. Between
// drains a recycled slot keeps only the LIVE successor in added()/replaced()
// (the dead predecessor's events are moot), while removed() keeps every
// distinct entity's record. On tag components the replaced channel is inert
// (tags carry no data and are never replaced). Trackers are pinned (the
// hooks hold a pointer to the tracker) and must be destroyed before their
// world.
// ----------------------------------------------------------------------------

enum class track : std::uint8_t
{
    added = 1,
    replaced = 2,
    removed = 4,
    all = added | replaced | removed,
};

[[nodiscard]] constexpr track operator|(track a, track b) noexcept
{
    // Flag enum: combined values are intentional.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<track>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr track operator&(track a, track b) noexcept
{
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<track>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

template <component T>
class tracker
{
public:
    explicit tracker(world& w,
                     track which = track::all,
                     std::pmr::memory_resource* memory = std::pmr::get_default_resource())
        : seen_added_(memory),
          seen_replaced_(memory),
          seen_removed_(memory),
          added_(memory),
          replaced_(memory),
          removed_(memory)
    {
        if ((which & track::added) == track::added)
        {
            on_added_ = scoped_hook(w, w.on_add<T>(&tracker::record_added, this));
        }
        if constexpr (!detail::is_tag_v<T>)
        {
            if ((which & track::replaced) == track::replaced)
            {
                on_replaced_ = scoped_hook(w, w.on_replace<T>(&tracker::record_replaced, this));
            }
        }
        if ((which & track::removed) == track::removed)
        {
            on_removed_ = scoped_hook(w, w.on_remove<T>(&tracker::record_removed, this));
        }
    }

    tracker(const tracker&) = delete;  // pinned: the hooks hold `this`
    tracker(tracker&&) = delete;
    tracker& operator=(const tracker&) = delete;
    tracker& operator=(tracker&&) = delete;
    ~tracker() = default;  // scoped_hooks disconnect

    [[nodiscard]] std::span<const entity> added() const noexcept { return added_; }
    [[nodiscard]] std::span<const entity> replaced() const noexcept { return replaced_; }
    [[nodiscard]] std::span<const entity> removed() const noexcept { return removed_; }

    void clear() noexcept
    {
        drain(added_, seen_added_);
        drain(replaced_, seen_replaced_);
        drain(removed_, seen_removed_);
    }

private:
    static void record_added(world&, entity e, void* user)
    {
        auto* self = static_cast<tracker*>(user);
        self->record(self->added_, self->seen_added_, e, false);
    }

    static void record_replaced(world&, entity e, void* user)
    {
        auto* self = static_cast<tracker*>(user);
        self->record(self->replaced_, self->seen_replaced_, e, false);
    }

    static void record_removed(world&, entity e, void* user)
    {
        auto* self = static_cast<tracker*>(user);
        self->record(self->removed_, self->seen_removed_, e, true);
    }

    void record(std::pmr::vector<entity>& list,
                detail::sparse_index& seen,
                entity e,
                bool keep_history)
    {
        const std::uint32_t pos = seen.get(e.index());
        if (pos != detail::npos32)
        {
            // Slot recycled before a drain. For added/replaced the live
            // successor wins (the predecessor's event is moot once it died);
            // for removed() history matters — a DISTINCT entity's removal
            // must not erase its predecessor's, so it appends instead.
            if (!keep_history || list[pos] == e)
            {
                list[pos] = e;
                return;
            }
        }
        seen.ensure(e.index());  // nofail set below
        list.push_back(e);
        seen.set_existing(e.index(), static_cast<std::uint32_t>(list.size() - 1));
    }

    static void drain(std::pmr::vector<entity>& list, detail::sparse_index& seen) noexcept
    {
        for (const entity e : list)
        {
            seen.erase(e.index());
        }
        list.clear();
    }

    detail::sparse_index seen_added_;  // entity slot -> list position (dedupe)
    detail::sparse_index seen_replaced_;
    detail::sparse_index seen_removed_;
    std::pmr::vector<entity> added_;
    std::pmr::vector<entity> replaced_;
    std::pmr::vector<entity> removed_;
    scoped_hook on_added_;
    scoped_hook on_replaced_;
    scoped_hook on_removed_;
};

// ----------------------------------------------------------------------------
// Watchers
//
// A watcher monitors a CONDITION SET — entities holding every component in
// its types<> list and none in its except<> list — and collects,
// deduplicated, the entities that BEGAN matching since the last drain (and,
// with changed<C> triggers, those whose C was replaced through
// replace/put/amend WHILE matching). Entities that stop matching before the
// drain are evicted, so matched() is live truth at drain time. That is the
// deliberate divergence from the tracker: tracker<T> is a per-pool event
// log whose removed() keeps history; a watcher is a membership monitor.
//
//   quiver::watcher<quiver::types<Burning, Health>> entered(world);
//   quiver::watcher<quiver::types<Burning>, quiver::except<Shielded>> raw(world);
//   quiver::watcher<quiver::types<Burning>, quiver::changed<Health>> hurt(world);
//   for (quiver::entity e : hurt.matched()) { ... }
//   hurt.clear();
//
// Watchers see EDGES: members that predate construction do not enter until
// a fresh edge re-collects them. Only the named pools gain hook entries
// (untouched pools keep their one-pointer test); watchers are pinned (the
// hooks hold `this`) and must be destroyed before their world.
// ----------------------------------------------------------------------------

// Trigger marker: collect entities whose T was replaced (via
// replace/put/amend) while the watcher's condition set held.
template <class T>
struct changed
{
};

namespace detail
{
template <class S>
struct watcher_spec
{
    static constexpr bool is_includes = false;
    static constexpr bool is_excludes = false;
    static constexpr bool is_trigger = false;
    using list = types<>;
};

template <class... Is>
struct watcher_spec<types<Is...>>
{
    static constexpr bool is_includes = true;
    static constexpr bool is_excludes = false;
    static constexpr bool is_trigger = false;
    using list = types<Is...>;
};

template <class... Es>
struct watcher_spec<except<Es...>>
{
    static constexpr bool is_includes = false;
    static constexpr bool is_excludes = true;
    static constexpr bool is_trigger = false;
    using list = types<Es...>;
};

template <class C>
struct watcher_spec<changed<C>>
{
    static constexpr bool is_includes = false;
    static constexpr bool is_excludes = false;
    static constexpr bool is_trigger = true;
    using list = types<C>;
};

template <class List>
struct list_size;

template <class... Us>
struct list_size<types<Us...>>
{
    static constexpr std::size_t value = sizeof...(Us);
};

template <class A, class B>
struct lists_disjoint;

template <class... As, class... Bs>
struct lists_disjoint<types<As...>, types<Bs...>>
{
    static constexpr bool value = (!type_among<As, Bs...> && ...);
};

template <class List>
struct watcher_conditions_ok;

template <class... Is>
struct watcher_conditions_ok<types<Is...>>
{
    static_assert((component<Is> && ...),
                  "quiver: watcher condition types must be plain, non-const component types");
    static_assert(all_distinct<Is...>, "quiver: duplicate component type in a watcher list");
    static constexpr bool value = true;
};

template <class List>
struct watcher_triggers_ok;

template <class... Cs>
struct watcher_triggers_ok<types<Cs...>>
{
    static_assert((component<Cs> && ...),
                  "quiver: changed<T> takes a plain, non-const component type");
    static_assert((!is_tag_v<Cs> && ...),
                  "quiver: changed<T> on a tag is inert — tags carry no data and are never "
                  "replaced");
    static_assert(all_distinct<Cs...>, "quiver: duplicate changed<> trigger");
    static constexpr bool value = true;
};
}  // namespace detail

template <class... Specs>
class watcher
{
    static_assert((std::size_t{0} + ... + std::size_t{detail::watcher_spec<Specs>::is_includes}) ==
                      1,
                  "quiver: a watcher takes exactly one types<...> condition list");
    static_assert((std::size_t{0} + ... + std::size_t{detail::watcher_spec<Specs>::is_excludes}) <=
                      1,
                  "quiver: at most one except<...> list per watcher");
    static_assert(((detail::watcher_spec<Specs>::is_includes ||
                    detail::watcher_spec<Specs>::is_excludes ||
                    detail::watcher_spec<Specs>::is_trigger) &&
                   ...),
                  "quiver: watcher specs are types<...>, except<...>, and changed<T>");

    using include_list = joined_t<std::conditional_t<detail::watcher_spec<Specs>::is_includes,
                                                     typename detail::watcher_spec<Specs>::list,
                                                     types<>>...>;
    using exclude_list = joined_t<std::conditional_t<detail::watcher_spec<Specs>::is_excludes,
                                                     typename detail::watcher_spec<Specs>::list,
                                                     types<>>...>;
    using trigger_list = joined_t<std::conditional_t<detail::watcher_spec<Specs>::is_trigger,
                                                     typename detail::watcher_spec<Specs>::list,
                                                     types<>>...>;

    static_assert(detail::list_size<include_list>::value >= 1,
                  "quiver: a watcher needs at least one condition component");
    static_assert(detail::watcher_conditions_ok<include_list>::value);
    static_assert(detail::watcher_conditions_ok<exclude_list>::value);
    static_assert(detail::watcher_triggers_ok<trigger_list>::value);
    static_assert(detail::lists_disjoint<include_list, exclude_list>::value,
                  "quiver: a component type appears in both the watcher's types<...> and "
                  "except<...> lists");

    static constexpr std::size_t hook_count = (detail::list_size<include_list>::value * 2) +
                                              (detail::list_size<exclude_list>::value * 2) +
                                              detail::list_size<trigger_list>::value;

public:
    explicit watcher(world& w, std::pmr::memory_resource* memory = std::pmr::get_default_resource())
        : seen_(memory),
          matched_(memory)
    {
        std::size_t k = 0;
        connect_includes(w, k, include_list{});
        connect_excludes(w, k, exclude_list{});
        connect_triggers(w, k, trigger_list{});
    }

    watcher(const watcher&) = delete;  // pinned: the hooks hold `this`
    watcher(watcher&&) = delete;
    watcher& operator=(const watcher&) = delete;
    watcher& operator=(watcher&&) = delete;
    ~watcher() = default;  // scoped_hooks disconnect

    // Deduplicated; every entry currently matches the condition set.
    [[nodiscard]] std::span<const entity> matched() const noexcept { return matched_; }
    [[nodiscard]] std::size_t count() const noexcept { return matched_.size(); }
    [[nodiscard]] bool empty() const noexcept { return matched_.empty(); }

    [[nodiscard]] bool contains(entity e) const noexcept
    {
        const std::uint32_t pos = seen_.get(e.index());
        return pos != detail::npos32 && matched_[pos] == e;
    }

    void clear() noexcept
    {
        for (const entity e : matched_)
        {
            seen_.erase(e.index());
        }
        matched_.clear();
    }

private:
    template <class... Is>
    void connect_includes(world& w, std::size_t& k, types<Is...>)
    {
        ((hooks_[k] = scoped_hook(w, w.on_add<Is>(&watcher::probe_edge, this)),
          hooks_[k + 1] = scoped_hook(w, w.on_remove<Is>(&watcher::evict_edge, this)),
          k += 2),
         ...);
    }

    template <class... Es>
    void connect_excludes(world& w, std::size_t& k, types<Es...>)
    {
        ((hooks_[k] = scoped_hook(w, w.on_add<Es>(&watcher::evict_edge, this)),
          hooks_[k + 1] = scoped_hook(w, w.on_remove<Es>(&watcher::unshielded_edge<Es>, this)),
          k += 2),
         ...);
    }

    template <class... Cs>
    void connect_triggers(world& w, std::size_t& k, types<Cs...>)
    {
        ((hooks_[k++] = scoped_hook(w, w.on_replace<Cs>(&watcher::probe_edge, this))), ...);
    }

    static void probe_edge(world& w, entity e, void* user)
    {
        auto* self = static_cast<watcher*>(user);
        if (matches<void>(w, e))
        {
            self->insert(e);
        }
    }

    // An exclude's on_remove fires BEFORE the component leaves: probe as if
    // it were already gone.
    template <class SkipX>
    static void unshielded_edge(world& w, entity e, void* user)
    {
        auto* self = static_cast<watcher*>(user);
        if (matches<SkipX>(w, e))
        {
            self->insert(e);
        }
    }

    static void evict_edge(world&, entity e, void* user) { static_cast<watcher*>(user)->evict(e); }

    template <class SkipX>
    [[nodiscard]] static bool matches(world& w, entity e)
    {
        const bool held = []<class... Is>(types<Is...>, world& ww, entity ee)
        { return (ww.template has<Is>(ee) && ...); }(include_list{}, w, e);
        if (!held)
        {
            return false;
        }
        return []<class... Es>(types<Es...>, world& ww, entity ee)
        { return ((std::same_as<Es, SkipX> || !ww.template has<Es>(ee)) && ...); }(
            exclude_list{}, w, e);
    }

    void insert(entity e)
    {
        if (seen_.get(e.index()) != detail::npos32)
        {
            return;  // already collected
        }
        seen_.ensure(e.index());  // nofail set below
        matched_.push_back(e);
        seen_.set_existing(e.index(), static_cast<std::uint32_t>(matched_.size() - 1));
    }

    void evict(entity e) noexcept
    {
        const std::uint32_t pos = seen_.get(e.index());
        if (pos == detail::npos32)
        {
            return;
        }
        const entity last = matched_.back();
        matched_[pos] = last;
        seen_.set_existing(last.index(), pos);
        matched_.pop_back();
        seen_.erase(e.index());
    }

    detail::sparse_index seen_;  // entity slot -> matched_ position (dedupe + O(1) evict)
    std::pmr::vector<entity> matched_;
    std::array<scoped_hook, hook_count> hooks_;
};

// ----------------------------------------------------------------------------
// Archives: pack / unpack / graft
//
// quiver owns ordering and identity; YOUR archive owns the encoding. A writer
// is any object callable with each value quiver emits; a reader mirrors it
// with references. Typed, not byte-oriented — components need not be
// trivially copyable, and versioning/endianness live in your archive where
// they belong.
//
//   pack<Transform, Health, Burning>(world, writer);     // save
//   unpack<Transform, Health, Burning>(fresh, reader);   // load (exact ids)
//   auto map = graft<Transform, Squad>(world, reader);   // merge (remapped ids)
//
// Stream layout: [u64 live count][live handles in slot order], then per type
// in declaration order: [u64 hash_of<T>][u64 count][entity (, value)] rows in
// dense (iteration) order — which round-trips sort<T> ordering. Tags emit
// membership only. List the same types in the same order on both sides.
//
// unpack requires an EMPTY world and re-creates exact index+generation pairs
// (rollback semantics). graft spawns FRESH entities and returns the old→new
// graft_map; component types whose values store entity handles opt into
// relinking explicitly — quiver never guesses at members:
//
//   struct Squad {
//       quiver::entity leader;
//       void quiver_relink(const quiver::graft_map& m) { leader = m.resolve(leader); }
//   };
//
// (or specialize quiver::relink_traits<T> for types you do not own.)
// resolve() answers no_entity for handles outside the archive — dangling
// references are data, but a row's OWNER outside the archive is a hard fault.
// Faults leave already-applied rows in place (not transactional): treat a
// failed world as suspect. Parent/child links never serialize — re-adopt
// after load. Readers signal corruption by throwing (quiver is
// exception-transparent) or by failing the next hash check. Two sharp edges
// on untrusted data: a stream with MORE trailing type sections than the
// unpack list simply stops early (no fault — list the same types on both
// sides), and unpack/graft trust the stream's entity COUNT before any row
// validates — bound it against your expected world size first, like
// restore_entity.
// ----------------------------------------------------------------------------

template <class W>
concept archive_writer = requires(W& w, std::uint64_t n, entity e) {
    w(n);
    w(e);
};

template <class W, class T>
concept archive_writer_for =
    archive_writer<W> && (detail::is_tag_v<T> || requires(W& w, const T& v) { w(v); });

template <class R>
concept archive_reader = requires(R& r, std::uint64_t& n, entity& e) {
    r(n);
    r(e);
};

template <class R, class T>
concept archive_reader_for =
    archive_reader<R> && (detail::is_tag_v<T> || requires(R& r, T& v) { r(v); });

namespace detail
{
struct archive_access;
}

// The old-handle → fresh-handle mapping a graft produces. Sorted by old slot
// (pack emits slot order); resolve is a binary search.
class graft_map
{
public:
    explicit graft_map(std::pmr::memory_resource* memory = std::pmr::get_default_resource())
        : pairs_(memory)
    {
    }

    // The fresh handle minted for `old`, or no_entity when the archive never
    // contained it.
    [[nodiscard]] entity resolve(entity old) const noexcept
    {
        const auto it = std::ranges::partition_point(pairs_,
                                                     [&](const std::pair<entity, entity>& p)
                                                     { return p.first.index() < old.index(); });
        if (it != pairs_.end() && it->first == old)
        {
            return it->second;
        }
        return no_entity;
    }

    [[nodiscard]] std::size_t size() const noexcept { return pairs_.size(); }
    [[nodiscard]] bool empty() const noexcept { return pairs_.empty(); }

    template <class F>
    void each(F&& fn) const  // fn(entity old, entity fresh)
    {
        for (const auto& [old, fresh] : pairs_)
        {
            fn(old, fresh);
        }
    }

private:
    friend struct detail::archive_access;
    std::pmr::vector<std::pair<entity, entity>> pairs_;
};

namespace detail
{
struct archive_access  // pack/unpack/graft internals; not user API
{
    static std::pmr::vector<std::pair<entity, entity>>& pairs(graft_map& map) noexcept
    {
        return map.pairs_;
    }
};

template <class T>
concept has_member_relink = requires(T& v, const graft_map& m) { v.quiver_relink(m); };
}  // namespace detail

// Opt-in entity-handle rewriting for graft. A component participates by
// defining `void quiver_relink(const graft_map&)`, or — for types you do not
// own — by specializing relink_traits<T> with `links = true` and a static
// relink(T&, const graft_map&).
template <class T>
struct relink_traits
{
    static constexpr bool links = detail::has_member_relink<T>;

    static void relink(T& value, const graft_map& map)
        requires detail::has_member_relink<T>
    {
        value.quiver_relink(map);
    }
};

namespace detail
{
template <component T, class W>
void pack_one(const world& w, W& out)
{
    out(hash_of<T>());
    const auto sel = w.select<T>();  // const world: all-const, never registers
    out(static_cast<std::uint64_t>(sel.count()));
    if constexpr (is_tag_v<T>)
    {
        sel.entities([&](entity e) { out(e); });
    }
    else
    {
        sel.each(
            [&](entity e, const T& value)
            {
                out(e);
                out(value);
            });
    }
}

template <component T, class R>
[[nodiscard]] std::expected<void, fault> unpack_one(world& w, R& in)
{
    std::uint64_t hash{};
    in(hash);
    if (hash != hash_of<T>())
    {
        return std::unexpected(
            fault{fault_code::archive_mismatch, name_of<T>(), "type hash differs"});
    }
    std::uint64_t count{};
    in(count);
    for (std::uint64_t i = 0; i < count; ++i)
    {
        entity e;
        in(e);
        if (!w.alive(e))
        {
            return std::unexpected(
                fault{fault_code::archive_mismatch, name_of<T>(), "row owner not restored"});
        }
        if constexpr (is_tag_v<T>)
        {
            w.add<T>(e);
        }
        else
        {
            static_assert(std::default_initializable<T> && std::is_move_constructible_v<T>,
                          "quiver: unpack/graft default-construct a row, read into it, and "
                          "move it into the world; give T those operations or load manually");
            T value{};
            in(value);
            w.add<T>(e, std::move(value));
        }
    }
    return {};
}

template <component T, class R>
[[nodiscard]] std::expected<void, fault> graft_one(world& w, R& in, const graft_map& map)
{
    std::uint64_t hash{};
    in(hash);
    if (hash != hash_of<T>())
    {
        return std::unexpected(
            fault{fault_code::archive_mismatch, name_of<T>(), "type hash differs"});
    }
    std::uint64_t count{};
    in(count);
    for (std::uint64_t i = 0; i < count; ++i)
    {
        entity old;
        in(old);
        const entity owner = map.resolve(old);
        if (owner == no_entity)
        {
            return std::unexpected(
                fault{fault_code::archive_mismatch, name_of<T>(), "row owner outside archive"});
        }
        if constexpr (is_tag_v<T>)
        {
            w.add<T>(owner);
        }
        else
        {
            static_assert(std::default_initializable<T> && std::is_move_constructible_v<T>,
                          "quiver: unpack/graft default-construct a row, read into it, and "
                          "move it into the world; give T those operations or load manually");
            T value{};
            in(value);
            if constexpr (relink_traits<T>::links)
            {
                relink_traits<T>::relink(value, map);
            }
            w.add<T>(owner, std::move(value));
        }
    }
    return {};
}
}  // namespace detail

// Serializes the live entity set plus the listed component types.
template <component... Ts, class W>
void pack(const world& w, W& out)
{
    static_assert(sizeof...(Ts) > 0, "quiver: pack needs at least one component type");
    static_assert((archive_writer_for<W, Ts> && ...),
                  "quiver: the writer must be callable with std::uint64_t, quiver::entity, and "
                  "each non-tag component as (const T&)");
    static_assert((std::same_as<Ts, detail::bare<Ts>> && ...),
                  "quiver: pack/unpack/graft take plain component types");
    static_assert(detail::all_distinct<Ts...>, "quiver: duplicate component type in pack");
    out(static_cast<std::uint64_t>(w.live_count()));
    w.live_entities([&](entity e) { out(e); });
    (detail::pack_one<Ts>(w, out), ...);
}

// Manifest form: pack(world, writer, Saved{}).
template <class W, component... Ts>
void pack(const world& w, W& out, types<Ts...>)
{
    pack<Ts...>(w, out);
}

// Restores a pack stream into an EMPTY world: exact index+generation pairs
// (rollback semantics — handles saved elsewhere stay meaningful).
template <component... Ts, class R>
[[nodiscard]] std::expected<void, fault> unpack(world& w, R& in)
{
    static_assert(sizeof...(Ts) > 0, "quiver: unpack needs at least one component type");
    static_assert((archive_reader_for<R, Ts> && ...),
                  "quiver: the reader must be callable with std::uint64_t&, quiver::entity&, "
                  "and each non-tag component as (T&)");
    static_assert((std::same_as<Ts, detail::bare<Ts>> && ...),
                  "quiver: pack/unpack/graft take plain component types");
    if (w.live_count() != 0)
    {
        if constexpr (checks_enabled)
        {
            detail::violate("unpack into a non-empty world (reset() it, or use graft)");
        }
        return std::unexpected(fault{fault_code::world_not_empty, {}, "unpack target"});
    }
    std::uint64_t count{};
    in(count);
    for (std::uint64_t i = 0; i < count; ++i)
    {
        entity e;
        in(e);
        if (auto restored = w.restore_entity(e); !restored)
        {
            return std::unexpected(restored.error());
        }
    }
    std::expected<void, fault> result{};
    // Left-to-right with short-circuit: stop at the first faulting type.
    static_cast<void>(((result = detail::unpack_one<Ts>(w, in)).has_value() && ...));
    return result;
}

// Manifest form: unpack(world, reader, Saved{}).
template <class R, component... Ts>
[[nodiscard]] std::expected<void, fault> unpack(world& w, R& in, types<Ts...>)
{
    return unpack<Ts...>(w, in);
}

// Merges a pack stream into a (possibly populated) world: every archived
// entity becomes a FRESH spawn; returns the old→new map. Component values
// relink their stored handles per relink_traits (explicit opt-in).
template <component... Ts, class R>
[[nodiscard]] std::expected<graft_map, fault> graft(
    world& w, R& in, std::pmr::memory_resource* memory = std::pmr::get_default_resource())
{
    static_assert(sizeof...(Ts) > 0, "quiver: graft needs at least one component type");
    static_assert((archive_reader_for<R, Ts> && ...),
                  "quiver: the reader must be callable with std::uint64_t&, quiver::entity&, "
                  "and each non-tag component as (T&)");
    static_assert((std::same_as<Ts, detail::bare<Ts>> && ...),
                  "quiver: pack/unpack/graft take plain component types");
    graft_map map(memory);
    auto& pairs = detail::archive_access::pairs(map);
    std::uint64_t count{};
    in(count);
    pairs.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i)
    {
        entity old;
        in(old);
        pairs.emplace_back(old, w.spawn());
    }
    std::expected<void, fault> result{};
    static_cast<void>(((result = detail::graft_one<Ts>(w, in, map)).has_value() && ...));
    if (!result)
    {
        return std::unexpected(result.error());
    }
    return map;
}

// Manifest form: graft(world, reader, Saved{}).
template <class R, component... Ts>
[[nodiscard]] std::expected<graft_map, fault> graft(
    world& w,
    R& in,
    types<Ts...>,
    std::pmr::memory_resource* memory = std::pmr::get_default_resource())
{
    return graft<Ts...>(w, in, memory);
}

// ----------------------------------------------------------------------------
// Pipeline
//
// The dumbest viable system runner: an ordered list of stages your game loop
// calls once per frame. quiver never owns the loop — a pipeline is a plain
// object you run. Its one real feature: it owns a command buffer
// (`deferred()`) and applies it AFTER EVERY STAGE, so each stage observes
// the previous stage's structural changes and nothing ever mutates under an
// iteration.
//
//   quiver::pipeline frame;
//   frame.stage("movement", [](quiver::world& w, float dt) { ... });
//   frame.stage("combat", [&frame](quiver::world& w, float) {
//       w.each<Health>([&](quiver::entity e, Health& h)
//                      { if (h.value <= 0) { frame.deferred().kill(e); } });
//   });
//   ...every frame: frame.run(world, dt);
//
// Stages are std::move_only_function (capture freely, including move-only
// state). Move-only; stages must outlive nothing — the pipeline owns them.
// ----------------------------------------------------------------------------

class pipeline
{
public:
    using stage_fn = std::move_only_function<void(world&, float)>;

    explicit pipeline(std::pmr::memory_resource* memory = std::pmr::get_default_resource())
        : deferred_(memory)
    {
    }

    pipeline(pipeline&&) noexcept = default;
    pipeline& operator=(pipeline&&) = default;  // not noexcept: deferred_ may rebind
    pipeline(const pipeline&) = delete;
    pipeline& operator=(const pipeline&) = delete;
    ~pipeline() = default;

    // Refused while run() executes (the running stage lives in this vector);
    // register stages between frames.
    pipeline& stage(std::string_view label, stage_fn fn)
    {
        if (running_)
        {
            if constexpr (checks_enabled)
            {
                detail::violate("pipeline::stage during run()");
            }
            return *this;
        }
        stages_.push_back(stage_entry{std::string(label), std::move(fn)});
        return *this;
    }

    pipeline& stage(stage_fn fn) { return stage({}, std::move(fn)); }

    // Runs every stage in declaration order, applying deferred() after each.
    void run(world& w, float dt)
    {
        running_ = true;
        for (stage_entry& entry : stages_)
        {
            entry.fn(w, dt);
            if (!deferred_.empty())
            {
                w.apply(deferred_);
            }
        }
        running_ = false;
    }

    // The pipeline-owned buffer stages record structural changes into.
    [[nodiscard]] command_buffer& deferred() noexcept { return deferred_; }

    [[nodiscard]] std::size_t stages() const noexcept { return stages_.size(); }
    [[nodiscard]] std::string_view label(std::size_t i) const noexcept { return stages_[i].label; }

private:
    struct stage_entry
    {
        std::string label;
        stage_fn fn;
    };

    std::vector<stage_entry> stages_;  // bounded structure: global allocator
    command_buffer deferred_;
    bool running_ = false;  // stage() during run() would relocate the running stage
};

// ----------------------------------------------------------------------------
// Entity references
//
// Pure forwarding sugar over the world verbs — every call inherits the world
// API's checks unchanged, nothing is cached. 16 bytes; pass it down call
// chains instead of (world&, entity) pairs. Never store it in components:
// it pins the world's address, which quiver::entity deliberately does not.
//
// A default-constructed ref is EMPTY (no world): queries (alive, has, find,
// remove, kill, orphan, parent) answer false/null/no-op on it; the
// reference-producing verbs (add, put, replace, obtain, get, owner) require
// a bound ref and are checked-build violations otherwise.
// ----------------------------------------------------------------------------

template <class W>  // W = world (full verb set) or const world (read-only)
class basic_entity_ref
{
    static constexpr bool writable = !std::is_const_v<W>;

public:
    basic_entity_ref() = default;

    basic_entity_ref(W& w, entity e) noexcept
        : world_(&w),
          entity_(e)
    {
    }

    // entity_ref converts to const_entity_ref, never the other way.
    basic_entity_ref(const basic_entity_ref<std::remove_const_t<W>>& other) noexcept
        requires std::is_const_v<W>
        : world_(other.world_),
          entity_(other.entity_)
    {
    }

    [[nodiscard]] entity id() const noexcept { return entity_; }
    operator entity() const noexcept { return entity_; }  // NOLINT(google-explicit-constructor)
    [[nodiscard]] W& owner() const noexcept { return *world_; }

    [[nodiscard]] bool alive() const noexcept
    {
        return world_ != nullptr && world_->alive(entity_);
    }
    explicit operator bool() const noexcept { return alive(); }

    template <component T, class... Args>
    decltype(auto) add(Args&&... args) const
        requires writable
    {
        return bound("entity_ref::add").template add<T>(entity_, std::forward<Args>(args)...);
    }

    template <component T, class... Args>
    decltype(auto) put(Args&&... args) const
        requires writable
    {
        return bound("entity_ref::put").template put<T>(entity_, std::forward<Args>(args)...);
    }

    template <component T, class... Args>
    T& replace(Args&&... args) const
        requires writable
    {
        return bound("entity_ref::replace")
            .template replace<T>(entity_, std::forward<Args>(args)...);
    }

    template <component T, class... Args>
    decltype(auto) obtain(Args&&... args) const
        requires writable
    {
        return bound("entity_ref::obtain").template obtain<T>(entity_, std::forward<Args>(args)...);
    }

    template <component T>
    bool remove() const
        requires writable
    {
        return world_ != nullptr && world_->template remove<T>(entity_);
    }

    template <component T>
    [[nodiscard]] bool has() const noexcept
    {
        return world_ != nullptr && world_->template has<T>(entity_);
    }

    // T& from an entity_ref, const T& from a const_entity_ref.
    template <component T>
    [[nodiscard]] decltype(auto) get() const
    {
        return bound("entity_ref::get").template get<T>(entity_);
    }

    template <component T>
    [[nodiscard]] auto* find() const noexcept
    {
        using pointer = decltype(world_->template find<T>(entity_));
        return world_ != nullptr ? world_->template find<T>(entity_) : pointer{nullptr};
    }

    void kill() const noexcept
        requires writable
    {
        if (world_ != nullptr)
        {
            world_->kill(entity_);
        }
    }

    void orphan() const
        requires writable
    {
        if (world_ != nullptr)
        {
            world_->orphan(entity_);
        }
    }

    [[nodiscard]] basic_entity_ref parent() const noexcept
    {
        return world_ != nullptr ? basic_entity_ref{*world_, world_->parent_of(entity_)}
                                 : basic_entity_ref{};
    }

private:
    template <class>
    friend class basic_entity_ref;  // the converting constructor reads members

    // The reference-producing verbs cannot no-op an empty ref.
    [[nodiscard]] W& bound(const char* what) const
    {
        if (world_ == nullptr)
        {
            if constexpr (checks_enabled)
            {
                detail::violate_pool("call on an empty (default-constructed) entity_ref:", what);
            }
            std::abort();
        }
        return *world_;
    }

    W* world_ = nullptr;
    entity entity_;
};

inline entity_ref world::ref(entity e) noexcept
{
    return entity_ref{*this, e};
}

inline const_entity_ref world::ref(entity e) const noexcept
{
    return const_entity_ref{*this, e};
}

inline entity_ref world::globals()
{
    if (!table_.alive(globals_))
    {
        globals_ = select<globals_mark>().first();  // an unpack may have restored it
        if (globals_ == no_entity)
        {
            globals_ = table_.create();
        }
    }
    if (!has<globals_mark>(globals_))
    {
        // First call — or self-healing after a lazy mark add was refused
        // (first globals() under an iteration of the mark pool).
        add<globals_mark>(globals_);
    }
    return entity_ref{*this, globals_};
}

inline const_entity_ref world::globals() const noexcept
{
    if (table_.alive(globals_))
    {
        return const_entity_ref{*this, globals_};
    }
    // No spawning on const worlds; a dead-safe ref when none exists yet.
    return const_entity_ref{*this, select<globals_mark>().first()};
}

// --- command_buffer's deferred ops (need the complete world type) ---

template <component T, command_buffer::op_kind Kind>
void command_buffer::erased_apply(world& w, entity target, void* payload)
{
    T& value = *static_cast<T*>(payload);
    if constexpr (Kind == op_kind::add)
    {
        w.add<T>(target, std::move(value));
    }
    else
    {
        w.put<T>(target, std::move(value));
    }
}

template <component T, command_buffer::op_kind Kind>
void command_buffer::erased_apply_tag(world& w, entity target)
{
    if constexpr (Kind == op_kind::add)
    {
        w.add<T>(target);
    }
    else
    {
        w.put<T>(target);
    }
}

template <component T>
void blueprint::erased_stamp(world& w, entity e)
{
    w.add<T>(e);
}

template <component T>
void blueprint::erased_stamp_value(world& w, entity e, const void* payload)
{
    w.add<T>(e, *static_cast<const T*>(payload));  // copy: the recipe is reusable
}

template <component T>
void command_buffer::erased_remove(world& w, entity target)
{
    w.remove<T>(target);
}

// ----------------------------------------------------------------------------
// any — a type-erased value with small-buffer optimization
//
// RTTI-free: identity is hash_of<T>, the same 64-bit name hash pools and
// archives key on. The vtable is a static per-type table of plain function
// pointers — no virtual classes, debugger-plain. Payloads up to three words
// with ordinary alignment live inline; everything else (and every
// over-aligned type) takes one aligned heap allocation. ref() makes a
// NON-OWNING view: writes alias the original, destruction leaves it alone,
// and copies of a ref are more refs. Copying an any whose payload is not
// copy-constructible is a checked violation that yields an empty any.
// as<T>() is the get-style accessor (aborts on mismatch in checked builds);
// try_as<T>() is the find-style one (null on mismatch).
// ----------------------------------------------------------------------------

class any
{
    static constexpr std::size_t sbo_bytes = 3 * sizeof(void*);

    union storage
    {
        void* remote;
        alignas(std::max_align_t) std::byte local[sbo_bytes];
    };

    template <class T>
    static constexpr bool fits_inline =
        sizeof(T) <= sbo_bytes && alignof(T) <= alignof(std::max_align_t) &&
        std::is_nothrow_move_constructible_v<T>;

    enum class place : std::uint8_t
    {
        local,
        remote,
        ref,
    };

    struct vtable_t
    {
        std::uint64_t hash;
        place where;
        void (*destroy)(any&) noexcept;
        void (*copy)(any& dst, const any& src);     // null: payload not copyable
        void (*move)(any& dst, any& src) noexcept;  // src left empty
        void* (*address)(const any&) noexcept;
    };

    template <class T, place Where>
    static const vtable_t* table() noexcept
    {
        static constexpr vtable_t vt{
            hash_of<T>(),
            Where,
            // destroy
            +[](any& self) noexcept
            {
                if constexpr (Where == place::local)
                {
                    std::destroy_at(static_cast<T*>(static_cast<void*>(self.store_.local)));
                }
                else if constexpr (Where == place::remote)
                {
                    T* payload = static_cast<T*>(self.store_.remote);
                    std::destroy_at(payload);
                    ::operator delete(payload, std::align_val_t{alignof(T)});
                }
            },
            // copy (refs copy as refs; owned payloads deep-copy when they can)
            []() -> void (*)(any&, const any&)
            {
                if constexpr (Where == place::ref)
                {
                    return +[](any& dst, const any& src)
                    {
                        dst.vt_ = src.vt_;
                        dst.store_.remote = src.store_.remote;
                    };
                }
                else if constexpr (std::copy_constructible<T>)
                {
                    return +[](any& dst, const any& src)
                    { dst.emplace_value<T>(*static_cast<const T*>(src.vt_->address(src))); };
                }
                else
                {
                    return nullptr;
                }
            }(),
            // move
            +[](any& dst, any& src) noexcept
            {
                if constexpr (Where == place::local)
                {
                    std::construct_at(
                        static_cast<T*>(static_cast<void*>(dst.store_.local)),
                        std::move(*static_cast<T*>(static_cast<void*>(src.store_.local))));
                    dst.vt_ = src.vt_;
                    src.vt_->destroy(src);
                }
                else  // remote and ref: steal the pointer
                {
                    dst.store_.remote = src.store_.remote;
                    dst.vt_ = src.vt_;
                }
                src.vt_ = nullptr;
            },
            // address
            +[](const any& self) noexcept -> void*
            {
                if constexpr (Where == place::local)
                {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                    return const_cast<std::byte*>(self.store_.local);
                }
                else
                {
                    return self.store_.remote;
                }
            },
        };
        return &vt;
    }

    template <class T, class... Args>
    void emplace_value(Args&&... args)
    {
        if constexpr (fits_inline<T>)
        {
            std::construct_at(static_cast<T*>(static_cast<void*>(store_.local)),
                              std::forward<Args>(args)...);
            vt_ = table<T, place::local>();
        }
        else
        {
            void* raw = ::operator new(sizeof(T), std::align_val_t{alignof(T)});
            try
            {
                store_.remote =
                    std::construct_at(static_cast<T*>(raw), std::forward<Args>(args)...);
            }
            catch (...)
            {
                ::operator delete(raw, std::align_val_t{alignof(T)});
                throw;
            }
            vt_ = table<T, place::remote>();
        }
    }

public:
    any() noexcept = default;  // empty

    // The owning maker (any's analogue of add: explicit, never implicit).
    template <class T, class... Args>
    [[nodiscard]] static any make(Args&&... args)
    {
        static_assert(std::same_as<T, detail::bare<T>>,
                      "quiver: any::make<T> takes a plain, unqualified value type");
        any out;
        out.emplace_value<T>(std::forward<Args>(args)...);
        return out;
    }

    // The non-owning view maker.
    template <class T>
    [[nodiscard]] static any ref(T& object) noexcept
    {
        static_assert(std::same_as<T, detail::bare<T>>,
                      "quiver: any::ref<T> views a plain, unqualified value type");
        any out;
        out.store_.remote = &object;
        out.vt_ = table<T, place::ref>();
        return out;
    }

    any(const any& other)
    {
        if (other.vt_ == nullptr)
        {
            return;
        }
        if (other.vt_->copy == nullptr)
        {
            if constexpr (checks_enabled)
            {
                detail::violate(
                    "copy of an any holding a non-copyable payload (the copy is "
                    "empty; move it or pass any::ref)");
            }
            return;
        }
        other.vt_->copy(*this, other);
    }

    any(any&& other) noexcept
    {
        if (other.vt_ != nullptr)
        {
            other.vt_->move(*this, other);
        }
    }

    any& operator=(const any& other)
    {
        if (this != &other)
        {
            any copy(other);
            reset();
            if (copy.vt_ != nullptr)
            {
                copy.vt_->move(*this, copy);
            }
        }
        return *this;
    }

    any& operator=(any&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            if (other.vt_ != nullptr)
            {
                other.vt_->move(*this, other);
            }
        }
        return *this;
    }

    ~any() { reset(); }

    void reset() noexcept
    {
        if (vt_ != nullptr)
        {
            vt_->destroy(*this);
            vt_ = nullptr;
        }
    }

    [[nodiscard]] bool holds() const noexcept { return vt_ != nullptr; }
    explicit operator bool() const noexcept { return holds(); }

    // hash_of<T> of the payload; 0 when empty.
    [[nodiscard]] std::uint64_t type_hash() const noexcept
    {
        return vt_ != nullptr ? vt_->hash : 0;
    }

    // find-style access: null on empty or type mismatch.
    template <class T>
    [[nodiscard]] T* try_as() noexcept
    {
        return vt_ != nullptr && vt_->hash == hash_of<detail::bare<T>>()
                   ? static_cast<T*>(vt_->address(*this))
                   : nullptr;
    }

    template <class T>
    [[nodiscard]] const T* try_as() const noexcept
    {
        return vt_ != nullptr && vt_->hash == hash_of<detail::bare<T>>()
                   ? static_cast<const T*>(vt_->address(*this))
                   : nullptr;
    }

    // get-style access: aborts on mismatch (a reference must be produced).
    template <class T>
    [[nodiscard]] T& as()
    {
        T* payload = try_as<T>();
        if (payload == nullptr)
        {
            if constexpr (checks_enabled)
            {
                detail::violate(
                    "as<T> on an any holding a different type (or nothing); "
                    "try_as<T> is the safe form");
            }
            std::abort();  // cannot produce a reference
        }
        return *payload;
    }

    template <class T>
    [[nodiscard]] const T& as() const
    {
        const T* payload = try_as<T>();
        if (payload == nullptr)
        {
            if constexpr (checks_enabled)
            {
                detail::violate(
                    "as<T> on an any holding a different type (or nothing); "
                    "try_as<T> is the safe form");
            }
            std::abort();
        }
        return *payload;
    }

    // Raw payload bytes for tooling (null when empty); the pointer obeys the
    // payload's own lifetime rules.
    [[nodiscard]] void* data() noexcept { return vt_ != nullptr ? vt_->address(*this) : nullptr; }
    [[nodiscard]] const void* data() const noexcept
    {
        return vt_ != nullptr ? vt_->address(*this) : nullptr;
    }

private:
    const vtable_t* vt_ = nullptr;
    storage store_{};
};

// ----------------------------------------------------------------------------
// Reflection — a process-wide, RTTI-free runtime type registry
//
// Registration is a fluent builder driven entirely by compile-time member
// pointers; lookups are lock-free binary searches keyed by hash_of<T> (the
// SAME identity archives and pools use, so reflection, serialization, and
// runtime pools agree on what a type is called). Register at startup —
// registration is externally synchronized, like world construction. Every
// runtime operation is fault-tolerant, editor-grade: mismatches answer with
// empty handles, empty anys, or false — never an abort.
//
//   quiver::reflect<Transform>()
//       .field<&Transform::x>("x")
//       .field<&Transform::y>("y")
//       .method<&Transform::translate>("translate")
//       .construct<float, float>();
//
//   if (auto r = quiver::reflection_of("Transform")) { ... }
// ----------------------------------------------------------------------------

namespace detail
{
struct field_node
{
    std::string_view name;
    std::uint64_t owner_hash;
    std::uint64_t type_hash;
    any (*get)(const void* object);
    bool (*set)(void* object, const any& value);
};

struct method_node
{
    std::string_view name;
    std::uint64_t owner_hash;
    bool is_const;
    any (*invoke)(void* object, std::span<any> args);
};

struct ctor_node
{
    std::size_t arity;
    any (*construct)(std::span<any> args);
};

// The meta×ECS bridge: structural verbs by hash, captured when reflect<T>()
// sees a component type. Alive-gated and presence-gated so editor calls
// refuse with false instead of tripping checked violations.
struct ecs_bridge
{
    bool (*add_to)(world& w, entity e, any& value) = nullptr;
    bool (*remove_from)(world& w, entity e) = nullptr;
    bool (*present_on)(const world& w, entity e) = nullptr;
};

struct type_node
{
    std::uint64_t hash = 0;
    std::string_view name;
    std::size_t size_bytes = 0;
    std::size_t align = 0;
    std::vector<field_node> fields;
    std::vector<method_node> methods;
    std::vector<ctor_node> ctors;
    ecs_bridge ecs;
};

// Sorted by hash; nodes are heap-stable so handles survive registrations.
inline std::vector<std::unique_ptr<type_node>>& reflection_registry()
{
    static std::vector<std::unique_ptr<type_node>> nodes;
    return nodes;
}

[[nodiscard]] inline type_node* find_type_node(std::uint64_t hash) noexcept
{
    auto& nodes = reflection_registry();
    const auto it = std::lower_bound(nodes.begin(),
                                     nodes.end(),
                                     hash,
                                     [](const std::unique_ptr<type_node>& node, std::uint64_t h)
                                     { return node->hash < h; });
    return it != nodes.end() && (*it)->hash == hash ? it->get() : nullptr;
}

// Member-pointer introspection for the builder.
template <class M>
struct member_object_traits;

template <class C, class M>
struct member_object_traits<M C::*>
{
    using owner = C;
    using value = M;
};

template <class F>
struct member_function_traits;

template <class C, class R, class... As>
struct member_function_traits<R (C::*)(As...)>
{
    using owner = C;
    using result = R;
    using args = types<As...>;
    static constexpr bool is_const = false;
};

template <class C, class R, class... As>
struct member_function_traits<R (C::*)(As...) const>
{
    using owner = C;
    using result = R;
    using args = types<As...>;
    static constexpr bool is_const = true;
};
}  // namespace detail

// A valid-or-empty handle over one registered data member.
class field
{
public:
    field() = default;

    explicit operator bool() const noexcept { return node_ != nullptr; }
    [[nodiscard]] std::string_view name() const noexcept { return node_->name; }
    [[nodiscard]] std::uint64_t type_hash() const noexcept { return node_->type_hash; }

    // Copies the member out of an any holding the owner type; empty any on
    // mismatch.
    [[nodiscard]] any get(const any& object) const
    {
        if (node_ == nullptr || object.type_hash() != node_->owner_hash)
        {
            return {};
        }
        return node_->get(object.data());
    }

    // Writes the member into an any holding the owner type; false on any
    // mismatch, leaving the value untouched.
    bool set(any& object, const any& value) const
    {
        if (node_ == nullptr || object.type_hash() != node_->owner_hash)
        {
            return false;
        }
        return node_->set(object.data(), value);
    }

    // Raw-pointer forms for LIVE components (pool_ref::raw bytes): the
    // caller vouches that object points at the owner type, and the pointer
    // obeys the pool's invalidation rules like any component reference.
    [[nodiscard]] any get_at(const void* object) const
    {
        return node_ != nullptr ? node_->get(object) : any{};
    }

    bool set_at(void* object, const any& value) const
    {
        return node_ != nullptr && node_->set(object, value);
    }

private:
    friend class reflection;

    explicit field(const detail::field_node* node) noexcept
        : node_(node)
    {
    }

    const detail::field_node* node_ = nullptr;
};

// A valid-or-empty handle over one registered member function.
class method
{
public:
    method() = default;

    explicit operator bool() const noexcept { return node_ != nullptr; }
    [[nodiscard]] std::string_view name() const noexcept { return node_->name; }

    // Invokes with arguments unpacked from anys; the result rides back in
    // one (empty for void). Wrong object type, arity, or argument types:
    // empty any, no call.
    any invoke(any& object, std::span<any> args) const
    {
        if (node_ == nullptr || object.type_hash() != node_->owner_hash)
        {
            return {};
        }
        return node_->invoke(object.data(), args);
    }

    // Const objects accept const member functions only.
    any invoke(const any& object, std::span<any> args) const
    {
        if (node_ == nullptr || !node_->is_const || object.type_hash() != node_->owner_hash)
        {
            return {};
        }
        // Const member call through the object's address: safe by is_const.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        return node_->invoke(const_cast<void*>(object.data()), args);
    }

private:
    friend class reflection;

    explicit method(const detail::method_node* node) noexcept
        : node_(node)
    {
    }

    const detail::method_node* node_ = nullptr;
};

// A valid-or-empty handle over one registered type.
class reflection
{
public:
    reflection() = default;

    explicit operator bool() const noexcept { return node_ != nullptr; }
    [[nodiscard]] std::string_view name() const noexcept { return node_->name; }
    [[nodiscard]] std::uint64_t hash() const noexcept { return node_->hash; }
    [[nodiscard]] std::size_t size_bytes() const noexcept { return node_->size_bytes; }
    [[nodiscard]] std::size_t align() const noexcept { return node_->align; }

    [[nodiscard]] field find_field(std::string_view name) const noexcept
    {
        if (node_ != nullptr)
        {
            for (const detail::field_node& f : node_->fields)
            {
                if (f.name == name)
                {
                    return field(&f);
                }
            }
        }
        return {};
    }

    [[nodiscard]] method find_method(std::string_view name) const noexcept
    {
        if (node_ != nullptr)
        {
            for (const detail::method_node& m : node_->methods)
            {
                if (m.name == name)
                {
                    return method(&m);
                }
            }
        }
        return {};
    }

    // Walks fields in registration order: fn(const quiver::field&).
    template <class F>
    void each_field(F&& fn) const
    {
        if (node_ != nullptr)
        {
            for (const detail::field_node& f : node_->fields)
            {
                fn(field(&f));
            }
        }
    }

    // Constructs an instance through the first registered constructor whose
    // arity and argument types match; empty any when none do.
    [[nodiscard]] any construct(std::span<any> args) const
    {
        if (node_ != nullptr)
        {
            for (const detail::ctor_node& c : node_->ctors)
            {
                if (c.arity != args.size())
                {
                    continue;
                }
                any made = c.construct(args);
                if (made.holds())
                {
                    return made;
                }
            }
        }
        return {};
    }

    // --- the meta×ECS bridge (captured for component types) ---
    // add_to moves the payload in (an EMPTY any default-constructs, and is
    // how tags add); every verb is alive- and presence-gated so editor calls
    // answer false instead of tripping checked violations. Mid-iteration
    // calls hit the same rules as the typed verbs — editors mutate between
    // frames.
    bool add_to(world& w, entity e, any value) const
    {
        return node_ != nullptr && node_->ecs.add_to != nullptr && node_->ecs.add_to(w, e, value);
    }

    bool remove_from(world& w, entity e) const
    {
        return node_ != nullptr && node_->ecs.remove_from != nullptr &&
               node_->ecs.remove_from(w, e);
    }

    [[nodiscard]] bool present_on(const world& w, entity e) const noexcept
    {
        return node_ != nullptr && node_->ecs.present_on != nullptr && node_->ecs.present_on(w, e);
    }

private:
    template <class T>
    friend class reflect_builder;
    friend reflection reflection_of(std::uint64_t hash) noexcept;

    explicit reflection(const detail::type_node* node) noexcept
        : node_(node)
    {
    }

    const detail::type_node* node_ = nullptr;
};

[[nodiscard]] inline reflection reflection_of(std::uint64_t hash) noexcept
{
    return reflection(detail::find_type_node(hash));
}

[[nodiscard]] inline reflection reflection_of(std::string_view name) noexcept
{
    // Names hash with the same fnv1a that hash_of<T> uses, so the name
    // lookup IS the hash lookup.
    return reflection_of(detail::fnv1a(name));
}

template <class T>
[[nodiscard]] reflection reflection_of() noexcept
{
    return reflection_of(hash_of<detail::bare<T>>());
}

template <class T>
class reflect_builder;

template <class T>
reflect_builder<T> reflect();

// The fluent registrar. Duplicate registration of the same type is a checked
// violation; the builder then appends to the existing node.
template <class T>
class reflect_builder
{
public:
    template <auto Member>
    reflect_builder& field(std::string_view name)
    {
        using traits = detail::member_object_traits<decltype(Member)>;
        static_assert(std::same_as<typename traits::owner, T>,
                      "quiver: field<&U::m> must name a member of the reflected type");
        using value = typename traits::value;
        node_->fields.push_back(detail::field_node{
            name,
            hash_of<T>(),
            hash_of<detail::bare<value>>(),
            +[](const void* object) -> any
            { return any::make<detail::bare<value>>(static_cast<const T*>(object)->*Member); },
            +[](void* object, const any& incoming) -> bool
            {
                const auto* payload = incoming.template try_as<detail::bare<value>>();
                if (payload == nullptr)
                {
                    return false;
                }
                static_cast<T*>(object)->*Member = *payload;
                return true;
            },
        });
        return *this;
    }

    template <auto Method>
    reflect_builder& method(std::string_view name)
    {
        using traits = detail::member_function_traits<decltype(Method)>;
        static_assert(std::same_as<typename traits::owner, T>,
                      "quiver: method<&U::fn> must name a member of the reflected type");
        node_->methods.push_back(detail::method_node{
            name,
            hash_of<T>(),
            traits::is_const,
            &invoke_thunk<Method>,
        });
        return *this;
    }

    template <class... Args>
    reflect_builder& construct()
    {
        static_assert(std::constructible_from<T, Args...>,
                      "quiver: construct<Args...> must name a real constructor of T");
        node_->ctors.push_back(detail::ctor_node{
            sizeof...(Args),
            +[](std::span<any> args) -> any
            {
                return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> any
                {
                    const bool typed =
                        ((args[Is].template try_as<detail::bare<Args>>() != nullptr) && ...);
                    if (!typed)
                    {
                        return {};
                    }
                    return any::make<T>(*args[Is].template try_as<detail::bare<Args>>()...);
                }(std::index_sequence_for<Args...>{});
            },
        });
        return *this;
    }

private:
    friend reflect_builder<T> reflect<T>();

    template <auto Method>
    static any invoke_thunk(void* object, std::span<any> args)
    {
        using traits = detail::member_function_traits<decltype(Method)>;
        return [&]<class... As>(types<As...>) -> any
        {
            if (args.size() != sizeof...(As))
            {
                return {};
            }
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> any
            {
                const bool typed =
                    ((args[Is].template try_as<detail::bare<As>>() != nullptr) && ...);
                if (!typed)
                {
                    return {};
                }
                using result = typename traits::result;
                if constexpr (std::is_void_v<result>)
                {
                    (static_cast<T*>(object)->*Method)(
                        *args[Is].template try_as<detail::bare<As>>()...);
                    return {};
                }
                else
                {
                    return any::make<detail::bare<result>>((static_cast<T*>(object)->*Method)(
                        *args[Is].template try_as<detail::bare<As>>()...));
                }
            }(std::index_sequence_for<As...>{});
        }(typename traits::args{});
    }

    explicit reflect_builder(detail::type_node* node) noexcept
        : node_(node)
    {
    }

    detail::type_node* node_;
};

// Registers T (idempotent identity: name from name_of<T>/quiver_label) and
// returns the builder for fields, methods, and constructors.
template <class T>
reflect_builder<T> reflect()
{
    static_assert(std::same_as<T, detail::bare<T>>,
                  "quiver: reflect<T> takes a plain, unqualified type");
    constexpr std::uint64_t hash = hash_of<T>();
    if (detail::type_node* standing = detail::find_type_node(hash); standing != nullptr)
    {
        if constexpr (checks_enabled)
        {
            detail::violate_pool("reflect<T> on an already registered type", name_of<T>());
        }
        return reflect_builder<T>(standing);
    }
    auto& nodes = detail::reflection_registry();
    const auto at = std::lower_bound(nodes.begin(),
                                     nodes.end(),
                                     hash,
                                     [](const std::unique_ptr<detail::type_node>& node,
                                        std::uint64_t h) { return node->hash < h; });
    auto node = std::make_unique<detail::type_node>();
    node->hash = hash;
    node->name = name_of<T>();
    node->size_bytes = sizeof(T);
    node->align = alignof(T);
    if constexpr (component<T>)
    {
        node->ecs.add_to = +[](world& w, entity e, any& value) -> bool
        {
            if (!w.alive(e) || w.has<T>(e))
            {
                return false;
            }
            if constexpr (detail::is_tag_v<T>)
            {
                w.add<T>(e);  // membership is the whole payload
                return true;
            }
            else
            {
                if (T* payload = value.try_as<T>(); payload != nullptr)
                {
                    if constexpr (std::move_constructible<T>)
                    {
                        w.add<T>(e, std::move(*payload));
                        return true;
                    }
                    else
                    {
                        return false;  // adds move the payload in
                    }
                }
                if (!value.holds())
                {
                    if constexpr (std::default_initializable<T>)
                    {
                        w.add<T>(e);  // empty any: default-construct
                        return true;
                    }
                    else
                    {
                        return false;
                    }
                }
                return false;  // payload of the wrong type
            }
        };
        node->ecs.remove_from =
            +[](world& w, entity e) -> bool { return w.alive(e) && w.remove<T>(e); };
        node->ecs.present_on = +[](const world& w, entity e) -> bool { return w.has<T>(e); };
    }
    detail::type_node* raw = node.get();
    nodes.insert(at, std::move(node));
    return reflect_builder<T>(raw);
}

// Manifest-driven identity registration: one call per types<> list.
// Idempotent — already registered types are skipped, not violations.
template <class... Ts>
void reflect_all(types<Ts...>)
{
    ((detail::find_type_node(hash_of<detail::bare<Ts>>()) == nullptr
          ? static_cast<void>(reflect<detail::bare<Ts>>())
          : void()),
     ...);
}

// ----------------------------------------------------------------------------
// Checked-build test backdoor: deliberately corrupts internal state so a test
// suite can prove validate() detects real damage, and exposes iteration-lock
// counters. Not part of the API contract.
// ----------------------------------------------------------------------------

#if QUIVER_CHECKS
struct test_access
{
    template <component T>
    static void corrupt_sparse(world& w, entity e)
    {
        w.peek_pool<T>()->sparse_.set(e.index(), detail::npos32 - 1);
    }

    static void corrupt_generation(world& w, entity e) { ++w.table_.generation_[e.index()]; }

    template <component T>
    static std::uint32_t lock_count(const world& w)
    {
        const auto* pool = w.peek_pool<T>();
        return pool == nullptr ? 0 : pool->locks_;
    }

    // Breaks the mirror invariant in ONE pool of a bonded pair (the partner
    // keeps its order), so tests can prove validate() detects it.
    template <component T>
    static void corrupt_bond(world& w)
    {
        auto* pool = w.peek_pool<T>();
        pool->swap_dense(0, 1);  // dense+sparse stay consistent; the mirror breaks
    }
};
#endif

// Named hasher mirroring std::hash<basic_entity<...>>. Exists for module
// builds, where some toolchains do not surface global-module-fragment
// std::hash specializations to importers:
// std::unordered_set<entity, entity_hash> works everywhere.
template <entity_traits Traits>
struct basic_entity_hash
{
    [[nodiscard]] std::size_t operator()(basic_entity<Traits> e) const noexcept
    {
        return std::hash<std::uint64_t>{}(e.bits());
    }
};

using entity_hash = basic_entity_hash<default_entity_traits>;

}  // namespace quiver

template <quiver::entity_traits Traits>
struct std::hash<quiver::basic_entity<Traits>>
{
    [[nodiscard]] std::size_t operator()(quiver::basic_entity<Traits> e) const noexcept
    {
        return std::hash<std::uint64_t>{}(e.bits());
    }
};