#pragma once
// Shared targeting policy.
//
// One selection routine for every combat feature (aim assist, skill aim, the
// Kimmy press-and-hold, combos, retribution proximity checks), so behaviour is
// consistent and the menu only needs one block of controls.
//
// Selection is two-stage, matching how the reference mods describe it:
//   1. mode   — which classes are eligible at all (players / monsters / both)
//   2. order  — an ordered class preference; the first non-empty bucket wins
// then the priority comparator picks within that bucket.
//
// Targeting lives on the entity pool kind, not on a per-entity flag: the game
// keeps heroes and lane soldiers in BattleManager.m_ShowPlayers and jungle
// units in m_ShowMonsters, so the list an entity came from is the reliable
// classifier. ShowEntity's own flags cannot separate a jungle monster from a
// hero (is_hero is derived as "not a soldier", which monsters also satisfy).

#include "game.h"

namespace target {

enum class Mode : int {
    PlayersOnly = 0,   // enemy heroes only
    MonstersOnly = 1,  // jungle monsters and lane minions
    Both = 2,          // adaptive: anything hostile
};

// Priority comparator, applied inside the winning class bucket. Distance is
// always the final tiebreak, so a comparator with nothing to separate on still
// orders sanely.
enum class Priority : int {
    Closest = 0,
    LowestHp = 1,
    LowestHpPct = 2,
    MostKills = 3,      // KDA kills, from the ShowPlayer companion object
    Richest = 4,        // live gold, from ShowPlayer
    MostFed = 5,        // kill bounty (kill-streak value), from ShowPlayer
    ScreenCentre = 6,   // closest to the crosshair / screen centre
};

// Entity classes. Monsters are split by kind now (the game's own config ids
// distinguish them), so a priority list can say "Lord first, then heroes, then
// creeps" instead of lumping every jungle unit into one bucket.
enum class Class : int {
    Minion = 0,
    Hero = 1,
    Turtle = 2,
    Lord = 3,
    Buff = 4,
    Creep = 5,
};

constexpr int kClassCount = 6;
constexpr int kNotListed = 99;

// Ordered class preference — index 0 is the highest priority. Entities are
// bucketed by class and the first non-empty bucket wins, so "minions first,
// then monsters, then heroes" is a list, not a numeric weight.
struct ClassOrder {
    Class order[kClassCount];
    ClassOrder() {
        order[0] = Class::Hero;
        order[1] = Class::Lord;
        order[2] = Class::Turtle;
        order[3] = Class::Buff;
        order[4] = Class::Creep;
        order[5] = Class::Minion;
    }
    // Position in the list, or kNotListed when the class is excluded.
    int Rank(Class c) const {
        for (int i = 0; i < kClassCount; i++)
            if (order[i] == c) return i;
        return kNotListed;
    }
    void Move(int from, int to) {
        if (from < 0 || from >= kClassCount || to < 0 || to >= kClassCount) return;
        const Class c = order[from];
        if (from < to)
            for (int i = from; i < to; i++) order[i] = order[i + 1];
        else
            for (int i = from; i > to; i--) order[i] = order[i - 1];
        order[to] = c;
    }
};

struct Query {
    float range = 12.0f;
    Mode mode = Mode::PlayersOnly;
    Priority priority = Priority::Closest;
    ClassOrder classes;
    bool enemies_only = true;      // never select the local camp
    bool exclude_dead = true;
    bool require_visible = false;  // only units the game is currently drawing
    // v2 filters
    bool cc_only = false;          // only targets the game flags as frozen
    bool use_atk_range = false;    // prefer the entity's own attack range
};

struct Result {
    const game::Entity* ent = nullptr;
    float dist = 0.0f;
    float hp_pct = 0.0f;
    Class cls = Class::Hero;
    bool ok() const { return ent != nullptr; }
};

// Which classes a mode makes eligible.
bool ModeAllows(Mode m, Class c);

// Build a preference order from the config's indices (values are Class).
inline ClassOrder MakeOrder(const int idx[kClassCount]) {
    ClassOrder o;
    for (int i = 0; i < kClassCount; i++) {
        const int v = idx[i];
        o.order[i] = (v >= 0 && v < kClassCount) ? static_cast<Class>(v)
                                                 : Class::Hero;
    }
    return o;
}

// Classify one monster entity from its config id, mirroring game::MonsterCategory
// so targeting and ESP agree on what a unit is.
Class MonsterClass(const game::Entity& e);

// Pick a target. `players` is the BattleManager.m_ShowPlayers snapshot and
// `monsters` the m_ShowMonsters snapshot; either may be null/empty. `self` is
// the local player, used for range and the camp comparison.
Result Select(const game::Entity& self,
              const game::Entity* players, int n_players,
              const game::Entity* monsters, int n_monsters,
              const Query& q);

const char* ModeName(Mode m);
const char* PriorityName(Priority p);
const char* ClassName(Class c);

}  // namespace target
