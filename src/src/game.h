#pragma once
// MLBB battle model.
//
// Resolved at runtime by name through il2cpp reflection, so this survives game
// updates that move fields around. Names/classes come from the IL2CPP metadata
// dump (see docs/MLBB-INTERNALS.md).

#include "il2cpp.h"

namespace game {

struct Entity {
    void* obj = nullptr;          // Battle.ShowEntity*
    uint32_t guid = 0;
    int32_t id = 0;               // config id (hero id for heroes)
    int32_t type = 0;
    int32_t level = 0;
    int32_t camp = 0;
    bool same_camp = false;
    bool is_soldier = false;
    bool is_hero = false;
    bool is_fake = false;
    bool is_dead = false;
    // The game's own death flags, distinct from m_bDeadAnima (which is the
    // animation state and can lag or miss a corpse entirely):
    //   unity_blood_death   — the engine considers the blood bar dead
    //   unity_minimap_death — the engine considers the minimap icon dead
    // Either one being set means the entity is a corpse whose cached position
    // no longer tracks anything.
    bool blood_death = false;
    bool minimap_death = false;
    float hp = 0, hp_max = 0;
    float x = 0, y = 0, z = 0;
    bool pos_is_unity = false;    // position came from m_vUnityCachePosition
    float radius = 0;
    int32_t skin = 0;
    bool minimap_visible = false;
    bool blood_visible = false;
    int32_t grass_id = 0;
    int32_t dead_sight = 0;

    // ---- combat / targeting extras ----
    // These live on the ShowPlayer companion object (KDA, economy) or on
    // ShowEntity itself (frozen, attack range). All default to 0 when the
    // source is unavailable, and every consumer treats 0 as "unknown" rather
    // than as a real value.
    bool is_frozen = false;       // m_bIsFrozen — hard CC
    float atk_range = 0.0f;       // m_AtkDis — the entity's own reach
    int32_t kills = 0;            // ShowPlayer.m_killNum
    int32_t deaths = 0;           // ShowPlayer.m_deadNum
    int32_t assists = 0;          // ShowPlayer.m_assistNum
    int32_t gold = 0;             // ShowPlayer._iGold (live)
    int32_t total_gold = 0;       // ShowPlayer._iTotalGold
    int32_t bounty = 0;           // ShowPlayer.m_KillBounty (kill streak value)
};

// Bind classes/fields; safe to call repeatedly (retries until the battle scene
// classes are actually loaded, since Rain starts before a match begins).
bool Init();
bool Ready();

// Snapshot every player entity. Returns the number written.
int SnapshotPlayers(Entity* out, int max);

// Snapshot jungle/camp entities (turtle, lord, buff creeps) from
// BattleManager.m_ShowMonsters. Returns the number written.
int SnapshotMonsters(Entity* out, int max);

// The local player's own ShowEntity (BattleManager.m_LocalPlayerShow).
// Returns false when no match is live or the manager is not bound.
bool SelfEntity(Entity* out);

// Local player's camp, so ESP can classify enemies.
int LocalCamp();
// Logic-layer authoritative camp (LogicBattleManager.m_SelfCampType); -1 when
// unavailable.
int LogicCamp();

// World -> screen using the Unity camera's matrices. Returns false when the
// point is behind the camera or the camera is unavailable.
bool WorldToScreen(float wx, float wy, float wz, float* sx, float* sy);

// Space-aware projection. Unity-space points project straight through the
// renderer's matrices; logic-space points are first rotated into Unity space
// (the map sits rotated 45 degrees in the Unity world) so ESP boxes land on
// the sprite instead of near it.
bool WorldToScreenEx(float wx, float wy, float wz, bool unity_space,
                     float* sx, float* sy);

// Call once per rendered frame. The camera matrices are re-read from here, so
// the projection never lags the moving camera.
void BeginFrame();

// ---- per-frame snapshot ----
// One walk of each entity list per rendered frame, shared by every consumer
// (ESP, minimap, targeting, the Status table). Each of those used to do its own
// walk, so a busy battle frame paid the same 2 KB process_vm_readv per entity
// six times over.
//
// Built lazily on first access after BeginFrame(), so a frame that draws
// nothing costs nothing. Valid until the next BeginFrame().
//
// Objective tracking (turtle/lord HP + pit positions) runs here too. It used to
// live inside the entity-ESP draw, which meant turning Entity ESP off silently
// starved auto-retribution of the data it needs.
struct FrameSnapshot {
    Entity players[256];
    int n_players = 0;
    Entity monsters[256];
    int n_monsters = 0;
    Entity self;
    bool has_self = false;
    int local_camp = -1;
};

const FrameSnapshot& Snapshot();

// Screen size of the current surface, filled in by the overlay each frame.
void SetScreenSize(int w, int h);
int ScreenWidth();
int ScreenHeight();

// ---- maphack ----
// Force every enemy entity to be considered visible: minimap icon, blood bar and
// grass reveal. These are client-side presentation flags, so the server is never
// consulted. Returns how many entities were touched.
int ApplyMaphack(bool enabled);

// ---- drone view ----
// Per-frame camera modifier. Widens Camera.fieldOfView and modifies
// SmoothFollow.offSetPos. With drone_hook set, a GlossHook on the game's own
// set_offSetPos adds the delta inside the controller's write (the VYNIX model),
// so nothing races; otherwise it falls back to a guarded backing-field write,
// then to lifting the live camera transform. Stock values are captured when the
// drone engages and restored when it disengages. Returns how many writes were
// made.
int ApplyDrone();

// Human-readable drone hook state for the menu: "live", "off", or the reason
// it is not installed. Never returns null.
const char* DroneHookState();

// ---- skin changer ----
// Re-applies the configured skin id to the local player every ~1 s while
// skin_enabled is set, so respawns and scene loads keep it.
void TickSkin();

// Hero id -> display name (0 if unknown).
const char* HeroName(int hero_id);

// Jungle monster id -> display name (null if unknown; callers fall back to the
// raw id). Ids are MLBB config ids; calibrate from logcat on first snapshot.
const char* MonsterName(int id);

// Which jungle category an entity belongs to: "turtle", "lord", "buff",
// "minion" or "creep". Null when the id is unknown and it is not a minion.
const char* MonsterCategory(const Entity& e);

// The entity's own display name (m_RoleName), read live from the game.
// Returns false when the entity has no resolvable name string — callers fall
// back to the hardcoded hero/monster tables.
bool EntityName(const Entity& e, char* out, int outlen);

// ---- spell cooldowns (from m_ShowCoolDownComp -> _dicCD) ----
struct SpellCD {
    int32_t spell_id = 0;
    float remaining = 0;  // seconds, derived from uiCoolTime
    float max = 0;        // seconds, from originalMaxCdTime
    bool active = false;  // m_isCoolDown
};

// Snapshot the on-cooldown spells of one entity. Returns the number written.
int SnapshotSpellCDs(const Entity& e, SpellCD* out, int max);

// Spell id for hero skill slot 0/1/2 (S1/S2/S3), or 0 when the mapping does not
// resolve on this build. Callers gate on it when non-zero and fall back to the
// conservative all-or-nothing cooldown test otherwise.
int SkillSlotSpellId(const Entity& self, int slot);

// ---- awareness (game-owned reporter state, read-only) ----
// True when the game's reporter classes and their state fields bind — the
// detection surface exists on this build. Live values are not reachable
// (instances held by non-singleton managers).
bool ReporterFieldsBound();

// ---- objective alerts ----
// Per-frame HP tracking for the turtle/lord pits. Call once per frame with the
// monster snapshot; queries below report what the banner should show.
void TrackObjectives(const Entity* ents, int n);

// Pit world positions, for enemy-proximity checks near the objectives.
bool ObjectivePos(const char* which, float* x, float* z);

// The tracked objective's live HP from the pit monster. Returns false when
// the objective has not been seen this match.
bool TrackedObjectiveHp(const char* which, float* hp, float* hp_max);

// True while the objective was damaged within the last ~5 s and is contested.
bool TurtleContested();
bool LordContested();
// Seconds since the objective was last damaged (999 when never seen).
float TurtleSinceHit();
float LordSinceHit();

// Write m_ShowSkinId on an entity (skin changer). Returns false when the field
// is unavailable.
bool SetShowSkin(void* obj, int skin_id);

}  // namespace game
