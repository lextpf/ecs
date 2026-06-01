// Smoke test for the experimental named module: `import quiver;` must expose
// the public surface, including the std::hash<entity> specialization.
import quiver;

#include <cstdio>
#include <unordered_set>

namespace ecs = quiver;

struct Pos
{
    int x = 0;
};

struct Tag
{
};

int main()
{
    ecs::world w;
    const ecs::entity e = w.spawn(Pos{7});
    w.add<Tag>(e);
    int seen = 0;
    w.each<Pos, Tag>([&](ecs::entity, Pos& p) { seen = p.x; });

    // entity_hash: the portable spelling for module builds (std::hash<entity>
    // specializations in the GMF are not surfaced by every toolchain).
    std::unordered_set<ecs::entity, ecs::entity_hash> set;
    set.insert(e);

    const bool ok = seen == 7 && set.contains(e) && static_cast<bool>(w.validate());
    std::printf("module smoke: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
