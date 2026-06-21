#include "test_harness.hpp"

#include <array>

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
    static constexpr std::string_view ecs_label = "meta.labeled";
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

struct MetaBlob
{
    double lanes[8] = {};
};

struct MetaProbe
{
    int a = 7;
};

TEST(Meta, AnyBasics)
{
    ecs::any small = ecs::any::make<int>(42);
    EXPECT_TRUE(small.holds());
    EXPECT_EQ(small.type_hash(), ecs::hash_of<int>());
    EXPECT_EQ(*small.try_as<int>(), 42);
    EXPECT_EQ(small.try_as<float>(), nullptr);
    EXPECT_EQ(small.as<int>(), 42);

    ecs::any big = ecs::any::make<MetaBlob>();
    big.as<MetaBlob>().lanes[7] = 9.0;
    ecs::any big2 = big;
    big.as<MetaBlob>().lanes[7] = 1.0;
    EXPECT_EQ(big2.as<MetaBlob>().lanes[7], 9.0);

    {
        ecs::any aligned = ecs::any::make<Aligned>();
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned.data()) % 64, 0);
    }
    EXPECT_EQ(misaligned_payloads, 0);

    {
        const int live_before = Counted::live;
        ecs::any a = ecs::any::make<Counted>(7);
        ecs::any b = a;
        ecs::any c = std::move(a);
        EXPECT_FALSE(a.holds());
        EXPECT_EQ(Counted::live, live_before + 2);
        b.reset();
        EXPECT_EQ(Counted::live, live_before + 1);
        c.reset();
        EXPECT_EQ(Counted::live, live_before);
    }

    {
        Vec2 v{1, 2};
        ecs::any view = ecs::any::ref(v);
        view.as<Vec2>().x = 10;
        EXPECT_EQ(v.x, 10);
        ecs::any view2 = view;
        view2.as<Vec2>().y = 20;
        EXPECT_EQ(v.y, 20);
    }

#if ECS_CHECKS
    {
        violation_scope guard;
        ecs::any pinned = ecs::any::make<MetaNoCopy>(5);
        ecs::any stolen = pinned;
        EXPECT_EQ(violations_seen, 1);
        EXPECT_FALSE(stolen.holds());
        EXPECT_TRUE(pinned.holds());
    }
#endif

    const ecs::any none;
    EXPECT_FALSE(none.holds());
    EXPECT_EQ(none.type_hash(), 0);
    EXPECT_EQ(none.try_as<int>(), nullptr);
}

TEST(Meta, ReflectionRegistry)
{
    ecs::reflect<Vec2>()
        .field<&Vec2::x>("x")
        .field<&Vec2::y>("y")
        .method<&Vec2::length2>("length2")
        .method<&Vec2::translate>("translate")
        .construct<float, float>();
    ecs::reflect<MetaLabeled>().field<&MetaLabeled::v>("v");

    const ecs::reflection by_type = ecs::reflection_of<Vec2>();
    EXPECT_TRUE(static_cast<bool>(by_type));
    EXPECT_EQ(by_type.hash(), ecs::hash_of<Vec2>());
    EXPECT_EQ(by_type.size_bytes(), sizeof(Vec2));
    EXPECT_EQ(by_type.align(), alignof(Vec2));

    const ecs::reflection by_hash = ecs::reflection_of(ecs::hash_of<Vec2>());
    EXPECT_EQ(by_hash.hash(), by_type.hash());

    const ecs::reflection by_name = ecs::reflection_of("meta.labeled");
    EXPECT_TRUE(static_cast<bool>(by_name));
    EXPECT_EQ(by_name.hash(), ecs::hash_of<MetaLabeled>());
    EXPECT_EQ(by_name.name(), "meta.labeled");

    EXPECT_FALSE(ecs::reflection_of(0x1234u));
    EXPECT_FALSE(ecs::reflection_of("meta.never_registered"));

#if ECS_CHECKS
    {
        violation_scope guard;
        ecs::reflect<Vec2>();
        EXPECT_EQ(violations_seen, 1);
        EXPECT_TRUE(static_cast<bool>(ecs::reflection_of<Vec2>()));
    }
#endif
}

TEST(Meta, EmptyReflectionHandlesAreSafe)
{
    const ecs::field f;
    EXPECT_FALSE(static_cast<bool>(f));
    EXPECT_TRUE(f.name().empty());
    EXPECT_EQ(f.type_hash(), 0U);

    const ecs::method m;
    EXPECT_FALSE(static_cast<bool>(m));
    EXPECT_TRUE(m.name().empty());

    const ecs::reflection r;
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_TRUE(r.name().empty());
    EXPECT_EQ(r.hash(), 0U);
    EXPECT_EQ(r.size_bytes(), 0U);
    EXPECT_EQ(r.align(), 0U);
}

TEST(Meta, ReflectionFieldHandlesSurviveAppend)
{
    ecs::reflect<MetaProbe>().field<&MetaProbe::a>("a");
    const ecs::field fa = ecs::reflection_of<MetaProbe>().find_field("a");
    ASSERT_TRUE(static_cast<bool>(fa));

    {
#if ECS_CHECKS
        const violation_scope guard;
#endif
        ecs::reflect<MetaProbe>()
            .field<&MetaProbe::a>("a02")
            .field<&MetaProbe::a>("a03")
            .field<&MetaProbe::a>("a04")
            .field<&MetaProbe::a>("a05")
            .field<&MetaProbe::a>("a06")
            .field<&MetaProbe::a>("a07")
            .field<&MetaProbe::a>("a08")
            .field<&MetaProbe::a>("a09");
    }

    ASSERT_EQ(fa.name(), "a");
    const MetaProbe obj{};
    EXPECT_EQ(fa.get_at(&obj).as<int>(), 7);
}

TEST(Meta, ReflectionFields)
{
    const ecs::reflection r = ecs::reflection_of<Vec2>();
    ecs::any obj = ecs::any::make<Vec2>(Vec2{3, 4});

    const ecs::field fx = r.find_field("x");
    EXPECT_TRUE(static_cast<bool>(fx));
    EXPECT_EQ(fx.name(), "x");
    EXPECT_EQ(fx.type_hash(), ecs::hash_of<float>());

    ecs::any x = fx.get(obj);
    EXPECT_EQ(x.as<float>(), 3.0f);
    EXPECT_TRUE(fx.set(obj, ecs::any::make<float>(30.0f)));
    EXPECT_EQ(obj.as<Vec2>().x, 30.0f);

    EXPECT_FALSE(fx.set(obj, ecs::any::make<int>(1)));
    EXPECT_EQ(obj.as<Vec2>().x, 30.0f);

    ecs::any wrong = ecs::any::make<int>(5);
    EXPECT_FALSE(fx.get(wrong).holds());

    EXPECT_FALSE(r.find_field("z"));
    std::array<std::string_view, 2> order{};
    std::size_t at = 0;
    r.each_field([&](const ecs::field& f) { order[at++] = f.name(); });
    EXPECT_EQ(at, 2);
    EXPECT_EQ(order[0], "x");
    EXPECT_EQ(order[1], "y");
}

TEST(Meta, ReflectionMethods)
{
    const ecs::reflection r = ecs::reflection_of<Vec2>();
    ecs::any obj = ecs::any::make<Vec2>(Vec2{3, 4});

    const ecs::method translate = r.find_method("translate");
    EXPECT_TRUE(static_cast<bool>(translate));
    std::array<ecs::any, 2> args{ecs::any::make<float>(1.0f), ecs::any::make<float>(2.0f)};
    translate.invoke(obj, args);
    EXPECT_EQ(obj.as<Vec2>().x, 4.0f);
    EXPECT_EQ(obj.as<Vec2>().y, 6.0f);

    const ecs::method length2 = r.find_method("length2");
    const ecs::any& frozen = obj;
    ecs::any len = length2.invoke(frozen, {});
    EXPECT_TRUE(len.holds());
    EXPECT_EQ(len.as<float>(), 52.0f);

    EXPECT_FALSE(translate.invoke(frozen, args).holds());
    EXPECT_EQ(obj.as<Vec2>().x, 4.0f);

    EXPECT_FALSE(translate.invoke(obj, {}).holds());
    std::array<ecs::any, 2> bad{ecs::any::make<int>(1), ecs::any::make<int>(2)};
    EXPECT_FALSE(translate.invoke(obj, bad).holds());
    EXPECT_EQ(obj.as<Vec2>().x, 4.0f);
}

TEST(Meta, ReflectionConstruct)
{
    const ecs::reflection r = ecs::reflection_of<Vec2>();

    std::array<ecs::any, 2> args{ecs::any::make<float>(5.0f), ecs::any::make<float>(12.0f)};
    ecs::any built = r.construct(args);
    EXPECT_TRUE(built.holds());
    EXPECT_EQ(built.as<Vec2>().x, 5.0f);
    EXPECT_EQ(built.as<Vec2>().length2(), 169.0f);

    EXPECT_FALSE(r.construct({}).holds());
    std::array<ecs::any, 2> bad{ecs::any::make<int>(1), ecs::any::make<int>(2)};
    EXPECT_FALSE(r.construct(bad).holds());

    ecs::reflect_all(ecs::types<Pos, Vel>{});
    EXPECT_TRUE(static_cast<bool>(ecs::reflection_of<Pos>()));
    EXPECT_EQ(ecs::reflection_of<Vel>().size_bytes(), sizeof(Vel));
}

TEST(Meta, ReflectionEcsFields)
{
    ecs::registry w;

    ecs::reflect_all(ecs::types<Hp, TagA>{});
    const ecs::entity e = w.create(Pos{1}, Hp{70});
    w.add<TagA>(e);

    const ecs::reflection rhp = ecs::reflection_of<Hp>();
    {
#if ECS_CHECKS
        violation_scope guard;
#endif
        ecs::reflect<Hp>().field<&Hp::hp>("hp");
    }
    void* bytes = w.find_pool<Hp>().raw(e);
    EXPECT_NE(bytes, nullptr);
    const ecs::field fhp = rhp.find_field("hp");
    EXPECT_EQ(fhp.get_at(bytes).as<int>(), 70);
    EXPECT_TRUE(fhp.set_at(bytes, ecs::any::make<int>(55)));
    EXPECT_EQ(w.get<Hp>(e).hp, 55);
    EXPECT_FALSE(fhp.set_at(bytes, ecs::any::make<float>(1.0f)));
    EXPECT_EQ(w.get<Hp>(e).hp, 55);

    std::size_t reflected = 0;
    std::size_t tags_seen = 0;
    w.components_of(e,
                    [&](const ecs::pool_info& info)
                    {
                        const ecs::reflection r = ecs::reflection_of(info.name_hash);
                        if (!r)
                        {
                            return;
                        }
                        ++reflected;
                        void* raw = w.find_pool(info.id).raw(e);
                        if (raw == nullptr)
                        {
                            ++tags_seen;
                        }
                    });
    EXPECT_EQ(reflected, 3);
    EXPECT_EQ(tags_seen, 1);

    EXPECT_TRUE(RegistryValid(w));
}

TEST(Meta, ReflectionEcsVerbs)
{
    ecs::registry w;

    const ecs::reflection rhp = ecs::reflection_of<Hp>();
    const ecs::reflection rtag = ecs::reflection_of<TagA>();
    const ecs::entity e = w.create(Pos{1});

    int arrivals = 0;
    const ecs::scoped_hook on_hp(
        w,
        w.on_add<Hp>([](ecs::registry&, ecs::entity, void* user) { ++*static_cast<int*>(user); },
                     &arrivals));
    EXPECT_TRUE(rhp.add_to(w, e, ecs::any::make<Hp>(Hp{9})));
    EXPECT_EQ(w.get<Hp>(e).hp, 9);
    EXPECT_EQ(arrivals, 1);
    EXPECT_TRUE(rhp.present_on(w, e));

    EXPECT_FALSE(rhp.add_to(w, e, ecs::any::make<Hp>(Hp{1})));
    const ecs::entity dead = w.create();
    w.destroy(dead);
    EXPECT_FALSE(rhp.add_to(w, dead, ecs::any::make<Hp>(Hp{1})));
    EXPECT_FALSE(rhp.present_on(w, dead));

    EXPECT_FALSE(rhp.add_to(w, w.create(), ecs::any::make<int>(5)));
    const ecs::entity fresh = w.create();
    EXPECT_TRUE(rhp.add_to(w, fresh, ecs::any{}));
    EXPECT_EQ(w.get<Hp>(fresh).hp, 0);

    EXPECT_TRUE(rtag.add_to(w, e, ecs::any{}));
    EXPECT_TRUE(w.has<TagA>(e));

    EXPECT_TRUE(rhp.remove_from(w, e));
    EXPECT_FALSE(rhp.present_on(w, e));
    EXPECT_FALSE(rhp.remove_from(w, e));

    EXPECT_FALSE(ecs::reflection_of("meta.never_registered"));

    EXPECT_TRUE(RegistryValid(w));
}

namespace
{
struct AggVec3
{
    float x, y, z;
};
struct AggEmpty
{
};
struct AggMixed
{
    int a;
    float b;
    char c;
};
}

TEST(Meta, AggregateReflection)
{
    static_assert(ecs::reflectable_aggregate<AggVec3>);
    static_assert(!ecs::reflectable_aggregate<int>);
    static_assert(ecs::field_count_v<AggVec3> == 3);
    static_assert(ecs::field_count_v<AggEmpty> == 0);
    static_assert(ecs::field_count_v<AggMixed> == 3);

    AggVec3 v{1.0F, 2.0F, 3.0F};
    auto tied = ecs::tie_fields(v);
    static_assert(std::tuple_size_v<decltype(tied)> == 3);
    std::get<1>(tied) = 9.0F;
    EXPECT_EQ(v.y, 9.0F);

    float sum = 0.0F;
    ecs::for_each(v, [&](auto& m) { sum += static_cast<float>(m); });
    EXPECT_EQ(sum, 13.0F);

    const AggVec3 cv{4.0F, 5.0F, 6.0F};
    int visited = 0;
    ecs::for_each(cv, [&](const auto&) { ++visited; });
    EXPECT_EQ(visited, 3);

    auto snap = ecs::as_tuple(v);
    static_assert(std::same_as<decltype(snap), std::tuple<float, float, float>>);
    EXPECT_EQ(std::get<0>(snap), 1.0F);
    EXPECT_EQ(std::get<2>(snap), 3.0F);
}

namespace
{
struct AutoReg
{
    int hp;
    float speed;
};
}

TEST(Meta, AutoFieldRegistration)
{
    ecs::reflect<AutoReg>().fields("hp", "speed");
    const ecs::reflection r = ecs::reflection_of<AutoReg>();
    ASSERT_TRUE(static_cast<bool>(r));

    std::array<std::string_view, 2> order{};
    std::size_t at = 0;
    r.each_field([&](const ecs::field& f) { order[at++] = f.name(); });
    EXPECT_EQ(at, 2);
    EXPECT_EQ(order[0], "hp");
    EXPECT_EQ(order[1], "speed");

    const ecs::field fs = r.find_field("speed");
    ASSERT_TRUE(static_cast<bool>(fs));
    EXPECT_EQ(fs.type_hash(), ecs::hash_of<float>());

    ecs::any obj = ecs::any::make<AutoReg>(AutoReg{10, 2.5F});
    EXPECT_EQ(fs.get(obj).as<float>(), 2.5F);
    EXPECT_TRUE(fs.set(obj, ecs::any::make<float>(9.0F)));
    EXPECT_EQ(obj.as<AutoReg>().speed, 9.0F);
    EXPECT_EQ(obj.as<AutoReg>().hp, 10);
}

namespace
{
struct Nested
{
    AggVec3 pos;
    int id;
};
}

TEST(Meta, ReflectionDerivedSerialization)
{
    Nested n{{1.0F, 2.0F, 3.0F}, 42};

    int leaves = 0;
    ecs::for_each_leaf(n, [&](const auto&) { ++leaves; });
    EXPECT_EQ(leaves, 4);

    byte_writer w;
    ecs::write_fields(w, n);
    Nested back{};
    byte_reader r{&w.data};
    ecs::read_fields(r, back);
    EXPECT_EQ(back.id, 42);
    EXPECT_EQ(back.pos.y, 2.0F);
    EXPECT_TRUE(ecs::fields_equal(back.pos, n.pos));

    EXPECT_TRUE(ecs::fields_equal(AggVec3{1.0F, 2.0F, 3.0F}, AggVec3{1.0F, 2.0F, 3.0F}));
    EXPECT_FALSE(ecs::fields_equal(AggVec3{1.0F, 2.0F, 3.0F}, AggVec3{1.0F, 9.0F, 3.0F}));

    Nested m{{1.0F, 2.0F, 3.0F}, 42};
    EXPECT_TRUE(ecs::fields_equal(n, m));
    m.pos.z = 99.0F;
    EXPECT_FALSE(ecs::fields_equal(n, m));
    EXPECT_EQ(ecs::hash_fields(n), ecs::hash_fields(Nested{{1.0F, 2.0F, 3.0F}, 42}));
    EXPECT_NE(ecs::hash_fields(n), ecs::hash_fields(m));
}

namespace meta_names
{
struct NVec3
{
    float x, y, z;
};
struct NAuto
{
    int health;
    int mana;
};
}

TEST(Meta, FieldNames)
{
    if (ecs::field_names_supported)
    {
        EXPECT_EQ((ecs::field_name<meta_names::NVec3, 0>()), "x");
        EXPECT_EQ((ecs::field_name<meta_names::NVec3, 1>()), "y");
        EXPECT_EQ((ecs::field_name<meta_names::NVec3, 2>()), "z");
    }

    ecs::reflect<meta_names::NAuto>().fields();
    const ecs::reflection r = ecs::reflection_of<meta_names::NAuto>();
    ASSERT_TRUE(static_cast<bool>(r));
    const std::string_view first = ecs::field_names_supported ? "health" : "0";
    EXPECT_TRUE(static_cast<bool>(r.find_field(first)));
}

namespace meta_names
{
struct VisComp
{
    int hp;
    float speed;
};
}

TEST(Meta, VisitComponents)
{
    ecs::reflect<meta_names::VisComp>().fields();
    ecs::registry w;
    const ecs::entity e = w.create(meta_names::VisComp{7, 1.5F}, Hp{99});

    int seen = 0;
    bool found = false;
    w.visit_components(e,
                       [&](const ecs::component_view& c)
                       {
                           ++seen;
                           if (c.name_hash() == ecs::hash_of<meta_names::VisComp>())
                           {
                               found = true;
                               EXPECT_TRUE(c.reflected());
                               int fields = 0;
                               c.each_field([&](const ecs::field&, const ecs::any&) { ++fields; });
                               EXPECT_EQ(fields, 2);
                               if (ecs::field_names_supported)
                               {
                                   EXPECT_EQ((ecs::field_name<meta_names::VisComp, 1>()), "speed");
                                   const ecs::any sp = c.value("speed");
                                   EXPECT_TRUE(sp.holds());
                                   const auto* p = sp.try_as<float>();
                                   EXPECT_NE(p, nullptr);
                                   if (p != nullptr)
                                   {
                                       EXPECT_EQ(*p, 1.5F);
                                   }
                               }
                           }
                       });
    EXPECT_EQ(seen, 2);
    EXPECT_TRUE(found);
    EXPECT_TRUE(RegistryValid(w));
}

struct FreeHelpers
{
    int a = 0;
    int b = 0;

    [[nodiscard]] int sum() const { return a + b; }
    void bump(int by) { a += by; }
};

TEST(Meta, FreeReflectionHelpers)
{
    ecs::reflect<FreeHelpers>()
        .field<&FreeHelpers::a>("a")
        .field<&FreeHelpers::b>("b")
        .method<&FreeHelpers::sum>("sum")
        .method<&FreeHelpers::bump>("bump");

    ecs::any obj = ecs::any::make<FreeHelpers>(FreeHelpers{3, 4});

    EXPECT_EQ(ecs::get(obj, "a").as<int>(), 3);
    EXPECT_TRUE(ecs::set(obj, "a", ecs::any::make<int>(30)));
    EXPECT_EQ(obj.as<FreeHelpers>().a, 30);
    EXPECT_FALSE(ecs::set(obj, "a", ecs::any::make<float>(1.0F)));
    EXPECT_FALSE(ecs::get(obj, "nope").holds());

    EXPECT_EQ(ecs::invoke(obj, "sum").as<int>(), 34);
    std::array<ecs::any, 1> args{ecs::any::make<int>(6)};
    ecs::invoke(obj, "bump", args);
    EXPECT_EQ(obj.as<FreeHelpers>().a, 36);

    bool found = false;
    ecs::for_each([&](const ecs::reflection& r)
                  { found = found || r.hash() == ecs::hash_of<FreeHelpers>(); });
    EXPECT_TRUE(found);

    ecs::any plain = ecs::any::make<int>(5);
    EXPECT_FALSE(ecs::get(plain, "x").holds());
    EXPECT_FALSE(ecs::set(plain, "x", ecs::any::make<int>(1)));
    EXPECT_FALSE(ecs::invoke(plain, "x").holds());
}

namespace
{
struct Flat3
{
    float a = 0;
    float b = 0;
    float c = 0;
};

struct MixedFields
{
    int n = 0;
    float f = 0;
};

struct WithCtor
{
    WithCtor() = default;
    int x = 0;
};

struct Hurtable
{
    int hp = 0;
    void hurt(int n) { hp -= n; }
};

template <class T>
concept Damageable = ecs::has_field<T, "hp"> && requires(T& t, int n) { t.hurt(n); };
}

TEST(Meta, ComponentContracts)
{
    static_assert(ecs::reflectable<Flat3>);
    static_assert(!ecs::reflectable<WithCtor>);

    static_assert(ecs::fields_all<Flat3, std::is_floating_point>);
    static_assert(!ecs::fields_all<MixedFields, std::is_floating_point>);
    static_assert(ecs::fields_all<MixedFields, std::is_arithmetic>);

    if constexpr (ecs::field_names_supported)
    {
        static_assert(ecs::has_field<Hurtable, "hp">);
        static_assert(!ecs::has_field<Hurtable, "mana">);
        static_assert(Damageable<Hurtable>);
        static_assert(!Damageable<Flat3>);
    }

    Hurtable h{10};
    h.hurt(3);
    EXPECT_EQ(h.hp, 7);
}
