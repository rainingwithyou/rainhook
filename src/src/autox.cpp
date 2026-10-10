#include "autox.h"

#include <android/log.h>
#include <cmath>
#include <cstring>
#include <cstdio>

#include "config.h"
#include "game.h"
#include "input.h"
#include "persist.h"
#include "synth.h"
#include "target.h"

#define LOG_TAG "Rain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace autox {

// Frame counter, shared with the feature ticks for their cooldown gates.
int g_frame_counter = 0;

namespace {

using game::Entity;

// Kimmy's config id. Calibration-verified live: the local player's Kimmy
// reported m_ID 71 with the hero select showing the same hero.
constexpr int kKimmyId = 71;

// ---- shared cooldown-gate state ----
int g_last_retri_frame = -1000;
int g_last_aim_frame = -1000;
int g_last_skill_frame = -1000;
int g_combo_step = 0;      // 0 = armed, 1..N = casting, 99 = cooling down
int g_combo_wait = 0;

// Kimmy hold latch, so the release path always runs exactly once.
bool g_kimmy_holding = false;
int  g_kimmy_lost_frames = 0;
// Finger id the aura is pressing with, so its own synthesized touch cannot be
// mistaken for the player's toggle tap. -1 = none.
int  g_kimmy_own_finger = -1;

// In-match floating toggle (the reference mod's "KM" button).
bool g_kimmy_btn_open = true;

const char* SlotName(int slot) {
    switch (slot) {
        case SLOT_S1:    return "S1";
        case SLOT_S2:    return "S2";
        case SLOT_S3:    return "S3";
        case SLOT_BASIC: return "Basic";
        case SLOT_SPELL: return "Spell";
    }
    return "?";
}

float Dist(const Entity& a, const Entity& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The frame's snapshot, already built by esp::Draw before this runs.
const game::FrameSnapshot& Snap() { return game::Snapshot(); }
const Entity* Self() {
    const game::FrameSnapshot& s = Snap();
    return s.has_self ? &s.self : nullptr;
}

// ---------------------------------------------------------------------------
// Auto retribution
// ---------------------------------------------------------------------------

// Enemies of ours near a world point.
bool EnemyNear(float px, float pz, float radius, const Entity* self) {
    if (!self) return false;
    const game::FrameSnapshot& s = Snap();
    if (s.n_players <= 0 || s.local_camp <= 0) return false;
    for (int i = 0; i < s.n_players; i++) {
        const Entity& e = s.players[i];
        if (!e.obj || e.is_dead) continue;
        if (e.camp == s.local_camp) continue;
        if (!e.is_hero || e.is_soldier) continue;
        const float dx = e.x - px, dz = e.z - pz;
        if (std::sqrt(dx * dx + dz * dz) <= radius) return true;
    }
    return false;
}

// Predicted Retribution damage for the local hero. Level drives it most; jungle
// stacks add on top. The coefficients are menu-tunable because the game's real
// table is compiled in and not statically recoverable.
float PredictedRetriDamage(const Entity* self) {
    if (!self) return 0.0f;
    const float lvl = (self->level > 0) ? static_cast<float>(self->level) : 1.0f;
    return (g_cfg.retri_base_dmg +
            g_cfg.retri_dmg_per_level * (lvl - 1.0f) +
            g_cfg.retri_stack_dmg * static_cast<float>(g_cfg.retri_stacks));
}

// Cast Retribution when a watched objective is low enough to secure. Each
// objective category has its own toggle now — the previous version iterated
// lord and turtle unconditionally and the checkboxes did nothing.
void TickRetri(const Entity* self) {
    if (!g_cfg.retri_enabled) return;
    if (g_cfg.retri_btn_x <= 0.0f || g_cfg.retri_btn_y <= 0.0f) return;  // uncalibrated
    if (g_frame_counter - g_last_retri_frame < 180) return;              // ~3 s

    struct Watch { const char* which; bool on; };
    const Watch watch[] = {
        {"lord",   g_cfg.retri_lord},
        {"turtle", g_cfg.retri_turtle},
        {"buff",   g_cfg.retri_buff},
        {"creep",  g_cfg.retri_litho},
    };

    for (const Watch& w : watch) {
        if (!w.on) continue;

        float px = 0, pz = 0;
        if (!game::ObjectivePos(w.which, &px, &pz)) continue;

        // Trigger mode 0 keeps the enemy guard; mode 1 fires regardless, which
        // is what you want when you are the one starting the pit.
        if (g_cfg.retri_trigger == 0 &&
            !EnemyNear(px, pz, g_cfg.retri_enemy_dist, self)) {
            continue;
        }

        float ohp = 0, ohpmax = 0;
        if (!game::TrackedObjectiveHp(w.which, &ohp, &ohpmax)) continue;
        if (ohpmax <= 0.0f) continue;

        bool fire = false;
        if (g_cfg.retri_use_model) {
            const float kill = PredictedRetriDamage(self) * g_cfg.retri_safety;
            fire = (ohp <= kill);
        } else {
            fire = ((ohp / ohpmax) * 100.0f <= g_cfg.retri_hp_pct);
        }
        if (!fire) continue;

        g_last_retri_frame = g_frame_counter;
        synth::Tap(g_cfg.retri_btn_x, g_cfg.retri_btn_y);
        LOGI("auto: retri %s (hp=%.0f/%.0f, kill=%.0f, mode=%s)", w.which, ohp,
             ohpmax, PredictedRetriDamage(self),
             g_cfg.retri_trigger ? "always" : "enemy-near");
        return;
    }
}

// ---------------------------------------------------------------------------
// Basic-attack assist
// ---------------------------------------------------------------------------

target::Query AimQuery() {
    target::Query q;
    q.range = g_cfg.aim_range;
    q.mode = static_cast<target::Mode>(g_cfg.aim_mode);
    q.priority = static_cast<target::Priority>(g_cfg.aim_priority);
    q.classes = target::MakeOrder(g_cfg.target_order);
    return q;
}

void TickAim(const Entity* self) {
    if (!g_cfg.aim_enabled) return;
    if (g_cfg.aim_btn_x <= 0.0f || g_cfg.aim_btn_y <= 0.0f) return;  // uncalibrated
    if (!self || self->is_dead) return;
    if (g_frame_counter - g_last_aim_frame < 90) return;             // ~1.5 s

    const game::FrameSnapshot& s = Snap();
    const target::Result r = target::Select(*self, s.players, s.n_players,
                                            s.monsters, s.n_monsters, AimQuery());
    if (!r.ok()) return;

    g_last_aim_frame = g_frame_counter;
    synth::Tap(g_cfg.aim_btn_x, g_cfg.aim_btn_y);
    LOGI("auto: aim %s id=%d hp%%=%.1f dist=%.1f", target::ClassName(r.cls),
         r.ent->id, r.hp_pct * 100.0f, r.dist);
}

// ---------------------------------------------------------------------------
// Kimmy press-and-hold
// ---------------------------------------------------------------------------

// Hold the basic-attack button so the game's own held-attack targeting keeps
// firing and auto-locks onto whatever is in range. The release path runs on
// every condition that should end the press — target lost, hero dead, feature
// off, menu open — because a stuck ACTION_DOWN would leave the game holding a
// button forever.
//
// Two separate things, deliberately not the same gesture:
//
//   ARM/DISARM  the floating KM button (menu::DrawFloatingButtons). It is an
//               ImGui button, so it works anywhere and cannot be confused with
//               a game input.
//   ANCHOR      captured automatically from the first tap in the HUD's action
//               zone while a match is live. Hero-select and mode screens also
//               have taps down there, so a spawned local entity is required
//               before an anchor is accepted.
//
// The aura's own synthesized press is visible to Unity as a real touch. Its
// finger is recorded so (a) it cannot be mistaken for a player tap, and (b) it
// is hidden from ImGui, because otherwise the KM button would stop responding
// the moment the aura armed — its hold would occupy the slot ImGui reads.
void TickKimmy(const Entity* self, bool menu_open) {
    const bool on_kimmy = self && self->id == kKimmyId;
    const game::FrameSnapshot& snap = Snap();
    const bool in_match = snap.has_self && self != nullptr && self->obj != nullptr;

    // ---- anchor capture ----
    if (on_kimmy && in_match && !menu_open &&
        g_cfg.kimmy_btn_x <= 0.0f && g_cfg.kimmy_btn_y <= 0.0f) {
        TouchPoint pts[10];
        const int n = input::Touches(pts, 10);
        const float sw = static_cast<float>(game::ScreenWidth());
        const float sh = static_cast<float>(game::ScreenHeight());
        for (int i = 0; i < n; i++) {
            if (pts[i].phase != 0) continue;   // Began only
            if (pts[i].finger_id == g_kimmy_own_finger) continue;
            // Action buttons live in the bottom-right of the HUD.
            if (pts[i].x < sw * 0.55f || pts[i].y < sh * 0.55f) continue;
            g_cfg.kimmy_btn_x = pts[i].x;
            g_cfg.kimmy_btn_y = pts[i].y;
            persist::MarkDirty();
            LOGI("auto: kimmy M1 anchored at (%.0f,%.0f)", pts[i].x, pts[i].y);
            break;
        }
    }

    const bool want = g_cfg.kimmy_enabled && !menu_open && on_kimmy &&
                      g_cfg.kimmy_btn_x > 0.0f && g_cfg.kimmy_btn_y > 0.0f &&
                      !self->is_dead;

    if (!want) {
        if (g_kimmy_holding) {
            synth::HoldEnd();
            g_kimmy_holding = false;
            g_kimmy_lost_frames = 0;
            g_kimmy_own_finger = -1;
            input::SetIgnoredFinger(-1);
            LOGI("auto: kimmy released");
        }
        return;
    }

    const game::FrameSnapshot& s = Snap();
    target::Query q;
    q.range = g_cfg.kimmy_range;
    q.priority = static_cast<target::Priority>(g_cfg.kimmy_priority);
    if (g_cfg.kimmy_minion_mode) {
        // Lane clear: minions first, then the jungle camps, then heroes. The
        // reference mod exposes exactly this ordering as its minion-clear mode.
        q.mode = target::Mode::Both;
        q.classes.order[0] = target::Class::Minion;
        q.classes.order[1] = target::Class::Creep;
        q.classes.order[2] = target::Class::Buff;
        q.classes.order[3] = target::Class::Turtle;
        q.classes.order[4] = target::Class::Lord;
        q.classes.order[5] = target::Class::Hero;
    } else {
        q.mode = target::Mode::Both;
        q.classes = target::MakeOrder(g_cfg.target_order);
    }

    const target::Result r = target::Select(*self, s.players, s.n_players,
                                            s.monsters, s.n_monsters, q);

    if (!r.ok()) {
        // Hysteresis: a target flickering at the edge of range must not make
        // the press stutter. Release only after a short grace period.
        if (g_kimmy_holding && ++g_kimmy_lost_frames > 12) {
            synth::HoldEnd();
            g_kimmy_holding = false;
            g_kimmy_own_finger = -1;
            input::SetIgnoredFinger(-1);
            LOGI("auto: kimmy released (no target)");
        }
        return;
    }
    g_kimmy_lost_frames = 0;

    if (!g_kimmy_holding) {
        // Let any in-flight gesture finish before claiming the pointer.
        if (synth::Busy()) return;
        if (synth::HoldBegin(g_cfg.kimmy_btn_x, g_cfg.kimmy_btn_y)) {
            g_kimmy_holding = true;
            LOGI("auto: kimmy holding on %s id=%d", target::ClassName(r.cls), r.ent->id);
        }
        return;
    }

    // Identify which finger the hold owns, so it can be hidden from ImGui and
    // never mistaken for a player tap. It is the one sitting exactly on the
    // anchor; a real finger moves and would not stay there frame after frame.
    if (g_kimmy_own_finger < 0) {
        TouchPoint pts[10];
        const int n = input::Touches(pts, 10);
        for (int i = 0; i < n; i++) {
            const float dx = pts[i].x - g_cfg.kimmy_btn_x;
            const float dy = pts[i].y - g_cfg.kimmy_btn_y;
            if (dx * dx + dy * dy <= 36.0f) {
                g_kimmy_own_finger = pts[i].finger_id;
                input::SetIgnoredFinger(g_kimmy_own_finger);
                LOGI("auto: kimmy owns finger %d", g_kimmy_own_finger);
                break;
            }
        }
    }

    // Keep the press anchored to the button's current position, which moves
    // when the player changes the HUD layout. In steer mode the press is
    // instead dragged toward the target's screen position: the game reads the
    // held drag vector and rotates the aim, which is XNISH's Kimmy Auto Aim
    // layered on top of the hold.
    float hx = g_cfg.kimmy_btn_x;
    float hy = g_cfg.kimmy_btn_y;
    if (g_cfg.kimmy_aim_mode == 1 && r.ent) {
        float tx = 0, ty = 0;
        if (game::WorldToScreenEx(r.ent->x, r.ent->y, r.ent->z,
                                  r.ent->pos_is_unity, &tx, &ty)) {
            float dx = tx - hx, dy = ty - hy;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len >= 1.0f) {
                // Only the direction is taken from the target; the magnitude is
                // the fixed drag length, so the same steer works at any range.
                dx /= len;
                dy /= len;
                hx += dx * g_cfg.kimmy_drag_len;
                hy += dy * g_cfg.kimmy_drag_len;
            }
        }
    }
    synth::HoldMove(hx, hy);
}

// ---------------------------------------------------------------------------
// Skill auto aim
// ---------------------------------------------------------------------------

// Which skill slot an entity's cooldown dictionary maps to. When the slot ->
// spell-id mapping resolves (game::SkillSlotSpellId), the gate is exact: the
// slot is ready unless that specific spell is cooling. When it does not, fall
// back to the conservative "every known spell is cooling" test — a skill fired
// while cooling is a wasted tap, but a skill withheld is just a miss.
bool SlotReady(const Entity& self, int slot) {
    if (!g_cfg.skill[slot].cd_check) return true;

    // Exact path: only the three hero skills have an OwnSkillData slot index.
    if (slot <= SLOT_S3) {
        const int spell = game::SkillSlotSpellId(self, slot);
        if (spell > 0) {
            game::SpellCD cds[8];
            const int n = game::SnapshotSpellCDs(self, cds, 8);
            for (int i = 0; i < n; i++) {
                if (cds[i].spell_id == spell && cds[i].active &&
                    cds[i].remaining > 0.0f)
                    return false;
            }
            return true;
        }
    }

    game::SpellCD cds[8];
    const int n = game::SnapshotSpellCDs(self, cds, 8);
    if (n <= 0) return true;   // no CD data on this build: do not block
    // Slot order is not known to match the dictionary order, so treat "every
    // known spell is on cooldown" as not-ready and anything else as ready.
    int active = 0;
    for (int i = 0; i < n; i++)
        if (cds[i].active && cds[i].remaining > 0.0f) active++;
    return active < n;
}

void TickSkillAim(const Entity* self) {
    if (!self || self->is_dead) return;
    if (g_frame_counter - g_last_skill_frame < 30) return;   // ~0.5 s cadence

    const game::FrameSnapshot& s = Snap();

    for (int slot = 0; slot < SLOT_COUNT; slot++) {
        const SkillCfg& sk = g_cfg.skill[slot];
        if (!sk.enabled) continue;
        if (sk.btn_x <= 0.0f || sk.btn_y <= 0.0f) continue;   // uncalibrated
        if (!SlotReady(*self, slot)) continue;

        target::Query q;
        q.range = sk.range;
        q.mode = static_cast<target::Mode>(sk.target_mode);
        q.priority = static_cast<target::Priority>(sk.priority);
        q.classes = target::MakeOrder(g_cfg.target_order);

        const target::Result r = target::Select(*self, s.players, s.n_players,
                                                s.monsters, s.n_monsters, q);
        if (!r.ok()) continue;

        if (sk.aim_mode == 0) {
            // Smart-cast: tap the button, the game picks the direction.
            synth::Tap(sk.btn_x, sk.btn_y);
            g_last_skill_frame = g_frame_counter;   // stamp only on a real cast
            LOGI("auto: %s smart-cast at %s id=%d", SlotName(slot),
                 target::ClassName(r.cls), r.ent->id);
        } else {
            // Drag-aim: press the button and drag toward where the target is on
            // screen. The direction is what matters, not the exact endpoint —
            // the game reads the drag vector, so it is clamped to a fixed
            // length and the same magnitude works at any distance.
            float tx = 0, ty = 0;
            if (!game::WorldToScreenEx(r.ent->x, r.ent->y, r.ent->z,
                                       r.ent->pos_is_unity, &tx, &ty)) {
                // Projection failed (target off-screen or behind the camera):
                // fall back to a smart-cast rather than burning the window.
                synth::Tap(sk.btn_x, sk.btn_y);
                g_last_skill_frame = g_frame_counter;
                LOGI("auto: %s smart-cast (target off-screen)", SlotName(slot));
                return;
            }
            float dx = tx - sk.btn_x, dy = ty - sk.btn_y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1.0f) continue;   // target is on the button: nothing to aim
            dx /= len;
            dy /= len;
            synth::Drag(sk.btn_x, sk.btn_y,
                        sk.btn_x + dx * sk.drag_len,
                        sk.btn_y + dy * sk.drag_len, 4);
            g_last_skill_frame = g_frame_counter;   // stamp only on a real cast
            LOGI("auto: %s drag-aim at %s id=%d (dir %.2f,%.2f)", SlotName(slot),
                 target::ClassName(r.cls), r.ent->id, dx, dy);
        }
        return;   // one cast per cadence window
    }
}

// ---------------------------------------------------------------------------
// Skill combo
// ---------------------------------------------------------------------------

// Position of the button a combo step casts, from the calibrated source
// positions and the configured order.
void ComboButton(int step, float* x, float* y) {
    const int i = step - 1;
    const int slot = (i >= 0 && i < 5) ? g_cfg.combo_seq[i] : 0;
    switch (slot) {
        case 1:  *x = g_cfg.combo_s2_x; *y = g_cfg.combo_s2_y; break;
        case 2:  *x = g_cfg.combo_s3_x; *y = g_cfg.combo_s3_y; break;
        case 3:  *x = g_cfg.combo_ba_x; *y = g_cfg.combo_ba_y; break;
        default: *x = g_cfg.combo_s1_x; *y = g_cfg.combo_s1_y; break;
    }
}

// Cast a configured button sequence when an enemy hero enters range. The
// sequence runs to g_cfg.combo_steps, then cools down; the target has to leave
// and re-enter range before the next one.
void TickCombo(const Entity* self) {
    if (!g_cfg.combo_enabled) return;
    if (g_cfg.combo_s1_x <= 0.0f || g_cfg.combo_s1_y <= 0.0f) return;  // uncalibrated
    if (!self || self->is_dead) return;

    const int steps = (g_cfg.combo_steps < 1) ? 1
                    : (g_cfg.combo_steps > 5) ? 5 : g_cfg.combo_steps;

    const game::FrameSnapshot& s = Snap();
    if (s.n_players <= 0 || s.local_camp <= 0) return;

    // The combo trigger runs through the shared targeting policy so it obeys
    // the same class priority and priority comparator as every other feature —
    // the old version had its own hero-only loop that ignored both.
    target::Query tq;
    tq.range = g_cfg.combo_range;
    tq.mode = target::Mode::PlayersOnly;   // combos are a hero-vs-hero tool
    tq.priority = static_cast<target::Priority>(g_cfg.combo_priority);
    tq.classes = target::MakeOrder(g_cfg.target_order);
    const target::Result tr = target::Select(*self, s.players, s.n_players,
                                             s.monsters, s.n_monsters, tq);
    const bool target_in_range = tr.ok();

    // phase 0 = armed, 1..steps = casting, 99 = cooling down
    if (g_combo_step == 0) {
        if (!target_in_range) return;
        g_combo_step = 1;
        g_combo_wait = g_frame_counter;
    } else if (g_combo_step == 99) {
        if (g_frame_counter < g_combo_wait) return;
        if (target_in_range) return;   // wait for the target to leave first
        g_combo_step = 0;
        return;
    }
    if (g_frame_counter < g_combo_wait) return;

    float bx = 0, by = 0;
    ComboButton(g_combo_step, &bx, &by);
    if (bx > 0.0f && by > 0.0f) {
        synth::Tap(bx, by);
        LOGI("auto: combo step %d/%d", g_combo_step, steps);
    }
    g_combo_step++;
    if (g_combo_step > steps) {
        g_combo_step = 99;                       // ~6 s, then re-arm on exit
        g_combo_wait = g_frame_counter + 360;
    } else {
        g_combo_wait = g_frame_counter + g_cfg.combo_delay;
    }
}

}  // namespace

void Tick(bool menu_open) {
    g_frame_counter++;

    const Entity* self = Self();

    TickRetri(self);
    TickAim(self);
    TickKimmy(self, menu_open);
    TickSkillAim(self);
    TickCombo(self);
}

void ReleaseAll() {
    if (g_kimmy_holding) {
        synth::HoldEnd();
        g_kimmy_holding = false;
        g_kimmy_lost_frames = 0;
    }
    g_kimmy_own_finger = -1;
    input::SetIgnoredFinger(-1);
    synth::Abort();
}

bool KimmyButtonOpen() { return g_kimmy_btn_open; }
void SetKimmyButtonOpen(bool open) { g_kimmy_btn_open = open; }

}  // namespace autox
