#include "target.h"

#include <cmath>
#include <cstring>

namespace target {

namespace {

float Dist3(const game::Entity& a, const game::Entity& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct Cand {
    const game::Entity* e = nullptr;
    float dist = 0.0f;
    float hp_pct = 1.0f;
    float kills = 0.0f;
    float gold = 0.0f;
    float bounty = 0.0f;
    float screen_dist = 0.0f;   // px from screen centre, when it projects
};

// Does `c` beat `best` under the priority comparator? Distance is always the
// tiebreak, so a comparator with nothing to separate on still orders sanely.
bool Better(const Cand& c, const Cand& best, Priority p) {
    switch (p) {
        case Priority::LowestHp:
            if (c.e->hp != best.e->hp) return c.e->hp < best.e->hp;
            break;
        case Priority::LowestHpPct:
            if (c.hp_pct != best.hp_pct) return c.hp_pct < best.hp_pct;
            break;
        case Priority::MostKills:
            if (c.kills != best.kills) return c.kills > best.kills;
            break;
        case Priority::Richest:
            if (c.gold != best.gold) return c.gold > best.gold;
            break;
        case Priority::MostFed:
            if (c.bounty != best.bounty) return c.bounty > best.bounty;
            break;
        case Priority::ScreenCentre:
            if (c.screen_dist != best.screen_dist)
                return c.screen_dist < best.screen_dist;
            break;
        case Priority::Closest:
            break;
    }
    return c.dist < best.dist;
}

}  // namespace

bool ModeAllows(Mode m, Class c) {
    switch (m) {
        case Mode::PlayersOnly:  return c == Class::Hero;
        case Mode::MonstersOnly: return c != Class::Hero;
        case Mode::Both:         return true;
    }
    return false;
}

Class MonsterClass(const game::Entity& e) {
    if (e.is_soldier) return Class::Minion;
    const char* n = game::MonsterName(e.id);
    if (!n) return Class::Creep;
    if (strstr(n, "Turtle")) return Class::Turtle;
    if (strstr(n, "Lord"))   return Class::Lord;
    if (strstr(n, "Buff"))   return Class::Buff;
    return Class::Creep;
}

Result Select(const game::Entity& self,
              const game::Entity* players, int n_players,
              const game::Entity* monsters, int n_monsters,
              const Query& q) {
    Result r;

    // Without a camp there is no friend/foe split, and guessing would aim at
    // our own team — the same failure the ESP colours were fixed for.
    const int local = game::LocalCamp();
    if (local <= 0) return r;

    // One slot per position in the class preference order: the best candidate
    // of whichever class sits at that rank.
    Cand best[kClassCount];
    bool have[kClassCount] = {false, false, false, false, false, false};

    const float sw = static_cast<float>(game::ScreenWidth());
    const float sh = static_cast<float>(game::ScreenHeight());
    const bool want_screen = (q.priority == Priority::ScreenCentre) &&
                             sw > 0.0f && sh > 0.0f;

    auto consider = [&](const game::Entity& e, Class cls) {
        if (!e.obj) return;
        if (q.exclude_dead && e.is_dead) return;
        if (q.enemies_only && e.camp == local) return;
        if (q.require_visible && !e.minimap_visible) return;
        if (q.cc_only && !e.is_frozen) return;
        if (!ModeAllows(q.mode, cls)) return;
        const int rank = q.classes.Rank(cls);
        if (rank == kNotListed) return;

        const float d = Dist3(e, self);
        // Attack-range mode: prefer the entity's own reach when the game
        // exposes it, falling back to the configured range.
        float range = q.range;
        if (q.use_atk_range && e.atk_range > 0.5f) range = e.atk_range;
        if (d > range) return;

        const float pct = (e.hp_max > 0.0f) ? (e.hp / e.hp_max) : 1.0f;

        Cand c{&e, d, pct};
        c.kills = static_cast<float>(e.kills);
        c.gold = static_cast<float>(e.gold);
        c.bounty = static_cast<float>(e.bounty);

        // Screen-centre priority needs a projection; a target that does not
        // project (behind the camera) sorts last rather than being dropped.
        c.screen_dist = 1e9f;
        if (want_screen) {
            float sx = 0, sy = 0;
            if (game::WorldToScreenEx(e.x, e.y, e.z, e.pos_is_unity, &sx, &sy)) {
                const float dx = sx - sw * 0.5f;
                const float dy = sy - sh * 0.5f;
                c.screen_dist = std::sqrt(dx * dx + dy * dy);
            }
        }

        if (!have[rank] || Better(c, best[rank], q.priority)) {
            best[rank] = c;
            have[rank] = true;
        }
    };

    if (players) {
        for (int i = 0; i < n_players; i++)
            consider(players[i], players[i].is_soldier ? Class::Minion : Class::Hero);
    }
    if (monsters) {
        for (int i = 0; i < n_monsters; i++)
            consider(monsters[i], MonsterClass(monsters[i]));
    }

    // The first non-empty bucket in preference order wins, which is what makes
    // the order a priority list rather than a filter.
    for (int rank = 0; rank < kClassCount; rank++) {
        if (!have[rank]) continue;
        r.ent = best[rank].e;
        r.dist = best[rank].dist;
        r.hp_pct = best[rank].hp_pct;
        r.cls = q.classes.order[rank];
        return r;
    }
    return r;
}

const char* ModeName(Mode m) {
    switch (m) {
        case Mode::PlayersOnly:  return "Players Only";
        case Mode::MonstersOnly: return "Monsters Only";
        case Mode::Both:         return "Both (Adaptive)";
    }
    return "?";
}

const char* PriorityName(Priority p) {
    switch (p) {
        case Priority::Closest:      return "Closest Distance";
        case Priority::LowestHp:     return "Lowest HP";
        case Priority::LowestHpPct:  return "Lowest HP %";
        case Priority::MostKills:    return "Most Kills";
        case Priority::Richest:      return "Richest";
        case Priority::MostFed:      return "Most Fed (bounty)";
        case Priority::ScreenCentre: return "Closest to crosshair";
    }
    return "?";
}

const char* ClassName(Class c) {
    switch (c) {
        case Class::Minion: return "Minion";
        case Class::Hero:   return "Hero";
        case Class::Turtle: return "Turtle";
        case Class::Lord:   return "Lord";
        case Class::Buff:   return "Buff";
        case Class::Creep:  return "Creep";
    }
    return "?";
}

}  // namespace target
