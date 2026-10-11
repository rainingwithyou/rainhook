#include "game.h"

#include <android/log.h>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <mutex>
#include <sys/uio.h>
#include <unistd.h>

#include "config.h"
#include "Gloss.h"

#define LOG_TAG "Rain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace game {

using namespace il2cpp;

namespace {

// ---- resolved classes ----
Il2CppClass* g_show_entity = nullptr;
Il2CppClass* g_battle_mgr = nullptr;
Il2CppClass* g_camera_mgr = nullptr;
Il2CppClass* g_unity_camera = nullptr;

// ---- ShowEntity fields ----
struct ShowFields {
    Field guid, id, type, level, camp, same_camp, is_soldier, is_special_soldier,
        is_cache_hero, is_fake, hp, hp_max, hp_last, pos_cache, pos_unity, pos_obj,
        radius, skin, show_skin, minimap_visible, blood_visible, grass_id, dead_sight,
        dead_sight_eye, renderer_visible, outside_grass, ignore_outside_grass,
        head_offset, dead_anima, skill_invisible, is_dead_flag, move_speed, entity_name,
        cool_comp,  // m_ShowCoolDownComp — per-entity spell cooldowns
        is_frozen,  // m_bIsFrozen — hard CC, used by the cc_only targeting filter
        atk_range;  // m_AtkDis — the entity's own attack reach
};
ShowFields F;

// ---- ShowPlayer companion fields ----
// KDA and economy live on ShowPlayer, not ShowEntity. ShowPlayer derives from
// ShowEntity (which is why the existing m_LocalPlayerShow reads work), so the
// companion object can be read with the same field mechanism. Bound by name,
// like everything else — no hardcoded offsets.
struct PlayerFields {
    Field kills, deaths, assists, gold, total_gold, bounty;
};
PlayerFields PF;

// Forward declarations for the ShowPlayer identification helpers, which are
// defined further down but used by the field resolver above.
extern Il2CppClass* g_show_player_class;
bool IsShowPlayerObject(const void* img);

    // ---- manager fields ----
Field g_f_mgr_instance, g_f_players, g_f_dic_players, g_f_local_player;
Field g_f_local_players;  // BattleManager.m_LocalShowPlayers — our own team
Field g_f_monsters;       // BattleManager.m_ShowMonsters — jungle entities
Field g_f_dic_monsters;   // BattleManager.m_dicMonsterShow — same set, dict form
Field g_f_cam_instance, g_f_cam_pos, g_f_cam_rot_vect, g_f_cam_fov;
Field g_f_cam_player_id;  // ShowBattleCamera._uCameraPlayerId
Field g_f_cam_ymax;       // ShowBattleCamera.m_iYMax (camera drag clamp)
Field g_f_logic_instance, g_f_logic_self_camp, g_f_logic_real_self;
int g_logic_camp = -1;    // LogicBattleManager.m_SelfCampType (authoritative)

Field g_f_cam_world2cam, g_f_cam_proj;  // Unity Camera properties (via methods)
const MethodInfo* g_m_world2cam = nullptr;
const MethodInfo* g_m_proj = nullptr;
const MethodInfo* g_m_cam_main = nullptr;
// Unity camera transform access, used by the drone view to raise the real
// camera on top of what the game's controller just wrote.
const MethodInfo* g_m_get_transform = nullptr;
const MethodInfo* g_m_tf_getpos = nullptr;
const MethodInfo* g_m_tf_setpos = nullptr;

int g_sw = 0, g_sh = 0;
int g_local_camp = -1;
bool g_local_camp_strong = false;  // came from an authoritative source
std::mutex g_mtx;

// Match-scoped state invalidation: when BattleManager.Instance changes (new
// match), camp cached from the old match is poison — it inverts colors and
// points maphack at the wrong team.
//
// The pointer alone is not a reliable match boundary: IL2CPP reuses freed
// memory, so match 2's manager can land at match 1's address and the pointer
// comparison sees "same manager". The local player's guid is the second
// signal — it is per-entity and changes with the match even when the manager
// does not. Both are tracked; either changing triggers the reset.
uintptr_t g_mgr_seen = 0;
uint32_t g_local_guid_seen = 0;
// Frames a differing camp has been observed. A real team change persists; a
// teardown artifact does not, so the change is only accepted once it holds.
int g_camp_change_frames = 0;

void ResetMatchState(const char* why) {
    g_local_camp = -1;
    g_local_camp_strong = false;
    g_mgr_seen = 0;
    g_local_guid_seen = 0;
    g_camp_change_frames = 0;
    LOGI("game: match state reset (%s)", why);
}

bool ResolveShowEntity() {
    g_show_entity = FindClass("", "ShowEntity");
    if (!g_show_entity) return false;
    F.guid = GetField(g_show_entity, "m_uGuid");
    F.id = GetField(g_show_entity, "m_ID");
    F.type = GetField(g_show_entity, "m_iType");
    F.level = GetField(g_show_entity, "m_Level");
    F.camp = GetField(g_show_entity, "m_EntityCampType");
    F.same_camp = GetField(g_show_entity, "m_bSameCampType");
    F.is_soldier = GetField(g_show_entity, "IsSoldier");
    F.is_special_soldier = GetField(g_show_entity, "IsSpecialSoldier");
    F.is_cache_hero = GetField(g_show_entity, "IsCacheHero");
    F.is_fake = GetField(g_show_entity, "m_bIsFakeHero");
    F.hp = GetField(g_show_entity, "m_Hp");
    F.hp_max = GetField(g_show_entity, "m_HpMax");
    F.hp_last = GetField(g_show_entity, "m_HpLast");
    F.pos_cache = GetField(g_show_entity, "m_vCachePosition");
    F.pos_unity = GetField(g_show_entity, "m_vUnityCachePosition");
    F.pos_obj = GetField(g_show_entity, "m_EntityObjPos");
    F.radius = GetField(g_show_entity, "m_fPlayerRadius");
    F.skin = GetField(g_show_entity, "m_SkinId");
    F.show_skin = GetField(g_show_entity, "m_ShowSkinId");
    F.minimap_visible = GetField(g_show_entity, "m_bUnityMinimapVisible");
    F.blood_visible = GetField(g_show_entity, "m_bUnityBloodVisible");
    F.grass_id = GetField(g_show_entity, "m_iGrassId");
    F.dead_sight = GetField(g_show_entity, "m_DeadSightState");
    F.dead_sight_eye = GetField(g_show_entity, "m_bDeadSightStateEye");
    F.renderer_visible = GetField(g_show_entity, "m_bRendererVisible");
    F.outside_grass = GetField(g_show_entity, "visibleOutsideGrassInRange");
    F.ignore_outside_grass = GetField(g_show_entity, "ignoreVisibleOutsideGrassInRange");
    F.head_offset = GetField(g_show_entity, "m_HeadPosOffset");
    F.dead_anima = GetField(g_show_entity, "m_bDeadAnima");
    F.skill_invisible = GetField(g_show_entity, "_iSkillInvisibility");
    F.move_speed = GetField(g_show_entity, "m_dMoveSpeed");
    F.cool_comp = GetField(g_show_entity, "m_ShowCoolDownComp");
    F.is_frozen = GetField(g_show_entity, "m_bIsFrozen");
    F.atk_range = GetField(g_show_entity, "m_AtkDis");

    // ShowPlayer companion: KDA and economy. Resolved separately so a build
    // without these fields simply leaves the targeting extras at zero.
    Il2CppClass* sp = FindClass("", "ShowPlayer");
    if (sp) {
        g_show_player_class = sp;
        PF.kills = GetField(sp, "m_killNum");
        PF.deaths = GetField(sp, "m_deadNum");
        PF.assists = GetField(sp, "m_assistNum");
        PF.gold = GetField(sp, "_iGold");
        PF.total_gold = GetField(sp, "_iTotalGold");
        PF.bounty = GetField(sp, "m_KillBounty");
        LOGI("game: ShowPlayer fields kills=%d gold=%d bounty=%d",
             PF.kills.ok(), PF.gold.ok(), PF.bounty.ok());
    }

    // Jungle entities carry a display name. The exact field name has moved
    // between builds, so probe the candidates once and keep whichever binds.
    // m_RoleName verified present on this build (2.1.67.11733) from the metadata
    // dump; the older candidates are kept for forward compatibility.
    for (const char* cand : {"m_RoleName", "m_Name", "m_strName", "m_sName",
                             "m_EntityName", "m_ShowName", "name"}) {
        F.entity_name = GetField(g_show_entity, cand);
        if (F.entity_name.ok()) {
            LOGI("game: entity name field = %s", cand);
            break;
        }
    }

    // Field offsets are assigned by the runtime from the metadata field order,
    // so log them once: a wrong name silently yields offset 0 and would make the
    // ESP read the object header.
    LOGI("game: ShowEntity fields hp=%zu hpMax=%zu camp=%zu guid=%zu id=%zu "
         "type=%zu pos=%zu radius=%zu minimap=%zu",
         F.hp.offset, F.hp_max.offset, F.camp.offset, F.guid.offset, F.id.offset,
         F.type.offset, F.pos_cache.offset, F.radius.offset, F.minimap_visible.offset);

    return F.hp.ok() && F.camp.ok();
}

bool ResolveManagers() {
    // BattleManager lives in the global namespace; the Logic* entities are the
    // ones under "Battle".
    g_battle_mgr = FindClass("", "BattleManager");
    if (g_battle_mgr) {
        g_f_mgr_instance = GetField(g_battle_mgr, "Instance");
        g_f_players = GetField(g_battle_mgr, "m_ShowPlayers");
        g_f_dic_players = GetField(g_battle_mgr, "m_dicPlayerShow");
        g_f_local_player = GetField(g_battle_mgr, "m_LocalPlayerShow");
        g_f_local_players = GetField(g_battle_mgr, "m_LocalShowPlayers");
        g_f_monsters = GetField(g_battle_mgr, "m_ShowMonsters");
        g_f_dic_monsters = GetField(g_battle_mgr, "m_dicMonsterShow");
    }
    // Logic layer: server truth. m_SelfCampType on LogicBattleManager is the
    // authoritative local-camp source — better than any Show-side derivation.
    Il2CppClass* logic_mgr = FindClass("", "LogicBattleManager");
    if (logic_mgr) {
        g_f_logic_instance = GetField(logic_mgr, "Instance");
        g_f_logic_self_camp = GetField(logic_mgr, "m_SelfCampType");
        g_f_logic_real_self = GetField(logic_mgr, "m_RealSelfPlayer");
    }
    g_camera_mgr = FindClass("", "ShowBattleCamera");
    if (g_camera_mgr) {
        g_f_cam_instance = GetField(g_camera_mgr, "Instance");
        g_f_cam_pos = GetField(g_camera_mgr, "m_CamPos");
        g_f_cam_rot_vect = GetField(g_camera_mgr, "m_CamRotVect");
        g_f_cam_fov = GetField(g_camera_mgr, "m_iFieldOfView");
        g_f_cam_player_id = GetField(g_camera_mgr, "_uCameraPlayerId");
        g_f_cam_ymax = GetField(g_camera_mgr, "m_iYMax");
    }
    return g_battle_mgr && g_f_mgr_instance.ok() && g_f_players.ok();
}

// Unity camera, used for a correct world->screen projection.
void ResolveUnityCamera() {
    if (g_unity_camera) return;
    Il2CppClass* cam = FindClass("UnityEngine", "Camera");
    if (!cam) return;
    g_m_cam_main = api.class_get_method_from_name(cam, "get_main", 0);
    g_m_world2cam = api.class_get_method_from_name(cam, "get_worldToCameraMatrix", 0);
    g_m_proj = api.class_get_method_from_name(cam, "get_projectionMatrix", 0);
    // get_transform can sit on a parent (Component); walk up if needed.
    for (Il2CppClass* k = cam; k && !g_m_get_transform;
         k = api.class_get_parent ? api.class_get_parent(k) : nullptr) {
        g_m_get_transform = api.class_get_method_from_name(k, "get_transform", 0);
    }
    Il2CppClass* tf = FindClass("UnityEngine", "Transform");
    if (tf) {
        g_m_tf_getpos = api.class_get_method_from_name(tf, "get_position", 0);
        g_m_tf_setpos = api.class_get_method_from_name(tf, "set_position", 1);
    }
    if (g_m_cam_main && g_m_world2cam && g_m_proj) {
        g_unity_camera = cam;
        LOGI("game: Unity Camera bound (transform=%p)", (void*)g_m_get_transform);
    }
}

inline bool PtrOk(uintptr_t p) { return PlausiblePtr(p); }

// ---- ShowPlayer identification ----
//
// KDA and gold live on ShowPlayer, not ShowEntity. Rather than build a guid
// map (which would need a second walk and a lifetime guard per entry), the
// object's own class pointer is compared: an object is a ShowPlayer exactly
// when its Il2CppClass* is ShowPlayer's class or a subclass of it.
//
// IMPORTANT: the class pointer is read from the PEEKED COPY, never the live
// object. Calling il2cpp_object_get_class() on a live pointer dereferences it,
// which is exactly the use-after-free the peek boundary exists to prevent —
// on the second match load the entity list still holds entries the game has
// already torn down, and that raw deref crashed the client.
Il2CppClass* g_show_player_class = nullptr;

bool IsShowPlayerObject(const void* img) {
    if (!g_show_player_class || !img) return false;
    // An Il2CppObject's first member is its Il2CppClass* — already inside the
    // copy, so no live memory is touched.
    uintptr_t k = 0;
    memcpy(&k, img, sizeof(k));
    if (!PtrOk(k)) return false;
    // Walk up: derived types (per-mode player classes) still carry the fields.
    for (Il2CppClass* cur = reinterpret_cast<Il2CppClass*>(k); cur;
         cur = api.class_get_parent ? api.class_get_parent(cur) : nullptr) {
        if (cur == g_show_player_class) return true;
    }
    return false;
}

// ---- lifetime guard ----
//
// Managed objects are destroyed or replaced by the game at any moment
// (respawns, scene reloads, entity removal). A plausible-looking pointer can
// still point at a freed or unmapped object, and a plain dereference of an
// unmapped page kills the process. Every read below therefore goes through a
// snapshot boundary: the object's bytes are copied into local memory with
// process_vm_readv — which fails with EFAULT on unmapped pages instead of
// faulting — and parsed from the copy. Frees that leave the page mapped are
// caught by the field sanity checks in Fill().
bool SafePeek(const void* addr, size_t len, void* out);
bool PeekEntity(void* o, void* buf);
bool Fill(Entity* e, void* o, const void* img);
constexpr size_t kEntityPeek = 2048;

float ReadF(const void* o, const Field& f) {
    return f.ok() ? Read<float>(const_cast<void*>(o), f.offset) : 0.0f;
}
int32_t ReadI(const void* o, const Field& f) {
    return f.ok() ? Read<int32_t>(const_cast<void*>(o), f.offset) : 0;
}
bool ReadB(const void* o, const Field& f) {
    return f.ok() ? Read<uint8_t>(const_cast<void*>(o), f.offset) != 0 : false;
}

// A managed Vector2/Vector3 stored inline: {float x, y[, z]}.
void ReadVec3(const void* o, const Field& f, float* x, float* y, float* z) {
    *x = *y = *z = 0;
    if (!f.ok()) return;
    const uintptr_t p = reinterpret_cast<uintptr_t>(o) + f.offset;
    const float* v = reinterpret_cast<const float*>(p);
    *x = v[0];
    *y = v[1];
    *z = v[2];
}

// Managed containers vary by build, so identify by class name.
enum class Container { List, Array, Unknown };

Container ClassifyContainer(void* obj, int* count_out) {
    *count_out = 0;
    if (!obj || !PtrOk(reinterpret_cast<uintptr_t>(obj))) return Container::Unknown;
    Il2CppClass* k = api.object_get_class ? api.object_get_class(reinterpret_cast<Il2CppObject*>(obj)) : nullptr;
    const char* n = (k && api.class_get_name) ? api.class_get_name(k) : nullptr;
    if (!n) return Container::Unknown;

    if (strstr(n, "List`1") || strstr(n, "List<")) {
        int32_t sz = Read<int32_t>(obj, kListSize);
        if (sz < 0 || sz > 4096) return Container::Unknown;
        *count_out = sz;
        return Container::List;
    }
    if (strstr(n, "[]")) {
        int32_t len = ArrayLength(obj);
        if (len < 0 || len > 4096) return Container::Unknown;
        *count_out = len;
        return Container::Array;
    }
    return Container::Unknown;
}

void* ContainerAt(void* obj, Container kind, int i) {
    if (kind == Container::List) {
        void* items = Read<void*>(obj, kListItems);
        if (!PtrOk(reinterpret_cast<uintptr_t>(items))) return nullptr;
        return Read<void*>(ArrayData(items), static_cast<size_t>(i) * 8);
    }
    if (kind == Container::Array) {
        return Read<void*>(ArrayData(obj), static_cast<size_t>(i) * 8);
    }
    return nullptr;
}

// The local player's camp, from sources the game maintains itself. Getting this
// wrong inverts every colour and makes maphack write onto our own team, so it is
// derived from the manager's own "local player" references rather than guessed
// from m_bSameCampType, which is not dependable in a real 5v5.
bool DeriveLocalCamp(void* list, Container kind, int count) {
    Entity scratch;
    // 0) Logic layer — server truth. LogicBattleManager.m_SelfCampType is set
    // authoritatively by the game; prefer it over every Show-side guess. But it
    // must agree with a live entity's camp first: garbage reads (pointer
    // fragments in the 0x7638 range were observed live, from the field address
    // holding remnants) match nothing, and latching one would mark every entity
    // — including the local player — as an enemy.
    if (g_f_logic_instance.ok() && g_f_logic_self_camp.ok()) {
        uintptr_t lmgr = ReadStaticPtr(g_f_logic_instance.info);
        if (PtrOk(lmgr)) {
            const int c = ReadI(reinterpret_cast<void*>(lmgr), g_f_logic_self_camp);
            bool observed = false;
            for (int i = 0; i < count && !observed; i++) {
                void* e = ContainerAt(list, kind, i);
                if (!PtrOk(reinterpret_cast<uintptr_t>(e))) continue;
                alignas(16) uint8_t buf[kEntityPeek];
                if (!PeekEntity(e, buf)) continue;
                if (Fill(&scratch, e, buf) && scratch.camp == c) observed = true;
            }
            if (c > 0 && observed) {
                g_local_camp = c;
                g_local_camp_strong = true;
                LOGI("game: local camp = %d (LogicBattleManager.m_SelfCampType)", c);
                return true;
            }
            if (c > 0) {
                LOGI("game: logic camp %d rejected (matches no live entity)", c);
            }
        }
    }

    uintptr_t mgr = ReadStaticPtr(g_f_mgr_instance.info);
    if (!PtrOk(mgr)) return false;
    void* m = reinterpret_cast<void*>(mgr);

    // 1) BattleManager.m_LocalPlayerShow -> our own ShowEntity
    if (g_f_local_player.ok()) {
        void* lp = Read<void*>(m, g_f_local_player.offset);
        if (PtrOk(reinterpret_cast<uintptr_t>(lp))) {
            alignas(16) uint8_t lpb[kEntityPeek];
            if (PeekEntity(lp, lpb)) {
                const int c = ReadI(lpb, F.camp);
                if (c > 0 && c < 17) {
                    g_local_camp = c;
                    g_local_camp_strong = true;
                    LOGI("game: local camp = %d (from m_LocalPlayerShow)", c);
                    return true;
                }
            }
        }
    }

    // 2) BattleManager.m_LocalShowPlayers -> any member of our own team
    if (g_f_local_players.ok()) {
        void* team = Read<void*>(m, g_f_local_players.offset);
        int tc = 0;
        const Container tk = ClassifyContainer(team, &tc);
        if (tk != Container::Unknown && tc > 0) {
            void* first = ContainerAt(team, tk, 0);
            if (PtrOk(reinterpret_cast<uintptr_t>(first))) {
                alignas(16) uint8_t fb[kEntityPeek];
                if (PeekEntity(first, fb)) {
                    const int c = ReadI(fb, F.camp);
                    if (c > 0 && c < 17) {
                        g_local_camp = c;
                        g_local_camp_strong = true;
                        LOGI("game: local camp = %d (from m_LocalShowPlayers)", c);
                        return true;
                    }
                }
            }
        }
    }

    // 3) The camera follows the local player, so its tracked id identifies us.
    if (g_f_cam_instance.ok() && g_f_cam_player_id.ok()) {
        uintptr_t cam = ReadStaticPtr(g_f_cam_instance.info);
        if (PtrOk(cam)) {
            const int32_t cid = Read<int32_t>(reinterpret_cast<void*>(cam),
                                              g_f_cam_player_id.offset);
            if (cid > 0) {
                for (int i = 0; i < count; i++) {
                    void* e = ContainerAt(list, kind, i);
                    if (!PtrOk(reinterpret_cast<uintptr_t>(e))) continue;
                    alignas(16) uint8_t buf[kEntityPeek];
                    if (!PeekEntity(e, buf)) continue;
                    if (!Fill(&scratch, e, buf)) continue;
                    // guid is unsigned; compare in the same width so ids above
                    // INT32_MAX still match
                    if (static_cast<uint32_t>(cid) == scratch.guid) {
                        const int c = scratch.camp;
                        if (c > 0) {
                            g_local_camp = c;
                            g_local_camp_strong = true;
                            LOGI("game: local camp = %d (camera id %d)", c, cid);
                            return true;
                        }
                    }
                }
            }
        }
    }

    // No fallback to m_bSameCampType: it is not a "same team as me" indicator.
    // Observed disagreeing with m_EntityCampType (camp=1, same=0 on one unit),
    // it most likely means "same camp as this unit's owner", which is true for
    // summons and most units. Trusting it inverted every colour and pointed
    // maphack at our own team. Better to report no camp than a wrong one.
    return false;
}

// Re-check the latched camp against the manager's live local-player reference.
//
// The manager-pointer change check in SnapshotPlayers is not sufficient on its
// own: IL2CPP reuses freed memory, so the second match's BattleManager often
// lands at the same address as the first's. When that happens the pointer
// comparison sees "same manager", skips ResetMatchState, and the camp latched
// in match 1 survives into match 2. If the player is on the other team the
// second time, every colour inverts and maphack writes onto their own side —
// the "enemy ESP shows my team" bug.
//
// The *value* of m_LocalPlayerShow does change between matches even when the
// manager's address does not, so the local entity is the reliable signal.
//
// A single differing read is not enough to act on: during match teardown the
// manager can briefly point at a stale entity, and latching that would be the
// same bug in the other direction. So a change must be observed on several
// consecutive frames before it is accepted — a real team change persists, a
// teardown artifact does not. The counter resets the moment the read agrees
// with the latch again.
bool RevalidateLocalCamp() {
    if (!g_f_local_player.ok()) return false;
    uintptr_t mgr = ReadStaticPtr(g_f_mgr_instance.info);
    if (!PtrOk(mgr)) return false;

    void* lp = Read<void*>(reinterpret_cast<void*>(mgr), g_f_local_player.offset);
    if (!PtrOk(reinterpret_cast<uintptr_t>(lp))) return false;

    alignas(16) uint8_t buf[kEntityPeek];
    if (!PeekEntity(lp, buf)) return false;

    // Full parse, not just the camp field: Fill's sanity checks (camp in
    // range, HP in a plausible range) reject a freed-but-mapped object, which
    // a raw field read would happily accept.
    Entity scratch;
    if (!Fill(&scratch, lp, buf)) return false;
    const int c = scratch.camp;
    if (c <= 0 || c > 16) return false;

    if (c == g_local_camp) {
        g_camp_change_frames = 0;   // agrees: no pending change
        return false;
    }

    // A differing camp. Require it to persist before trusting it.
    constexpr int kConfirmFrames = 5;
    if (++g_camp_change_frames < kConfirmFrames) return false;

    LOGI("game: local camp changed %d -> %d (m_LocalPlayerShow, stable %d frames)",
         g_local_camp, c, g_camp_change_frames);
    g_local_camp = c;
    g_local_camp_strong = true;
    g_camp_change_frames = 0;
    return true;
}

// Read through process_vm_readv: fails with EFAULT on unmapped/freed pages
// instead of taking the SIGSEGV that a plain dereference would.
bool SafePeek(const void* addr, size_t len, void* out) {
    iovec local{out, len};
    iovec remote{const_cast<void*>(addr), len};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(len);
}

// Peek an entity's bytes into a local buffer. False when the object is
// unmapped or freed (EFAULT) — callers skip the entity entirely.
bool PeekEntity(void* o, void* buf) {
    if (!o || !PtrOk(reinterpret_cast<uintptr_t>(o))) return false;
    return SafePeek(o, kEntityPeek, buf);
}

// Parse one entity from a peeked copy of the LIVE object o. Returns false
// when the fields look like garbage (a freed-but-mapped object) — camp reads
// as pointer fragments (30264/30263 were observed live) and HP leaves a sane
// range on those.
//
// The peeked copy is only a read boundary for the crash: the parsed Entity
// keeps obj = the live pointer, so pointer fields (name strings) and feature
// writes still operate on the live object.
bool Fill(Entity* e, void* o, const void* img) {
    e->obj = o;
    e->guid = static_cast<uint32_t>(ReadI(img, F.guid));
    e->id = ReadI(img, F.id);
    e->type = ReadI(img, F.type);
    e->level = ReadI(img, F.level);
    e->camp = ReadI(img, F.camp);
    e->same_camp = ReadB(img, F.same_camp);
    e->is_soldier = ReadB(img, F.is_soldier) || ReadB(img, F.is_special_soldier);
    e->is_fake = ReadB(img, F.is_fake);
    e->radius = ReadF(img, F.radius);
    e->skin = ReadI(img, F.skin);
    e->minimap_visible = ReadB(img, F.minimap_visible);
    e->blood_visible = ReadB(img, F.blood_visible);
    e->grass_id = ReadI(img, F.grass_id);
    e->dead_sight = ReadI(img, F.dead_sight);
    e->is_dead = ReadB(img, F.dead_anima);
    e->is_hero = ReadB(img, F.is_cache_hero) || !e->is_soldier;

    // Camp is a small enum; pointer-fragment garbage reads far outside it.
    if (e->camp < 0 || e->camp > 16) return false;

    // HP is stored as an integer in MLBB. Reading an int through a float
    // reinterpretation yields a denormal that prints as 0, so accept whichever
    // interpretation lands in a sane range.
    const int32_t hp_i = F.hp.ok() ? Read<int32_t>(const_cast<void*>(img), F.hp.offset) : 0;
    const int32_t hpm_i = F.hp_max.ok() ? Read<int32_t>(const_cast<void*>(img), F.hp_max.offset) : 0;
    if (hp_i > 0 && hp_i < 10000000 && hpm_i > 0 && hpm_i < 10000000) {
        e->hp = static_cast<float>(hp_i);
        e->hp_max = static_cast<float>(hpm_i);
    } else {
        e->hp = ReadF(img, F.hp);
        e->hp_max = ReadF(img, F.hp_max);
        if (e->hp < 0.0f || e->hp > 1e9f || e->hp_max < 0.0f || e->hp_max > 1e9f)
            return false;
    }

    // Position: prefer the Unity-space cached value, which is what the renderer
    // positions the model from. The logic-space cache can lag the sprite while
    // moving, which reads as the box trailing the character. The chosen space
    // travels with the entity: a logic-space position must not be projected
    // through the renderer's matrices without the map's 45-degree turn.
    float ux = 0, uy = 0, uz = 0, cx = 0, cy = 0, cz = 0, ox = 0, oy = 0, oz = 0;
    ReadVec3(img, F.pos_unity, &ux, &uy, &uz);
    ReadVec3(img, F.pos_cache, &cx, &cy, &cz);
    ReadVec3(img, F.pos_obj, &ox, &oy, &oz);

    if (ux != 0 || uy != 0 || uz != 0) {
        e->x = ux; e->y = uy; e->z = uz; e->pos_is_unity = true;
    } else if (cx != 0 || cy != 0 || cz != 0) {
        e->x = cx; e->y = cy; e->z = cz; e->pos_is_unity = false;
    } else {
        e->x = ox; e->y = oy; e->z = oz; e->pos_is_unity = false;
    }

    // Combat extras that live on ShowEntity itself.
    e->is_frozen = ReadB(img, F.is_frozen);
    e->atk_range = ReadF(img, F.atk_range);

    // KDA / economy live on the ShowPlayer companion. The object is only
    // readable as a ShowPlayer when it actually is one, so the class pointer
    // (read from the peeked copy — never the live object) is checked first: a
    // plain ShowEntity read at these offsets would be garbage.
    if (IsShowPlayerObject(img)) {
        e->kills = ReadI(img, PF.kills);
        e->deaths = ReadI(img, PF.deaths);
        e->assists = ReadI(img, PF.assists);
        e->gold = ReadI(img, PF.gold);
        e->total_gold = ReadI(img, PF.total_gold);
        e->bounty = ReadI(img, PF.bounty);
    }

    static int plog = 0;
    if (plog++ < 8) {
        LOGI("game: pos unity=(%.1f,%.1f,%.1f) cache=(%.1f,%.1f,%.1f) obj=(%.1f,%.1f,%.1f)",
             ux, uy, uz, cx, cy, cz, ox, oy, oz);
    }
    return true;
}

// ---- matrix math for world->screen ----
struct Mat4 {
    float m[16];
};

// Unity stores matrices column-major: m[col*4 + row].
inline float Mul(const Mat4& a, int r, int c) { return a.m[c * 4 + r]; }

// Unity euler angles (degrees) use the ZXY order.
void RotFromEuler(float xd, float yd, float zd, float R[3][3]) {
    const float x = xd * 3.14159265358979f / 180.0f;
    const float y = yd * 3.14159265358979f / 180.0f;
    const float z = zd * 3.14159265358979f / 180.0f;
    const float cx = cosf(x), sx = sinf(x);
    const float cy = cosf(y), sy = sinf(y);
    const float cz = cosf(z), sz = sinf(z);
    R[0][0] = cy * cz + sy * sx * sz;  R[0][1] = cx * sz;  R[0][2] = -sy * cz + cy * sx * sz;
    R[1][0] = -cy * sz + sy * sx * cz; R[1][1] = cx * cz;  R[1][2] = sy * sz + cy * sx * cz;
    R[2][0] = sy * cx;                 R[2][1] = -sx;     R[2][2] = cy * cx;
}

// Build a view-projection matrix from the game's own camera fields. This avoids
// Unity's managed Camera entirely: Camera.get_main asserts on a non-main thread
// and returns null from the render thread, whereas ShowBattleCamera exposes
// position / rotation / FOV as plain fields we can read directly.
bool BuildVpFromGameCamera(Mat4* vp_out, bool log) {
    if (!g_f_cam_instance.ok()) return false;
    uintptr_t cam = ReadStaticPtr(g_f_cam_instance.info);
    if (!PtrOk(cam)) return false;
    void* c = reinterpret_cast<void*>(cam);

    float px = 0, py = 0, pz = 0;
    float rx = 0, ry = 0, rz = 0;
    int fov = 0;
    ReadVec3(c, g_f_cam_pos, &px, &py, &pz);
    ReadVec3(c, g_f_cam_rot_vect, &rx, &ry, &rz);
    if (g_f_cam_fov.ok()) fov = Read<int32_t>(c, g_f_cam_fov.offset);

    if (log) {
        LOGI("game: camgame pos=(%.1f,%.1f,%.1f) rot=(%.1f,%.1f,%.1f) fov=%d",
             px, py, pz, rx, ry, rz, fov);
    }
    if (fov <= 0 || fov > 170) fov = 45;

    // View: inverse of the camera transform = [R^T | -R^T * eye]
    float R[3][3];
    RotFromEuler(rx, ry, rz, R);
    Mat4 view{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            view.m[j * 4 + i] = R[j][i];  // (R^T)[i][j] = R[j][i], column-major
    view.m[3] = -(R[0][0] * px + R[1][0] * py + R[2][0] * pz);
    view.m[7] = -(R[0][1] * px + R[1][1] * py + R[2][1] * pz);
    view.m[11] = -(R[0][2] * px + R[1][2] * py + R[2][2] * pz);
    view.m[15] = 1.0f;

    // Perspective projection, GL convention (camera looks down -Z in view space)
    const float aspect = (g_sh > 0) ? static_cast<float>(g_sw) / static_cast<float>(g_sh) : 1.78f;
    const float f = 1.0f / tanf(fov * 0.5f * 3.14159265358979f / 180.0f);
    const float zn = 0.3f, zf = 2000.0f;
    Mat4 proj{};
    proj.m[0] = f / aspect;
    proj.m[5] = f;
    proj.m[10] = (zf + zn) / (zn - zf);
    proj.m[11] = -1.0f;
    proj.m[14] = (2.0f * zf * zn) / (zn - zf);
    proj.m[15] = 0.0f;

    for (int cix = 0; cix < 4; cix++)
        for (int r = 0; r < 4; r++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += Mul(proj, r, k) * Mul(view, k, cix);
            vp_out->m[cix * 4 + r] = s;
        }
    return true;
}

// Unity's managed Camera matrix getters. il2cpp_runtime_invoke boxes value-type
// results, so the 16 floats sit after the 16-byte object header. That is the
// only reading used: an earlier version also tried the raw pointer as a
// fallback, but the object header (two pointers) is non-zero and finite, so it
// passed a loose sanity check and produced a garbage matrix.
bool InvokeMatrix(const MethodInfo* m, void* obj, Mat4* out) {
    if (!m || !api.runtime_invoke) return false;
    Il2CppObject* exc = nullptr;
    void* r = api.runtime_invoke(m, obj, nullptr, &exc);
    if (!r || exc) return false;

    // The boxed return's header is two pointers; the Matrix4x4 payload
    // follows. kObjHeader is the arch-correct header size.
    const float* f = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(r) + kObjHeader);
    for (int i = 0; i < 16; i++) {
        if (!(f[i] == f[i]) || f[i] > 1e12f || f[i] < -1e12f) return false;
    }
    memcpy(out->m, f, sizeof(float) * 16);
    return true;
}

bool ProjectPoint(const Mat4& vp, float wx, float wy, float wz,
                  int sw, int sh, float* sx, float* sy) {
    const float cx = Mul(vp, 0, 0) * wx + Mul(vp, 0, 1) * wy + Mul(vp, 0, 2) * wz + Mul(vp, 0, 3);
    const float cy = Mul(vp, 1, 0) * wx + Mul(vp, 1, 1) * wy + Mul(vp, 1, 2) * wz + Mul(vp, 1, 3);
    const float cw = Mul(vp, 3, 0) * wx + Mul(vp, 3, 1) * wy + Mul(vp, 3, 2) * wz + Mul(vp, 3, 3);
    if (cw <= 0.0001f) return false;  // behind camera
    const float ndc_x = cx / cw;
    const float ndc_y = cy / cw;
    *sx = (ndc_x * 0.5f + 0.5f) * static_cast<float>(sw);
    *sy = (0.5f - ndc_y * 0.5f) * static_cast<float>(sh);
    return true;
}

// Read the main camera's world position through the managed Transform API.
bool UnityCamPos(float* x, float* y, float* z) {
    if (!g_m_get_transform || !g_m_tf_getpos || !g_m_cam_main || !api.runtime_invoke)
        return false;
    Il2CppObject* exc = nullptr;
    void* camobj = api.runtime_invoke(g_m_cam_main, nullptr, nullptr, &exc);
    if (!camobj || exc) return false;
    void* tf = api.runtime_invoke(g_m_get_transform, camobj, nullptr, &exc);
    if (!tf || exc) return false;
    void* r = api.runtime_invoke(g_m_tf_getpos, tf, nullptr, &exc);
    if (!r || exc) return false;
    // Boxed Vector3: payload follows the object header.
    const float* v = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(r) + kObjHeader);
    if (!(v[0] == v[0]) || !(v[1] == v[1]) || !(v[2] == v[2])) return false;
    *x = v[0]; *y = v[1]; *z = v[2];
    return true;
}

// Write the main camera's world position: Transform.set_position(Vector3).
bool UnityCamSetPos(float x, float y, float z) {
    if (!g_m_get_transform || !g_m_tf_setpos || !g_m_cam_main || !api.runtime_invoke)
        return false;
    Il2CppObject* exc = nullptr;
    void* camobj = api.runtime_invoke(g_m_cam_main, nullptr, nullptr, &exc);
    if (!camobj || exc) return false;
    void* tf = api.runtime_invoke(g_m_get_transform, camobj, nullptr, &exc);
    if (!tf || exc) return false;
    float vec[3] = {x, y, z};
    void* params[1] = {vec};
    api.runtime_invoke(g_m_tf_setpos, tf, params, &exc);
    return exc == nullptr;
}

// Cheap deterministic hero-name table. IDs are the game's own m_ID values.
//
// The mapping is heroId = releaseIndex + 6, recovered from VYNIX's embedded
// roster: it carries 125 hero names in exact release order (Tigreal first,
// Obsidia last), and two of them are live-verified against m_ID — Zilong sits
// at release index 10 and reports 16, Kimmy at 65 and reports 71. Both land on
// +6, so the whole table follows from the order.
//
// The previous table used a 1001+ id space that m_ID does not use, which is why
// the nametag fell back to "Hero <id>" whenever the live m_RoleName string was
// empty. These ids are the ones the game actually reports.
struct HeroName { int id; const char* name; };
const HeroName kHeroes[] = {
    {   6, "Tigreal"},
    {   7, "Alucard"},
    {   8, "Karina"},
    {   9, "Akai"},
    {  10, "Franco"},
    {  11, "Bane"},
    {  12, "Bruno"},
    {  13, "Clint"},
    {  14, "Rafaela"},
    {  15, "Eudora"},
    {  16, "Zilong"},
    {  17, "Fanny"},
    {  18, "Layla"},
    {  19, "Minotaur"},
    {  20, "Lolita"},
    {  21, "Hayabusa"},
    {  22, "Freya"},
    {  23, "Gord"},
    {  24, "Natalia"},
    {  25, "Kagura"},
    {  26, "Chou"},
    {  27, "Sun"},
    {  28, "Alpha"},
    {  29, "Ruby"},
    {  30, "Yi Sun-shin"},
    {  31, "Moskov"},
    {  32, "Johnson"},
    {  33, "Cyclops"},
    {  34, "Estes"},
    {  35, "Hilda"},
    {  36, "Aurora"},
    {  37, "Lapu-Lapu"},
    {  38, "Vexana"},
    {  39, "Roger"},
    {  40, "Karrie"},
    {  41, "Gatotkaca"},
    {  42, "Harley"},
    {  43, "Irithel"},
    {  44, "Grock"},
    {  45, "Argus"},
    {  46, "Odette"},
    {  47, "Lancelot"},
    {  48, "Diggie"},
    {  49, "Hylos"},
    {  50, "Zhask"},
    {  51, "Helcurt"},
    {  52, "Pharsa"},
    {  53, "Lesley"},
    {  54, "Jawhead"},
    {  55, "Angela"},
    {  56, "Gusion"},
    {  57, "Valir"},
    {  58, "Martis"},
    {  59, "Uranus"},
    {  60, "Hanabi"},
    {  61, "Chang'e"},
    {  62, "Kaja"},
    {  63, "Selena"},
    {  64, "Aldous"},
    {  65, "Claude"},
    {  66, "Vale"},
    {  67, "Leomord"},
    {  68, "Lunox"},
    {  69, "Hanzo"},
    {  70, "Belerick"},
    {  71, "Kimmy"},
    {  72, "Thamuz"},
    {  73, "Harith"},
    {  74, "Minsitthar"},
    {  75, "Kadita"},
    {  76, "Faramis"},
    {  77, "Badang"},
    {  78, "Khufra"},
    {  79, "Granger"},
    {  80, "Guinevere"},
    {  81, "Esmeralda"},
    {  82, "Terizla"},
    {  83, "X.Borg"},
    {  84, "Ling"},
    {  85, "Dyrroth"},
    {  86, "Lylia"},
    {  87, "Baxia"},
    {  88, "Masha"},
    {  89, "Wanwan"},
    {  90, "Silvanna"},
    {  91, "Cecilion"},
    {  92, "Carmilla"},
    {  93, "Atlas"},
    {  94, "Popol & Kupa"},
    {  95, "Yu Zhong"},
    {  96, "Luo Yi"},
    {  97, "Benedetta"},
    {  98, "Khaleed"},
    {  99, "Barats"},
    { 100, "Brody"},
    { 101, "Yve"},
    { 102, "Mathilda"},
    { 103, "Paquito"},
    { 104, "Gloo"},
    { 105, "Beatrix"},
    { 106, "Phoveus"},
    { 107, "Natan"},
    { 108, "Aulus"},
    { 109, "Aamon"},
    { 110, "Valentina"},
    { 111, "Edith"},
    { 112, "Floryn"},
    { 113, "Yin"},
    { 114, "Melissa"},
    { 115, "Xavier"},
    { 116, "Julian"},
    { 117, "Fredrinn"},
    { 118, "Joy"},
    { 119, "Novaria"},
    { 120, "Arlott"},
    { 121, "Ixia"},
    { 122, "Nolan"},
    { 123, "Cici"},
    { 124, "Chip"},
    { 125, "Zhuxin"},
    { 126, "Suyou"},
    { 127, "Lukas"},
    { 128, "Kalea"},
    { 129, "Zetian"},
    { 130, "Obsidia"},
};

}  // namespace

void SetScreenSize(int w, int h) { g_sw = w; g_sh = h; }
int ScreenWidth() { return g_sw; }
int ScreenHeight() { return g_sh; }

bool Init() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!ResolveShowEntity()) return false;
    if (!ResolveManagers()) return false;
    ResolveUnityCamera();
    return true;
}

bool Ready() {
    return g_show_entity && g_battle_mgr && g_f_mgr_instance.ok() && g_f_players.ok();
}

int LocalCamp() { return g_local_camp; }
// Logic-layer camp when available (authoritative); -1 otherwise.
int LogicCamp() {
    if (!g_f_logic_instance.ok() || !g_f_logic_self_camp.ok()) return -1;
    uintptr_t lmgr = ReadStaticPtr(g_f_logic_instance.info);
    if (!PtrOk(lmgr)) return -1;
    const int c = ReadI(reinterpret_cast<void*>(lmgr), g_f_logic_self_camp);
    return c > 0 ? c : -1;
}

int SnapshotPlayers(Entity* out, int max) {
    if (!Ready() || !out || max <= 0) return 0;
    uintptr_t mgr = ReadStaticPtr(g_f_mgr_instance.info);
    if (!PtrOk(mgr)) return 0;

    // New match detection. Two independent signals, because the manager
    // pointer alone can be reused by the allocator between matches:
    //   - the manager pointer changing, and
    //   - the local player's guid changing (checked after the local entity is
    //     resolved, below).
    // Either invalidates every match-scoped cache (camp, maphack target set).
    if (g_mgr_seen != 0 && g_mgr_seen != mgr) ResetMatchState("new BattleManager.Instance");
    g_mgr_seen = mgr;

    void* list = Read<void*>(reinterpret_cast<void*>(mgr), g_f_players.offset);
    int count = 0;
    Container kind = ClassifyContainer(list, &count);
    if (kind == Container::Unknown) return 0;

    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        void* e = ContainerAt(list, kind, i);
        if (!PtrOk(reinterpret_cast<uintptr_t>(e))) continue;
        alignas(16) uint8_t buf[kEntityPeek];
        if (!PeekEntity(e, buf)) continue;
        if (!Fill(&out[n], e, buf)) continue;
        n++;
    }

    // Second match-boundary signal: the local player's guid. The manager
    // pointer can be reused between matches, but a fresh match gives the
    // player a new entity guid, so a changed guid is a definitive boundary
    // even when the pointer is identical. Read the manager's own local-player
    // reference, not the list (the list may not contain us yet on the first
    // frame of a match).
    if (g_f_local_player.ok()) {
        void* lp = Read<void*>(reinterpret_cast<void*>(mgr), g_f_local_player.offset);
        if (PtrOk(reinterpret_cast<uintptr_t>(lp))) {
            alignas(16) uint8_t lb[kEntityPeek];
            if (PeekEntity(lp, lb)) {
                const int32_t g = ReadI(lb, F.guid);
                if (g != 0) {
                    const uint32_t gu = static_cast<uint32_t>(g);
                    if (g_local_guid_seen != 0 && g_local_guid_seen != gu) {
                        ResetMatchState("local player guid changed");
                    }
                    g_local_guid_seen = gu;
                }
            }
        }
    }

    // Camp: derive when unknown, re-validate when latched. The re-validation
    // is what actually fixes the second-match inversion — the reset above can
    // only fire when a boundary is visible, and the re-check catches the case
    // where the camp simply differs from the live local player's.
    if (!g_local_camp_strong) {
        DeriveLocalCamp(list, kind, count);
    } else {
        RevalidateLocalCamp();
    }
    return n;
}

int SnapshotMonsters(Entity* out, int max) {
    if (!Ready() || !g_f_monsters.ok() || !out || max <= 0) return 0;
    uintptr_t mgr = ReadStaticPtr(g_f_mgr_instance.info);
    if (!PtrOk(mgr)) return 0;

    void* list = Read<void*>(reinterpret_cast<void*>(mgr), g_f_monsters.offset);
    int count = 0;
    Container kind = ClassifyContainer(list, &count);

    // Diagnostic: the aura needs this list, and an empty one is silent — it
    // looks identical to "no monsters in range". Report why it is empty.
    static int wlog = 0;
    if (wlog++ % 300 == 0) {
        LOGI("game: monsters field ok=%d list=%p kind=%d count=%d",
             g_f_monsters.ok(), list, static_cast<int>(kind), count);
    }

    // m_ShowMonsters is the intended source but reads empty on this build, so
    // the dictionary the metadata also carries is used as a fallback. Without
    // it the aura can never see a jungle camp, which is the whole point of the
    // minion-clear mode.
    if (kind == Container::Unknown || count <= 0) {
        if (g_f_dic_monsters.ok()) {
            void* dict = Read<void*>(reinterpret_cast<void*>(mgr),
                                     g_f_dic_monsters.offset);
            int n = 0;
            if (dict && PtrOk(reinterpret_cast<uintptr_t>(dict))) {
                n = il2cpp::DictEach(dict, [&](int32_t /*key*/, void* val) {
                    if (n >= max) return;
                    if (!val || !PtrOk(reinterpret_cast<uintptr_t>(val))) return;
                    alignas(16) uint8_t buf[kEntityPeek];
                    if (!PeekEntity(val, buf)) return;
                    if (!Fill(&out[n], val, buf)) return;
                    n++;
                });
            }
            static int dlog = 0;
            if (dlog++ % 300 == 0)
                LOGI("game: monster dict=%p -> %d entities", dict, n);
            return n;
        }
        return 0;
    }

    int n = 0;
    static int mlog = 0;
    for (int i = 0; i < count && n < max; i++) {
        void* e = ContainerAt(list, kind, i);
        if (!PtrOk(reinterpret_cast<uintptr_t>(e))) continue;
        alignas(16) uint8_t buf[kEntityPeek];
        if (!PeekEntity(e, buf)) continue;
        if (!Fill(&out[n], e, buf)) continue;
        if (mlog++ < 12) {
            char nm[64] = {0};
            if (F.entity_name.ok()) {
                void* sp = Read<void*>(buf, F.entity_name.offset);
                if (PtrOk(reinterpret_cast<uintptr_t>(sp)))
                    ReadIl2CppString(reinterpret_cast<Il2CppString*>(sp), nm, sizeof(nm));
            }
            LOGI("game: monster[%d] id=%d type=%d soldier=%d name='%s' pos=(%.1f,%.1f,%.1f)",
                 i, out[n].id, out[n].type, out[n].is_soldier, nm, out[n].x, out[n].y, out[n].z);
        }
        n++;
    }
    return n;
}

bool SelfEntity(Entity* out) {
    if (!Ready() || !out || !g_f_local_player.ok()) return false;
    uintptr_t mgr = ReadStaticPtr(g_f_mgr_instance.info);
    if (!PtrOk(mgr)) return false;
    void* lp = Read<void*>(reinterpret_cast<void*>(mgr), g_f_local_player.offset);
    if (!PtrOk(reinterpret_cast<uintptr_t>(lp))) return false;
    alignas(16) uint8_t buf[kEntityPeek];
    if (!PeekEntity(lp, buf)) return false;
    Fill(out, lp, buf);
    return true;
}

// Set by BeginFrame(); the camera matrices are rebuilt once per frame so the
// projection tracks a moving camera instead of lagging behind it.
static bool g_vp_dirty = true;

namespace {
FrameSnapshot g_snap;
bool g_snap_valid = false;
}  // namespace

void BeginFrame() {
    g_vp_dirty = true;
    g_snap_valid = false;
}

const FrameSnapshot& Snapshot() {
    if (g_snap_valid) return g_snap;
    g_snap_valid = true;   // set first: the builders below must not recurse

    g_snap.n_players = SnapshotPlayers(g_snap.players, 256);
    g_snap.n_monsters = SnapshotMonsters(g_snap.monsters, 256);
    g_snap.has_self = SelfEntity(&g_snap.self) && g_snap.self.obj != nullptr;

    // The local camp comes from the live self entity when there is one. That
    // object was parsed this frame, so its camp field is the freshest truth
    // available and cannot be stale across a match boundary — the failure the
    // latch suffered. LocalCamp() (the derived/latched value) is the fallback
    // for the frames before the local entity exists (lobby, first load frame).
    if (g_snap.has_self && g_snap.self.camp > 0 && g_snap.self.camp < 17) {
        g_snap.local_camp = g_snap.self.camp;
        // Keep the latch in step so the fallback path agrees with what the
        // ESP is actually using.
        if (g_local_camp != g_snap.self.camp) {
            LOGI("game: local camp %d -> %d (live self entity)",
                 g_local_camp, g_snap.self.camp);
            g_local_camp = g_snap.self.camp;
            g_local_camp_strong = true;
        }
    } else {
        g_snap.local_camp = LocalCamp();
    }

    // Objective HP tracking rides the same walk. It has to run whether or not
    // anything is drawn from it, because auto-retribution reads its result.
    if (g_snap.n_monsters > 0)
        TrackObjectives(g_snap.monsters, g_snap.n_monsters);

    return g_snap;
}

namespace {

// Unity's own camera matrices — the truth for where a Unity-space point (and
// therefore a sprite drawn from it) lands on screen. Re-fetched periodically
// rather than once: the main camera is not the same object across scenes, and
// a stale pointer yields a garbage matrix.
bool UnityProject(float wx, float wy, float wz, float* sx, float* sy) {
    static void* cam = nullptr;
    static int cam_age = 1000;
    if (cam_age++ > 30 || !PtrOk(reinterpret_cast<uintptr_t>(cam))) {
        cam_age = 0;
        if (g_unity_camera && g_m_cam_main && api.runtime_invoke) {
            Il2CppObject* exc = nullptr;
            void* r = api.runtime_invoke(g_m_cam_main, nullptr, nullptr, &exc);
            if (PtrOk(reinterpret_cast<uintptr_t>(r))) cam = r;
            static int logged = 0;
            if (logged++ < 3) LOGI("game: Camera.get_main -> %p (exc=%p)", r, (void*)exc);
        }
    }

    static Mat4 vp{};
    static bool have_vp = false;
    if (g_vp_dirty) {
        g_vp_dirty = false;
        have_vp = false;
        if (PtrOk(reinterpret_cast<uintptr_t>(cam))) {
            Mat4 view{}, proj{};
            if (InvokeMatrix(g_m_world2cam, cam, &view) && InvokeMatrix(g_m_proj, cam, &proj)) {
                for (int cix = 0; cix < 4; cix++)
                    for (int r = 0; r < 4; r++) {
                        float s = 0;
                        for (int k = 0; k < 4; k++) s += Mul(proj, r, k) * Mul(view, k, cix);
                        vp.m[cix * 4 + r] = s;
                    }
                have_vp = true;
            }
        }
    }
    if (have_vp && ProjectPoint(vp, wx, wy, wz, g_sw, g_sh, sx, sy)) return true;
    return false;
}

// A matrix derived from the game camera's own fields, for when the managed
// Camera is unreachable. Retried while it has never succeeded — the first
// attempt can happen before ShowBattleCamera.Instance exists — and rebuilt
// periodically so it tracks FOV changes.
bool GameProject(float wx, float wy, float wz, float* sx, float* sy) {
    static Mat4 game_vp{};
    static bool have_game_vp = false;
    static int game_vp_age = 1000;
    if (!have_game_vp || game_vp_age++ > 60) {
        game_vp_age = 0;
        have_game_vp = BuildVpFromGameCamera(&game_vp, false);
    }
    if (have_game_vp && ProjectPoint(game_vp, wx, wy, wz, g_sw, g_sh, sx, sy)) return true;
    return false;
}

}  // namespace

bool WorldToScreenEx(float wx, float wy, float wz, bool unity_space,
                     float* sx, float* sy) {
    if (g_sw <= 0 || g_sh <= 0) return false;

    // Every observed position value on this build projects correctly through
    // the renderer's matrices as-is: the local hero at its spawn lands within
    // ~13 px of its sprite, and rotating the point (the map's 45-degree minimap
    // convention) pushed it thousands of pixels off. So both spaces go through
    // the Unity matrices directly; the unity flag only records the source.
    if (UnityProject(wx, wy, wz, sx, sy)) return true;
    return GameProject(wx, wy, wz, sx, sy);
}

bool WorldToScreen(float wx, float wy, float wz, float* sx, float* sy) {
    return WorldToScreenEx(wx, wy, wz, true, sx, sy);
}

int ApplyMaphack(bool enabled) {
    if (!Ready()) return 0;
    uintptr_t mgr = ReadStaticPtr(g_f_mgr_instance.info);
    if (!PtrOk(mgr)) return 0;

    void* list = Read<void*>(reinterpret_cast<void*>(mgr), g_f_players.offset);
    int count = 0;
    Container kind = ClassifyContainer(list, &count);
    if (kind == Container::Unknown) return 0;

    // Only the minimap and health-bar flags are written. The bush reveal
    // (visibleOutsideGrassInRange / ignoreVisibleOutsideGrassInRange) and the
    // renderer flag are NOT written: both are read by engine paths that assume
    // the game set them, and forcing them freezes and then crashes the client.
    const bool want_minimap = g_cfg.map_minimap;
    const bool want_health = g_cfg.map_health;

    int touched = 0;
    for (int i = 0; i < count; i++) {
        void* e = ContainerAt(list, kind, i);
        if (!PtrOk(reinterpret_cast<uintptr_t>(e))) continue;

        // Read through a peeked copy (never dies if the object was freed),
        // but only write to the live object — and only after the copy
        // validates, so garbage reads never turn into garbage writes.
        alignas(16) uint8_t buf[kEntityPeek];
        if (!PeekEntity(e, buf)) continue;

        // Team membership is decided by camp alone. m_bSameCampType is not a
        // friend/foe flag, so using it here would skip enemies and leave the
        // reveal writing to our own side.
        const int camp = ReadI(buf, F.camp);
        if (g_local_camp <= 0 || camp == g_local_camp) continue;
        if (camp < 0 || camp > 16) continue;

        // A dead entity may already have torn-down presentation objects.
        if (ReadB(buf, F.dead_anima)) continue;

        // The game recomputes these flags every frame from its own vision logic,
        // so they have to be rewritten every frame or the reveal is simply
        // overwritten. Cheap: a handful of entities, one byte each.
        auto set_flag = [&](const Field& f, bool on) {
            if (!f.ok()) return false;
            Write<uint8_t>(e, f.offset, on ? 1 : 0);
            return true;
        };

        if (enabled) {
            if (want_minimap && set_flag(F.minimap_visible, true)) touched++;
            if (want_health) set_flag(F.blood_visible, true);
        }
    }
    return touched;
}

const char* HeroName(int hero_id) {
    for (const auto& h : kHeroes)
        if (h.id == hero_id) return h.name;
    return nullptr;
}

// ---- drone view (VYNIX model) ----
//
// The camera controller derives the camera from its own parameters whenever it
// acts, so writing the camera's Transform — what this used to do — is a race
// the controller wins on every follow tick, drag or camera event. That race is
// the flicker: the lift lands, the controller overwrites it, the next frame
// lifts again, and the view oscillates between stock and raised.
//
// Two coordinated changes instead:
//
//   FOV      Camera.fieldOfView = stock + drone_fov. A pure zoom-out. It has no
//            interaction with the controller whatsoever, and it is what makes
//            the view actually read as a drone rather than a raised camera.
//   OFFSET   SmoothFollow.offSetPos — the *parameter* the controller reads to
//            place the camera. Writing the input rather than the output means
//            the controller carries the change itself and nothing fights.
//
// Both stock values are captured when the drone engages and restored when it
// disengages, including m_iYMax — the previous version drove that to 9999 and
// never put it back, leaving the camera drag clamp disabled for the rest of the
// process.
//
// Fallback: if SmoothFollow cannot be resolved (name moved, no live instance),
// the legacy Transform lift still runs, so the drone degrades rather than
// disappearing.

struct DroneState {
    bool engaged = false;          // stock values captured
    float fov_base = 0.0f;
    bool fov_have = false;
    float off_base[3] = {0, 0, 0};
    bool off_have = false;
    int ymax_base = 0;
    bool ymax_have = false;
    float lift_y = 0.0f;           // legacy fallback read-back guard
    bool lift_have = false;
};
DroneState g_drone;

// SmoothFollow, resolved lazily. offSetPos is an auto-property, so the backing
// field is preferred: a plain write costs no managed call and no boxing, and it
// cannot run a property body with side effects.
Il2CppClass* g_sf_class = nullptr;
Field g_f_sf_off;
uintptr_t g_sf_inst = 0;
bool g_sf_class_done = false;      // class/field looked up (successfully or not)

const MethodInfo* g_m_cam_get_fov = nullptr;
const MethodInfo* g_m_cam_set_fov = nullptr;

// Unity camera object, via the main-camera accessor the projection already
// relies on.
bool UnityCamObj(void** out) {
    if (!g_m_cam_main || !api.runtime_invoke) return false;
    Il2CppObject* exc = nullptr;
    void* c = api.runtime_invoke(g_m_cam_main, nullptr, nullptr, &exc);
    if (!c || exc || !PtrOk(reinterpret_cast<uintptr_t>(c))) return false;
    *out = c;
    return true;
}

bool UnityCamGetFov(float* out) {
    if (!g_m_cam_get_fov) return false;
    void* c = nullptr;
    if (!UnityCamObj(&c)) return false;
    Il2CppObject* exc = nullptr;
    void* r = api.runtime_invoke(g_m_cam_get_fov, c, nullptr, &exc);
    if (!r || exc) return false;
    const float v = *reinterpret_cast<const float*>(
        reinterpret_cast<uintptr_t>(r) + kObjHeader);
    if (!(v == v) || v < 1.0f || v > 179.0f) return false;
    *out = v;
    return true;
}

bool UnityCamSetFov(float v) {
    if (!g_m_cam_set_fov) return false;
    void* c = nullptr;
    if (!UnityCamObj(&c)) return false;
    float arg = v;
    void* params[1] = {&arg};
    Il2CppObject* exc = nullptr;
    api.runtime_invoke(g_m_cam_set_fov, c, params, &exc);
    return exc == nullptr;
}

// offSetPos is a managed Vector3 stored inline: three floats at the field
// offset. Read through a peek so a torn-down instance cannot fault the render
// thread; write straight to the live object, which the game owns.
bool SmoothFollowOffset(float* x, float* y, float* z) {
    if (!g_sf_inst || !g_f_sf_off.ok()) return false;
    float v[3];
    if (!SafePeek(reinterpret_cast<void*>(g_sf_inst + g_f_sf_off.offset),
                  sizeof(v), v)) return false;
    if (!(v[0] == v[0]) || !(v[1] == v[1]) || !(v[2] == v[2])) return false;
    *x = v[0]; *y = v[1]; *z = v[2];
    return true;
}

void SmoothFollowSetOffset(float x, float y, float z) {
    if (!g_sf_inst || !g_f_sf_off.ok()) return;
    void* base = reinterpret_cast<void*>(g_sf_inst);
    Write<float>(base, g_f_sf_off.offset + 0, x);
    Write<float>(base, g_f_sf_off.offset + 4, y);
    Write<float>(base, g_f_sf_off.offset + 8, z);
}

// ---------------------------------------------------------------------------
// Drone hook — the VYNIX model
// ---------------------------------------------------------------------------
//
// The camera controller *writes* SmoothFollow.offSetPos every time it acts, and
// it derives the camera from that value. Writing the field from here is a race
// the controller wins on every follow tick, drag or camera event — that race is
// the flicker.
//
// VYNIX does not race it: it hooks the setter and modifies the value on its way
// in. The controller then reads *our* modified offset and carries the change
// itself, so nothing fights and nothing flickers. This is that hook.

// True when `p` falls inside an executable mapping of this process. Used to
// validate a methodPointer before hooking it — a wrong offset would otherwise
// hand GlossHook a data address and crash on the first patch.
bool IsExecutablePtr(uintptr_t p) {
    if (!p) return false;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512];
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        uintptr_t s = 0, e = 0;
        char perms[8] = {0};
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %7s", &s, &e, perms) != 3) continue;
        if (p >= s && p < e) {
            ok = (perms[2] == 'x');
            break;
        }
    }
    fclose(f);
    return ok;
}

// IL2CPP MethodInfo's first member is the code pointer. Unity 2019.4 (IL2CPP
// 24.x) puts methodPointer at 0; some revisions insert a union member first, so
// 8 is tried as a fallback. Both are validated against the maps before use.
uintptr_t MethodCodePtr(const MethodInfo* m) {
    if (!m) return 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(m);
    for (size_t off : {0x0, 0x8}) {
        uintptr_t p = 0;
        if (!SafePeek(reinterpret_cast<const void*>(base + off), sizeof(p), &p)) continue;
        if (IsExecutablePtr(p)) return p;
    }
    return 0;
}

// The detour. Signature matches the managed setter: this, the Vector3 being
// written (three floats in s0-s2 as an HFA on AArch64), and the MethodInfo
// IL2CPP passes last.
//
// It must stay minimal: no allocation, no IL2CPP calls, no logging on the hot
// path. It only adds the drone delta to the value the game is already writing
// and forwards.
typedef void (*OffSetPosFn)(void* self, float x, float y, float z,
                            const MethodInfo* method);
OffSetPosFn g_orig_offset = nullptr;
bool g_drone_hook_ready = false;

// Set by ApplyDrone each frame: the height delta to add while engaged, 0 when
// off. Read here on the game's camera thread.
volatile float g_drone_hook_height = 0.0f;

void DroneOffsetDetour(void* self, float x, float y, float z,
                       const MethodInfo* method) {
    const float h = g_drone_hook_height;
    if (h > 0.01f) {
        // VYNIX's per-axis shape: pull back along the sign of the existing
        // lateral offset and drop the eye height, which frames the hero from
        // above and behind.
        x += ((x >= 0.0f) ? 1.0f : -1.0f) * 0.234f * h;
        y -= 0.307f * h;
        z += ((z >= 0.0f) ? 1.0f : -1.0f) * 0.235f * h;
    }
    if (g_orig_offset) g_orig_offset(self, x, y, z, method);
}

// Install the hook once, when the class and setter are resolvable. Returns true
// when the detour is live.
//
// A failure that is not "the class is not loaded yet" is latched: the method
// lookup and the executable-range scan behind MethodCodePtr are not free, and
// retrying them every frame on a build where the hook simply cannot install
// starves the render thread. Once the class resolves and the setter is
// reachable, one attempt is the answer for this session.
bool g_drone_hook_failed = false;

bool InstallDroneHook() {
    if (g_drone_hook_ready) return true;
    if (g_drone_hook_failed) return false;
    if (!g_sf_class || !api.class_get_method_from_name) return false;

    const MethodInfo* setter = api.class_get_method_from_name(g_sf_class,
                                                             "set_offSetPos", 1);
    if (!setter) {
        // The class is bound but the setter is not — a build with a renamed
        // setter. Latch: the name is not going to appear by retrying.
        LOGI("drone: set_offSetPos not found on SmoothFollow, hook disabled");
        g_drone_hook_failed = true;
        return false;
    }

    const uintptr_t code = MethodCodePtr(setter);
    if (!code) {
        LOGI("drone: set_offSetPos found but no executable code pointer");
        g_drone_hook_failed = true;
        return false;
    }

    GHook h = GlossHook(reinterpret_cast<void*>(code),
                        reinterpret_cast<void*>(DroneOffsetDetour),
                        reinterpret_cast<void**>(&g_orig_offset));
    if (!h || !g_orig_offset) {
        LOGI("drone: set_offSetPos hook failed (code=%p)", (void*)code);
        g_drone_hook_failed = true;
        return false;
    }
    g_drone_hook_ready = true;
    LOGI("drone: set_offSetPos hooked at %p (orig=%p)", (void*)code,
         (void*)g_orig_offset);
    return true;
}

// Class, field and method lookup — once, or until it succeeds.
void ResolveDroneClasses() {
    if (g_unity_camera && !g_m_cam_get_fov) {
        g_m_cam_get_fov = api.class_get_method_from_name(g_unity_camera,
                                                         "get_fieldOfView", 0);
        g_m_cam_set_fov = api.class_get_method_from_name(g_unity_camera,
                                                         "set_fieldOfView", 1);
        if (g_m_cam_get_fov)
            LOGI("drone: Camera.fieldOfView bound");
    }
    if (g_sf_class_done) return;
    g_sf_class = FindClass("", "SmoothFollow");
    if (!g_sf_class) return;  // not loaded yet; try again later

    for (const char* cand : {"__offSetPos", "<offSetPos>k__BackingField",
                             "offSetPos", "m_offSetPos"}) {
        g_f_sf_off = GetField(g_sf_class, cand);
        if (g_f_sf_off.ok()) {
            LOGI("drone: SmoothFollow offset field = %s (+0x%zx)",
                 cand, g_f_sf_off.offset);
            break;
        }
    }
    g_sf_class_done = true;
    if (!g_f_sf_off.ok())
        LOGI("drone: SmoothFollow found, no offset field bound");
}

// Live SmoothFollow instance. FindObjectOfType is a scene scan, so it runs only
// when there is no instance or the cached one has gone stale.
bool ResolveDroneInstance() {
    if (!g_f_sf_off.ok() || !api.type_get_object || !api.class_get_type) return false;

    if (g_sf_inst) {
        float a, b, c;
        if (SmoothFollowOffset(&a, &b, &c)) return true;  // still alive
        g_sf_inst = 0;                                     // scene changed
    }

    Il2CppClass* obj_cls = FindClass("UnityEngine", "Object");
    const MethodInfo* m_find = obj_cls
        ? api.class_get_method_from_name(obj_cls, "FindObjectOfType", 1) : nullptr;
    if (!m_find) return false;

    Il2CppType* t = api.class_get_type(g_sf_class);
    Il2CppObject* type_obj = t ? api.type_get_object(t) : nullptr;
    if (!type_obj) return false;

    Il2CppObject* exc = nullptr;
    void* params[1] = {type_obj};
    void* inst = api.runtime_invoke(m_find, nullptr, params, &exc);
    if (!inst || exc || !PtrOk(reinterpret_cast<uintptr_t>(inst))) return false;

    g_sf_inst = reinterpret_cast<uintptr_t>(inst);
    LOGI("drone: SmoothFollow instance = %p", (void*)g_sf_inst);
    return true;
}

void DroneRelease() {
    if (!g_drone.engaged) return;
    g_drone_hook_height = 0.0f;   // the detour stops lifting immediately
    if (g_drone.fov_have) UnityCamSetFov(g_drone.fov_base);
    if (g_drone.off_have) {
        // Only restore if the instance is still the one we captured from.
        if (g_sf_inst) {
            SmoothFollowSetOffset(g_drone.off_base[0], g_drone.off_base[1],
                                  g_drone.off_base[2]);
        } else {
            LOGI("drone: release skipped the offset (instance gone)");
        }
    }
    if (g_drone.ymax_have && g_f_cam_instance.ok() && g_f_cam_ymax.ok()) {
        uintptr_t cam = ReadStaticPtr(g_f_cam_instance.info);
        if (PtrOk(cam)) {
            Write<int32_t>(reinterpret_cast<void*>(cam), g_f_cam_ymax.offset,
                           g_drone.ymax_base);
            LOGI("drone: m_iYMax restored to %d", g_drone.ymax_base);
        }
    }
    g_drone = DroneState{};
}

int ApplyDrone() {
    // Cheap gate first: the previous version ran three managed calls per frame
    // even with the drone switched off, allocating a boxed Vector3 each time.
    if (!g_cfg.drone_enabled ||
        (g_cfg.drone_height <= 0.01f && g_cfg.drone_fov <= 0.01f)) {
        g_drone_hook_height = 0.0f;   // the detour must never lift while off
        if (g_drone.engaged) DroneRelease();
        return 0;
    }
    if (!g_unity_camera) return 0;

    // Class/field lookup retries while unbound (SmoothFollow arrives with the
    // battle scene).
    static int resolve_age = 1000;
    if (!g_sf_class_done && resolve_age++ > 120) {
        resolve_age = 0;
        ResolveDroneClasses();
    }

    // The instance lookup is a FindObjectOfType scene scan, so it is throttled
    // hard AND only run when something actually needs an instance. Only the
    // field-write fallback needs one: the hook path works from the class's
    // MethodInfo alone, and the FOV path never touches SmoothFollow.
    // The previous code called this every frame unconditionally; in the lobby
    // there is no SmoothFollow instance to find, so every frame walked the
    // whole scene and the render thread starved — the lobby freeze.
    const bool want_instance = (g_cfg.drone_height > 0.01f) &&
                               !(g_cfg.drone_hook && g_drone_hook_ready);
    if (g_sf_inst) {
        // Resolved already: the liveness re-check inside is a peek, not a
        // scan, so it can run per frame.
        ResolveDroneInstance();
    } else if (want_instance) {
        static int scan_age = 1000;
        if (scan_age++ > 300) {   // ~5 s at 60 fps
            scan_age = 0;
            ResolveDroneInstance();
        }
    }

    // Preferred path: the VYNIX-model hook. Once the class resolves it is
    // installed once and then does the lifting on the game's own camera thread,
    // inside the controller's own write — no race, no flicker.
    const bool hook_live = g_cfg.drone_hook && InstallDroneHook();

    int touched = 0;

    if (!g_drone.engaged) {
        g_drone.engaged = true;
        g_drone.fov_have = (g_m_cam_get_fov != nullptr) &&
                           UnityCamGetFov(&g_drone.fov_base);
        g_drone.off_have = g_f_sf_off.ok() && g_sf_inst &&
                           SmoothFollowOffset(&g_drone.off_base[0],
                                              &g_drone.off_base[1],
                                              &g_drone.off_base[2]);
        if (g_f_cam_instance.ok() && g_f_cam_ymax.ok()) {
            uintptr_t cam = ReadStaticPtr(g_f_cam_instance.info);
            if (PtrOk(cam)) {
                g_drone.ymax_base = Read<int32_t>(reinterpret_cast<void*>(cam),
                                                  g_f_cam_ymax.offset);
                g_drone.ymax_have = true;
            }
        }
        LOGI("drone: engaged fov=%.1f(have=%d) offset=(%.2f,%.2f,%.2f)(have=%d) ymax=%d hook=%d",
             g_drone.fov_base, g_drone.fov_have,
             g_drone.off_base[0], g_drone.off_base[1], g_drone.off_base[2],
             g_drone.off_have, g_drone.ymax_base, hook_live);
    }


    // --- zoom-out: no controller interaction at all ---
    if (g_cfg.drone_fov > 0.01f && g_drone.fov_have) {
        if (UnityCamSetFov(g_drone.fov_base + g_cfg.drone_fov)) touched++;
    }

    if (g_cfg.drone_height > 0.01f) {
        if (hook_live) {
            // --- VYNIX path: let the detour do it ---
            // The hook adds the delta inside the controller's own write, so
            // there is nothing to race and nothing to guard against. This
            // frame's height is published for the detour to read.
            g_drone_hook_height = g_cfg.drone_height;
            touched++;
        } else if (g_drone.off_have) {
            // --- fallback: field write, read-back guarded ---
            const float bx = g_drone.off_base[0];
            const float by = g_drone.off_base[1];
            const float bz = g_drone.off_base[2];
            const float h = g_cfg.drone_height;
            // Their per-axis shape: pull back along the sign of the existing
            // lateral offset and drop the eye height, which frames the hero
            // from above and behind. Magnitudes are VYNIX's.
            const float nx = bx + ((bx >= 0.0f) ? 1.0f : -1.0f) * 0.234f * h;
            const float ny = by - 0.307f * h;
            const float nz = bz + ((bz >= 0.0f) ? 1.0f : -1.0f) * 0.235f * h;
            float cx = 0, cy = 0, cz = 0;
            if (SmoothFollowOffset(&cx, &cy, &cz)) {
                if (fabsf(cx - nx) > 0.01f || fabsf(cy - ny) > 0.01f ||
                    fabsf(cz - nz) > 0.01f) {
                    SmoothFollowSetOffset(nx, ny, nz);
                    touched++;
                }
            }
        } else {
            // --- legacy fallback: lift the live transform ---
            float cx = 0, cy = 0, cz = 0;
            if (UnityCamPos(&cx, &cy, &cz)) {
                const float target = cy + g_cfg.drone_height;
                if (!g_drone.lift_have || fabsf(cy - g_drone.lift_y) > 0.05f) {
                    UnityCamSetPos(cx, target, cz);
                    g_drone.lift_y = target;
                    g_drone.lift_have = true;
                    touched++;
                }
            }
        }
    } else {
        g_drone_hook_height = 0.0f;   // height slider at zero: no lift
    }

    // Hold the drag clamp above the raised camera — but only while something
    // was actually written, and it is restored by DroneRelease().
    if (touched > 0 && g_drone.ymax_have && g_f_cam_instance.ok() &&
        g_f_cam_ymax.ok()) {
        uintptr_t cam = ReadStaticPtr(g_f_cam_instance.info);
        if (PtrOk(cam)) {
            void* c = reinterpret_cast<void*>(cam);
            if (Read<int32_t>(c, g_f_cam_ymax.offset) < 9999)
                Write<int32_t>(c, g_f_cam_ymax.offset, 9999);
        }
    }

    static int dlog = 0;
    if (dlog++ % 300 == 0) {
        LOGI("drone: h=%.1f fov=+%.1f touched=%d sf=%d hook=%d", g_cfg.drone_height,
             g_cfg.drone_fov, touched, g_sf_inst != 0, g_drone_hook_ready);
    }
    return touched;
}

// ---- skin changer tick ----
// One write is lost on respawn or scene load, so re-apply to the local player
// roughly every second while the toggle is on.
void TickSkin() {
    if (!g_cfg.skin_enabled) return;
    static int frame = 0;
    if (++frame % 60 != 1) return;
    if (!Ready() || !g_f_local_player.ok()) return;
    uintptr_t mgr = ReadStaticPtr(g_f_mgr_instance.info);
    if (!PtrOk(mgr)) return;
    void* lp = Read<void*>(reinterpret_cast<void*>(mgr), g_f_local_player.offset);
    if (PtrOk(reinterpret_cast<uintptr_t>(lp))) SetShowSkin(lp, g_cfg.skin_id);
}

// Menu readout for the drone hook. Kept out of ApplyDrone so the menu can ask
// at any time without side effects.
const char* DroneHookState() {
    if (!g_cfg.drone_hook) return "off";
    if (g_drone_hook_ready) return "live";
    if (g_drone_hook_failed) return "unavailable on this build";
    if (!g_sf_class) return "waiting for SmoothFollow";
    return "not installed";
}

// MLBB jungle monster config ids. Unknown ids fall back to "Creep %d", and the
// first monster snapshot logs raw ids to logcat so the table can be calibrated
// against a live match. Ids 2002+ verified against XNISH's shipped config table
// (Lord 2002, Turtle 2003/2110, Buff 2004/2005, Lithowanderer 2056/2072); the
// low ids (7-16) are kept from the earlier calibration in case older scenes use
// the legacy id space.
struct MonsterId { int id; const char* name; };
const MonsterId kMonsters[] = {
    {7, "Lord"}, {8, "Turtle"}, {9, "Turtle"},
    {11, "Buff"}, {12, "Buff"}, {13, "Lithowander"}, {14, "Lithowander"},
    {15, "Crab"}, {16, "Crab"},
    {2002, "Lord"},
    {2003, "Turtle"}, {2110, "Turtle"},
    {2004, "Buff"}, {2005, "Buff"},
    {2056, "Lithowander"}, {2072, "Lithowander"},
};

const char* MonsterName(int id) {
    for (const auto& m : kMonsters)
        if (m.id == id) return m.name;
    return nullptr;
}

bool SetShowSkin(void* obj, int skin_id) {
    if (!obj || !F.show_skin.ok()) return false;
    Write<int32_t>(obj, F.show_skin.offset, skin_id);
    return true;
}

// ---- objective alerts ----
// HP tracking for the turtle/lord pits. The banner fires when an objective's HP
// dropped recently — someone is hitting it, and that is what the team needs to
// know even when the pit is fogged.
struct ObjTrack {
    bool seen = false;          // any live entity of this objective in view
    float hp = 0, hp_max = 0;
    float last_hp_seen = 0;     // hp at the last snapshot
    double last_hit_time = -1;  // clock time of the last HP decrease
    float x = 0, z = 0;         // last-seen pit position
    bool have_pos = false;
};
ObjTrack g_turtle, g_lord;
double g_clock = 0.0;  // seconds, advanced per frame by TrackObjectives

// Multi-entity camps (buffs, Lithowanderer) — there are several on the map, so
// instead of one pit they track whichever live member is lowest on HP.
struct CampTrack {
    bool seen = false;
    float hp = 0, hp_max = 0;
    int32_t id = 0;
    float x = 0, z = 0;
    bool have_pos = false;
};
CampTrack g_buff_camp, g_litho_camp;

void TrackCampGroup(const Entity* ents, int n, const char* want, CampTrack* t);
const char* MonsterCategory(const Entity& e);
const char* MonsterName(int id);

void TrackObjectives(const Entity* ents, int n) {
    // Real elapsed time between calls, not a fixed 1/60: the render cadence
    // varies (30/60 fps, stalls) and the contested window would silently
    // stretch with a fixed step.
    static double last = 0.0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const double now = static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
    if (last > 0.0) g_clock += now - last;
    last = now;

    for (int i = 0; i < n; i++) {
        const Entity& e = ents[i];
        const char* cat = MonsterCategory(e);
        if (!cat) continue;
        ObjTrack* t = nullptr;
        if (!strcmp(cat, "turtle")) t = &g_turtle;
        else if (!strcmp(cat, "lord")) t = &g_lord;
        if (!t || !e.obj) continue;

        if (!t->seen || e.hp < t->last_hp_seen - 0.5f) {
            // HP dropped since the last sighting: it is being attacked now.
            if (t->seen && e.hp < t->last_hp_seen - 0.5f) t->last_hit_time = g_clock;
            t->last_hp_seen = e.hp;
            t->seen = true;
        } else if (e.hp > t->last_hp_seen + 0.5f) {
            // Respawned / reset: re-baseline without firing the alert.
            t->last_hp_seen = e.hp;
        }
        t->hp = e.hp;
        t->hp_max = e.hp_max;
        if (e.x != 0.0f || e.z != 0.0f) {
            t->x = e.x;
            t->z = e.z;
            t->have_pos = true;
        }
    }

    // Buffs and Lithowanderer are not single pits — there are several on the
    // map at once, so they get their own model: track whichever live camp of
    // that kind is lowest on HP (the one worth stealing) and its position.
    // Without this the retri_buff / retri_litho toggles were dead: the old
    // ObjectivePos/TrackedObjectiveHp only ever knew "turtle" and "lord".
    TrackCampGroup(ents, n, "buff", &g_buff_camp);
    TrackCampGroup(ents, n, "creep", &g_litho_camp);
}

// Find the lowest-HP live entity of one category and keep its HP/position.
// A camp that disappears (killed, or left the snapshot) clears `seen` so a
// stale HP value can never trigger a retribution cast.
void TrackCampGroup(const Entity* ents, int n, const char* want, CampTrack* t) {
    const Entity* best = nullptr;
    for (int i = 0; i < n; i++) {
        const Entity& e = ents[i];
        if (!e.obj || e.is_dead) continue;
        const char* cat = MonsterCategory(e);
        if (!cat || strcmp(cat, want) != 0) continue;
        // Only the Lithowanderer counts as a litho camp; other creeps are
        // ordinary jungle and must not arm the retribution.
        if (!strcmp(want, "creep")) {
            const char* nm = MonsterName(e.id);
            if (!nm || !strstr(nm, "Litho")) continue;
        }
        if (!best || e.hp < best->hp) best = &e;
    }
    if (!best) {
        t->seen = false;
        return;
    }
    t->seen = true;
    t->hp = best->hp;
    t->hp_max = best->hp_max;
    t->id = best->id;
    if (best->x != 0.0f || best->z != 0.0f) {
        t->x = best->x;
        t->z = best->z;
        t->have_pos = true;
    }
}

static double SinceHit(const ObjTrack& t) {
    if (!t.seen || t.last_hit_time < 0) return 999.0;
    return g_clock - t.last_hit_time;
}
float TurtleSinceHit() { return static_cast<float>(SinceHit(g_turtle)); }
float LordSinceHit() { return static_cast<float>(SinceHit(g_lord)); }
bool TurtleContested() { return TurtleSinceHit() < 5.0f; }
bool LordContested() { return LordSinceHit() < 5.0f; }

// Which jungle category an entity belongs to, from its config id. Returns null
// for anything the caller's toggles do not care about.
const char* MonsterCategory(const Entity& e) {
    if (e.is_soldier) return "minion";
    const char* n = MonsterName(e.id);
    if (!n) return nullptr;
    if (strstr(n, "Turtle")) return "turtle";
    if (strstr(n, "Lord")) return "lord";
    if (strstr(n, "Buff")) return "buff";
    return "creep";
}

// ---- entity display name (runtime resolution) ----
// The entity's own m_RoleName, read live. This is the game's own name for the
// unit — for heroes it is the hero name — so no hardcoded table is needed for
// entities on screen; the tables stay as the fallback when the string is
// empty or the field is unavailable.
//
// The name pointer is parsed from a peeked copy of the entity (crash-safe):
// the pointer itself is then read live, and the string bytes are copied with
// the snapshot boundary, so a freed name string cannot take the process down.
bool EntityName(const Entity& e, char* out, int outlen) {
    if (!e.obj || !F.entity_name.ok() || !out || outlen <= 0) return false;
    alignas(16) uint8_t buf[kEntityPeek];
    if (!PeekEntity(e.obj, buf)) return false;
    void* sp = Read<void*>(buf, F.entity_name.offset);
    if (!PtrOk(reinterpret_cast<uintptr_t>(sp))) return false;
    alignas(16) uint8_t sbuf[kObjHeader + 4 + 257 * 2];
    if (!SafePeek(sp, sizeof(sbuf), sbuf)) return false;
    return ReadIl2CppString(reinterpret_cast<Il2CppString*>(sbuf), out,
                            static_cast<size_t>(outlen));
}

// ---- spell cooldowns ----
// Battle.CoolDownData, six 4-byte fields after the 0x10 object header:
//   0x10 iSpellID   0x14 uiCoolTime   0x18 originalMaxCdTime
//   0x1C uiStartTime 0x20 m_isCoolDown  0x24 iNewCoolChangeStep
// (Field order from the metadata dump; the earlier build read m_isCoolDown at
// 0x24, which is iNewCoolChangeStep — the flag never matched.)
// uiCoolTime is the remaining cooldown; values that look like wall-clock
// stamps are skipped until calibrated against a live match.
bool PeekSpellCD(void* cd, SpellCD* out) {
    if (!cd || !PtrOk(reinterpret_cast<uintptr_t>(cd))) return false;
    alignas(16) uint8_t buf[256];
    if (!SafePeek(cd, sizeof(buf), buf)) return false;

    out->spell_id = Read<int32_t>(buf, kObjHeader + 0x00);  // iSpellID (after header)
    const int32_t cool = Read<int32_t>(buf, kObjHeader + 0x04);   // uiCoolTime
    const int32_t maxcd = Read<int32_t>(buf, kObjHeader + 0x08);  // originalMaxCdTime
    out->active = Read<uint8_t>(buf, kObjHeader + 0x10) != 0;     // m_isCoolDown

    // Wall-clock stamps (epoch-scale) are not remaining times — skip until a
    // live match calibrates the semantics.
    if (cool < 0 || cool > 600000 || maxcd < 0 || maxcd > 600000) return false;
    out->remaining = static_cast<float>(cool) / 1000.0f;
    out->max = static_cast<float>(maxcd) / 1000.0f;
    return out->spell_id > 0;
}

int SnapshotSpellCDs(const Entity& e, SpellCD* out, int max) {
    if (!e.obj || !F.cool_comp.ok() || !out || max <= 0) return 0;
    alignas(16) uint8_t buf[kEntityPeek];
    if (!PeekEntity(e.obj, buf)) return 0;
    void* comp = Read<void*>(buf, F.cool_comp.offset);
    if (!PtrOk(reinterpret_cast<uintptr_t>(comp))) return 0;
    // ShowCoolDownComp fields: _pOwner(0), _dicCD(1), _dicSkillCnf(2). The
    // dictionary is expected in slot 1; probe it first, then the neighbouring
    // slots for field movement between builds. PtrSlot is arch-correct.
    alignas(16) uint8_t cbuf[128];
    if (!SafePeek(comp, sizeof(cbuf), cbuf)) return 0;
    int n = 0;
    for (size_t off : {PtrSlot(1), PtrSlot(0), PtrSlot(2), PtrSlot(3), PtrSlot(4), PtrSlot(5)}) {
        void* cand = Read<void*>(cbuf, off);
        if (!cand || !PtrOk(reinterpret_cast<uintptr_t>(cand))) continue;
        n = il2cpp::DictEach(cand, [&](int32_t key, void* val) {
            if (n >= max) return;
            SpellCD cd;
            if (PeekSpellCD(val, &cd)) out[n++] = cd;
        });
        if (n > 0) break;
    }
    return n;
}

// ---- slot -> spell id mapping ----
// Battle.OwnSkillData carries m_iSpellID and m_iUseIndex, and the owner's
// OwnSkillComp holds them in a dictionary. Knowing which spell id belongs to
// skill slot 1/2/3 lets the aim gate on that specific slot instead of the
// all-or-nothing "every known spell is cooling" test.
//
// The mapping is only used when it resolves cleanly; the caller falls back to
// the conservative test otherwise, so an unmapped build degrades rather than
// misfires.
int SkillSlotSpellId(const Entity& self, int slot) {
    if (!self.obj || slot < 0 || slot > 2) return 0;
    // OwnSkillComp hangs off ShowEntity. The exact field name has moved between
    // builds, so probe the candidates once and keep whichever binds.
    static Field f_comp = {};
    static bool probed = false;
    if (!probed) {
        probed = true;
        for (const char* cand : {"m_OwnSkillComp", "m_SkillComp", "m_ShowSkillComp"}) {
            f_comp = GetField(g_show_entity, cand);
            if (f_comp.ok()) {
                LOGI("skillmap: component field = %s (+0x%zx)", cand, f_comp.offset);
                break;
            }
        }
        if (!f_comp.ok()) LOGI("skillmap: no skill component field bound");
    }
    if (!f_comp.ok()) return 0;

    alignas(16) uint8_t buf[kEntityPeek];
    if (!PeekEntity(self.obj, buf)) return 0;
    void* comp = Read<void*>(buf, f_comp.offset);
    if (!PtrOk(reinterpret_cast<uintptr_t>(comp))) return 0;

    // OwnSkillComp: OwnSkillDict at index 1 -> the second pointer slot after
    // the header, same layout family as the cooldown dictionary. Probe a
    // couple of slots (arch-correct via PtrSlot).
    alignas(16) uint8_t cbuf[160];
    if (!SafePeek(comp, sizeof(cbuf), cbuf)) return 0;
    for (size_t off : {PtrSlot(1), PtrSlot(0), PtrSlot(2), PtrSlot(3)}) {
        void* dict = Read<void*>(cbuf, off);
        if (!dict || !PtrOk(reinterpret_cast<uintptr_t>(dict))) continue;
        int found = 0;
        il2cpp::DictEach(dict, [&](int32_t /*key*/, void* val) {
            if (found || !val || !PtrOk(reinterpret_cast<uintptr_t>(val))) return;
            alignas(16) uint8_t sbuf[256];
            if (!SafePeek(val, sizeof(sbuf), sbuf)) return;
            // OwnSkillData: m_iSpellID at the payload start (after the header),
            // m_iUseIndex ten ints later.
            const int32_t spell = Read<int32_t>(sbuf, kObjHeader + 0x00);
            const int32_t use_index = Read<int32_t>(sbuf, kObjHeader + 0x28);
            // UseIndex 1..3 are the three hero skills; 0 is the basic attack.
            if (use_index == slot + 1 && spell > 0) found = spell;
        });
        if (found) return found;
    }
    return 0;
}

// ---- awareness (game-owned reporter state) ----
// The game's reporter classes carry m_bEnable / m_bActive (CheatAfkReporter)
// and bHasRecord (AntiCheatReporter), but their instances are held by
// managers this build does not expose as singletons — the live values are
// not reachable. What IS verifiable is that the fields exist: the Status tab
// uses this to show the detection surface honestly, without pretending to
// read state it cannot see.
bool ReporterFieldsBound() {
    bool ok = true;
    if (Il2CppClass* k = il2cpp::FindClass("", "CheatAfkReporter")) {
        ok &= il2cpp::GetField(k, "m_bEnable").ok();
        ok &= il2cpp::GetField(k, "m_bActive").ok();
    }
    if (Il2CppClass* k = il2cpp::FindClass("", "AntiCheatReporter")) {
        ok &= il2cpp::GetField(k, "bHasRecord").ok();
    }
    return ok;
}

// Pit world positions for enemy-proximity checks near the objectives. Also
// answers for the multi-entity camps ("buff", "creep"/litho), which track the
// lowest-HP live member — that is the one a retribution would be spent on.
bool ObjectivePos(const char* which, float* x, float* z) {
    if (!which || !x || !z) return false;
    if (!strcmp(which, "turtle")) {
        if (!g_turtle.have_pos) return false;
        *x = g_turtle.x; *z = g_turtle.z; return true;
    }
    if (!strcmp(which, "lord")) {
        if (!g_lord.have_pos) return false;
        *x = g_lord.x; *z = g_lord.z; return true;
    }
    if (!strcmp(which, "buff")) {
        if (!g_buff_camp.have_pos) return false;
        *x = g_buff_camp.x; *z = g_buff_camp.z; return true;
    }
    if (!strcmp(which, "creep")) {
        if (!g_litho_camp.have_pos) return false;
        *x = g_litho_camp.x; *z = g_litho_camp.z; return true;
    }
    return false;
}

// The tracked objective's live HP. Turtle/Lord come from their pit trackers;
// buff and litho from the lowest-HP live camp member.
bool TrackedObjectiveHp(const char* which, float* hp, float* hp_max) {
    if (!which || !hp || !hp_max) return false;
    if (!strcmp(which, "turtle")) {
        if (!g_turtle.seen || g_turtle.hp_max <= 0.0f) return false;
        *hp = g_turtle.hp; *hp_max = g_turtle.hp_max; return true;
    }
    if (!strcmp(which, "lord")) {
        if (!g_lord.seen || g_lord.hp_max <= 0.0f) return false;
        *hp = g_lord.hp; *hp_max = g_lord.hp_max; return true;
    }
    if (!strcmp(which, "buff")) {
        if (!g_buff_camp.seen || g_buff_camp.hp_max <= 0.0f) return false;
        *hp = g_buff_camp.hp; *hp_max = g_buff_camp.hp_max; return true;
    }
    if (!strcmp(which, "creep")) {
        if (!g_litho_camp.seen || g_litho_camp.hp_max <= 0.0f) return false;
        *hp = g_litho_camp.hp; *hp_max = g_litho_camp.hp_max; return true;
    }
    return false;
}

}  // namespace game
