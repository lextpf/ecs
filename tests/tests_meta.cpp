// ============================================================================
// tests_meta.cpp — quiver::any and the reflection registry (M6), plus the
// meta×ECS bridge (M8). RTTI-free: identity is hash_of/name_of throughout.
// ============================================================================

#include "test_harness.hpp"

#include <array>

// Reflection subjects.
struct Vec2
{
    float x = 0;
    float y = 0;

    [[nodiscard]] float length2() const { return (x * x) + (y * y); }

    void translate(float dx, float dy)
    {
        x += dx;
        y += dy;
    }
};

struct MetaLabeled
{
    int v = 0;
    static constexpr std::string_view quiver_label = "meta.labeled";
};

struct MetaNoCopy
{
    MetaNoCopy() = default;

    explicit MetaNoCopy(int v)
        : box(std::make_unique<int>(v))
    {
    }

    MetaNoCopy(MetaNoCopy&&) = default;
    MetaNoCopy& operator=(MetaNoCopy&&) = default;

    std::unique_ptr<int> box;
};

struct MetaBlob  // 64 bytes: beyond the SBO buffer, lands on the heap
{
    double lanes[8] = {};
};

void test_any_basics()
{
    section("any: SBO, heap, refs, lifetime balance");

    // Small payloads live inline; the value round-trips.
    ecs::any small = ecs::any::make<int>(42);
    CHECK(small.holds());
    CHECK(small.type_hash() == ecs::hash_of<int>());
    CHECK(*small.try_as<int>() == 42);
    CHECK(small.try_as<float>() == nullptr);  // find-style mismatch
    CHECK(small.as<int>() == 42);

    // Large payloads go to the heap; same surface.
    ecs::any big = ecs::any::make<MetaBlob>();
    big.as<MetaBlob>().lanes[7] = 9.0;
    ecs::any big2 = big;  // deep copy
    big.as<MetaBlob>().lanes[7] = 1.0;
    CHECK(big2.as<MetaBlob>().lanes[7] == 9.0);

    // Over-aligned payloads are honored on the heap path.
    {
        ecs::any aligned = ecs::any::make<Aligned>();
        CHECK(reinterpret_cast<std::uintptr_t>(aligned.data()) % 64 == 0);
    }
    CHECK(misaligned_payloads == 0);

    // Lifetime balance across copy / move / reset.
    {
        const int live_before = Counted::live;
        ecs::any a = ecs::any::make<Counted>(7);
        ecs::any b = a;             // copy: two live
        ecs::any c = std::move(a);  // move: still two live, a empty
        CHECK(!a.holds());
        CHECK(Counted::live == live_before + 2);
        b.reset();
        CHECK(Counted::live == live_before + 1);
        c.reset();
        CHECK(Counted::live == live_before);
    }

    // ref() aliases without owning: writes hit the original, destruction
    // leaves it alone.
    {
        Vec2 v{1, 2};
        ecs::any view = ecs::any::ref(v);
        view.as<Vec2>().x = 10;
        CHECK(v.x == 10);
        ecs::any view2 = view;  // copying a ref makes another ref
        view2.as<Vec2>().y = 20;
        CHECK(v.y == 20);
    }

    // Copying an any that holds a non-copyable payload: violation + empty.
#if QUIVER_CHECKS
    {
        violation_scope guard;
        ecs::any pinned = ecs::any::make<MetaNoCopy>(5);
        ecs::any stolen = pinned;  // NOLINT(performance-unnecessary-copy-initialization)
        CHECK(violations_seen == 1);
        CHECK(!stolen.holds());
        CHECK(pinned.holds());  // the source is untouched
    }
#endif

    // Empty anys answer consistently.
    const ecs::any none;
    CHECK(!none.holds());
    CHECK(none.type_hash() == 0);
    CHECK(none.try_as<int>() == nullptr);
}

void test_reflection_registry()
{
    section("reflection: registration and resolve");

    ecs::reflect<Vec2>()
        .field<&Vec2::x>("x")
        .field<&Vec2::y>("y")
        .method<&Vec2::length2>("length2")
        .method<&Vec2::translate>("translate")
        .construct<float, float>();
    ecs::reflect<MetaLabeled>().field<&MetaLabeled::v>("v");

    // Resolve by type, by hash, and by (label-pinned) name.
    const ecs::reflection by_type = ecs::reflection_of<Vec2>();
    CHECK(static_cast<bool>(by_type));
    CHECK(by_type.hash() == ecs::hash_of<Vec2>());
    CHECK(by_type.size_bytes() == sizeof(Vec2));
    CHECK(by_type.align() == alignof(Vec2));

    const ecs::reflection by_hash = ecs::reflection_of(ecs::hash_of<Vec2>());
    CHECK(by_hash.hash() == by_type.hash());

    const ecs::reflection by_name = ecs::reflection_of("meta.labeled");
    CHECK(static_cast<bool>(by_name));
    CHECK(by_name.hash() == ecs::hash_of<MetaLabeled>());
    CHECK(by_name.name() == "meta.labeled");

    // Unknown types answer with an empty handle, never an error.
    CHECK(!ecs::reflection_of(0x1234u));
    CHECK(!ecs::reflection_of("meta.never_registered"));

#if QUIVER_CHECKS
    {
        // Re-registering is a violation; the original node survives.
        violation_scope guard;
        ecs::reflect<Vec2>();
        CHECK(violations_seen == 1);
        CHECK(static_cast<bool>(ecs::reflection_of<Vec2>()));
    }
#endif
}

void test_reflection_fields()
{
    section("reflection: field get/set through any");

    const ecs::reflection r = ecs::reflection_of<Vec2>();
    ecs::any obj = ecs::any::make<Vec2>(Vec2{3, 4});

    const ecs::field fx = r.find_field("x");
    CHECK(static_cast<bool>(fx));
    CHECK(fx.name() == "x");
    CHECK(fx.type_hash() == ecs::hash_of<float>());

    // get copies the value out; set writes it back.
    ecs::any x = fx.get(obj);
    CHECK(x.as<float>() == 3.0f);
    CHECK(fx.set(obj, ecs::any::make<float>(30.0f)));
    CHECK(obj.as<Vec2>().x == 30.0f);

    // A mismatched set returns false and leaves the value intact.
    CHECK(!fx.set(obj, ecs::any::make<int>(1)));
    CHECK(obj.as<Vec2>().x == 30.0f);

    // get from a mismatched object: empty any, never an abort.
    ecs::any wrong = ecs::any::make<int>(5);
    CHECK(!fx.get(wrong).holds());

    // Unknown fields are empty handles; each_field walks registration order.
    CHECK(!r.find_field("z"));
    std::array<std::string_view, 2> order{};
    std::size_t at = 0;
    r.each_field([&](const ecs::field& f) { order[at++] = f.name(); });
    CHECK(at == 2);
    CHECK(order[0] == "x");
    CHECK(order[1] == "y");
}

void test_reflection_methods()
{
    section("reflection: method invocation");

    const ecs::reflection r = ecs::reflection_of<Vec2>();
    ecs::any obj = ecs::any::make<Vec2>(Vec2{3, 4});

    // Exact call: arguments unpacked from anys, result wrapped in one.
    const ecs::method translate = r.find_method("translate");
    CHECK(static_cast<bool>(translate));
    std::array<ecs::any, 2> args{ecs::any::make<float>(1.0f), ecs::any::make<float>(2.0f)};
    translate.invoke(obj, args);
    CHECK(obj.as<Vec2>().x == 4.0f);
    CHECK(obj.as<Vec2>().y == 6.0f);

    // A const method runs on a const object.
    const ecs::method length2 = r.find_method("length2");
    const ecs::any& frozen = obj;
    ecs::any len = length2.invoke(frozen, {});
    CHECK(len.holds());
    CHECK(len.as<float>() == 52.0f);

    // A mutating method on a const object: empty any, no call.
    CHECK(!translate.invoke(frozen, args).holds());
    CHECK(obj.as<Vec2>().x == 4.0f);

    // Wrong arity / wrong argument types: empty any, no call.
    CHECK(!translate.invoke(obj, {}).holds());
    std::array<ecs::any, 2> bad{ecs::any::make<int>(1), ecs::any::make<int>(2)};
    CHECK(!translate.invoke(obj, bad).holds());
    CHECK(obj.as<Vec2>().x == 4.0f);
}

void test_reflection_construct()
{
    section("reflection: type-erased construction");

    const ecs::reflection r = ecs::reflection_of<Vec2>();

    // The registered (float, float) constructor matches.
    std::array<ecs::any, 2> args{ecs::any::make<float>(5.0f), ecs::any::make<float>(12.0f)};
    ecs::any built = r.construct(args);
    CHECK(built.holds());
    CHECK(built.as<Vec2>().x == 5.0f);
    CHECK(built.as<Vec2>().length2() == 169.0f);

    // No matching arity or types: empty any.
    CHECK(!r.construct({}).holds());  // () was never registered
    std::array<ecs::any, 2> bad{ecs::any::make<int>(1), ecs::any::make<int>(2)};
    CHECK(!r.construct(bad).holds());

    // reflect_all registers identity for whole manifests in one call.
    ecs::reflect_all(ecs::types<Pos, Vel>{});
    CHECK(static_cast<bool>(ecs::reflection_of<Pos>()));
    CHECK(ecs::reflection_of<Vel>().size_bytes() == sizeof(Vel));
}
