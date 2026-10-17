#include "esp.h"

#include <cstdio>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <android/log.h>

#include <imgui.h>

#include "config.h"
#include "game.h"
#include "icons.h"

// ESP calibration logging (ESP-offset hunt, 2026-10-17). Off in releases; flip
// to 1 to sample projections per hero box when chasing an alignment report.
#define RAIN_ESP_DIAG 0

#define LOG_TAG "Rain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

Config g_cfg;

// Per-category defaults. Colours are per-index, so they cannot live in the
// CatCfg member initialiser; this runs once at startup, before the saved config
// is loaded over the top.
void ConfigDefaults(Config& c) {
    struct Def {
        EspCat id;
        float r, g, b, a;
        bool enabled, box, health, health_text, name, hero_name, distance, icon;
        int dead_mode;
    };
    static const Def kDefs[] = {
        // self: off by default (it clutters the centre of the screen), but the
        // box and bar are there the moment it is switched on.
        {CAT_SELF,          0.35f, 0.65f, 1.00f, 1.0f, false, true,  true,  false, true,  false, false, false, 1},
        {CAT_ALLY_HERO,     0.30f, 0.85f, 0.40f, 1.0f, false, true,  true,  false, true,  true,  true,  true,  0},
        // Enemy hero corpses stay visible (they are information), minions hide.
        // Dead heroes default to HIDE: the engine stops updating their cached
        // position the moment they die, so a corpse ESP is drawn at the last
        // live position — which reads as the box being detached from the hero
        // (the "42m frozen" report). Users who want the corpse marker can set
        // the row's dead_mode to 1 (own colour) or 2 (shared dead colour).
        {CAT_ENEMY_HERO,    1.00f, 0.25f, 0.25f, 1.0f, true,  true,  true,  false, true,  true,  true,  true,  0},
        {CAT_ALLY_MINION,   0.35f, 0.70f, 0.45f, 1.0f, false, true,  true,  false, false, false, false, false, 0},
        {CAT_ENEMY_MINION,  0.95f, 0.55f, 0.30f, 1.0f, false, true,  true,  false, false, false, false, false, 0},
        {CAT_TURTLE,        0.55f, 0.85f, 1.00f, 1.0f, true,  true,  true,  false, false, true,  false, false, 0},
        {CAT_LORD,          0.80f, 0.45f, 1.00f, 1.0f, true,  true,  true,  false, false, true,  false, false, 0},
        // Blue rune buff (Serpent) — blue, matching what the game calls it.
        {CAT_BLUE_BUFF,     0.30f, 0.55f, 1.00f, 1.0f, true,  true,  true,  false, false, true,  false, false, 0},
        // Red rune buff (Fire Beetle) — red-orange.
        {CAT_RED_BUFF,      1.00f, 0.45f, 0.25f, 1.0f, true,  true,  true,  false, false, true,  false, false, 0},
        // Lithowanderer — the walkie-grass camp; green.
        {CAT_LITHO,         0.45f, 0.95f, 0.45f, 1.0f, true,  true,  true,  false, false, true,  false, false, 0},
        {CAT_CREEP,         0.85f, 0.85f, 0.60f, 1.0f, true,  true,  true,  false, false, true,  false, false, 0},
    };
    for (const Def& d : kDefs) {
        CatCfg& cc = c.cat[d.id];
        cc.color[0] = d.r; cc.color[1] = d.g; cc.color[2] = d.b; cc.color[3] = d.a;
        cc.enabled = d.enabled;
        cc.box = d.box;
        cc.health = d.health;
        cc.health_text = d.health_text;
        cc.name = d.name;
        cc.hero_name = d.hero_name;
        cc.distance = d.distance;
        cc.icon = d.icon;
        cc.dead_mode = d.dead_mode;
    }
}

namespace esp {

namespace {

inline ImU32 Col(const float c[4]) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3]));
}

inline ImU32 WithAlpha(ImU32 col, unsigned char a) {
    return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

void TextShadowed(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* txt) {
    dl->AddText(ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 190), txt);
    dl->AddText(pos, col, txt);
}

// Health fraction -> colour, green through amber to red.
ImU32 HealthColor(float pct, unsigned char a = 255) {
    if (pct > 0.5f)  return IM_COL32(70, 220, 90, a);
    if (pct > 0.25f) return IM_COL32(240, 200, 60, a);
    return IM_COL32(235, 70, 70, a);
}

// Model height in world units, used to derive the box from a single projected
// point. Calibrated against the sprites: heroes are ~1.35 units, soldiers
// shorter; the jungle units are much larger and each got its own measurement
// after the single 1.9-unit guess floated their boxes above the model.
float UnitHeight(const game::Entity& e) {
    return e.is_soldier ? 1.2f : 1.35f;
}

float EntityHeight(EspCat id) {
    switch (id) {
        case CAT_TURTLE:    return 3.6f;
        case CAT_LORD:      return 4.2f;
        case CAT_BLUE_BUFF: return 2.4f;
        case CAT_RED_BUFF:  return 2.4f;
        case CAT_LITHO:     return 2.0f;
        case CAT_CREEP:     return 1.6f;
        default:            return 1.6f;
    }
}

// Box styles, all anchored to the projected base point and head point.
//   0 = 2D corner, 1 = full outline, 2 = glow (layered translucent halo).
void DrawBox(ImDrawList* dl, float x0, float y0, float x1, float y1,
             ImU32 col, int style, int glow_layers, float th) {
    if (style == 2) {
        for (int i = glow_layers; i >= 1; i--) {
            const float e = i * 3.0f;
            dl->AddRectFilled(ImVec2(x0 - e, y0 - e), ImVec2(x1 + e, y1 + e),
                              WithAlpha(col, static_cast<unsigned char>(52 / i)),
                              6.0f + i * 2.0f);
        }
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), col, 4.0f, 0, th);
        return;
    }
    if (style == 1) {
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), col, 0.0f, 0, th);
        return;
    }

    const float cw = (x1 - x0) * 0.28f;
    const float ch = (y1 - y0) * 0.22f;
    dl->AddLine(ImVec2(x0, y0), ImVec2(x0 + cw, y0), col, th);
    dl->AddLine(ImVec2(x1 - cw, y0), ImVec2(x1, y0), col, th);
    dl->AddLine(ImVec2(x0, y1), ImVec2(x0 + cw, y1), col, th);
    dl->AddLine(ImVec2(x1 - cw, y1), ImVec2(x1, y1), col, th);
    dl->AddLine(ImVec2(x0, y0), ImVec2(x0, y0 + ch), col, th);
    dl->AddLine(ImVec2(x0, y1 - ch), ImVec2(x0, y1), col, th);
    dl->AddLine(ImVec2(x1, y0), ImVec2(x1, y0 + ch), col, th);
    dl->AddLine(ImVec2(x1, y1 - ch), ImVec2(x1, y1), col, th);
}

// Health bar in one of four orientations around the box.
//   0 = vertical left, 1 = vertical right, 2 = horizontal up, 3 = horizontal down.
void DrawHealthBar(ImDrawList* dl, int pos, float x0, float y0, float x1, float y1,
                   float pct) {
    const ImU32 bg = IM_COL32(0, 0, 0, 170);
    const ImU32 hc = HealthColor(pct);
    const ImU32 border = IM_COL32(0, 0, 0, 220);

    if (pos <= 1) {
        const float bw = 5.0f;
        const float bx = (pos == 0) ? x0 - bw - 4.0f : x1 + 4.0f;
        dl->AddRectFilled(ImVec2(bx, y0), ImVec2(bx + bw, y1), bg);
        const float fh = (y1 - y0) * pct;
        dl->AddRectFilled(ImVec2(bx, y1 - fh), ImVec2(bx + bw, y1), hc);
        dl->AddRect(ImVec2(bx, y0), ImVec2(bx + bw, y1), border);
    } else {
        const float bh = 5.0f;
        const float by = (pos == 2) ? y0 - bh - 4.0f : y1 + 4.0f;
        dl->AddRectFilled(ImVec2(x0, by), ImVec2(x1, by + bh), bg);
        const float fw = (x1 - x0) * pct;
        dl->AddRectFilled(ImVec2(x0, by), ImVec2(x0 + fw, by + bh), hc);
        dl->AddRect(ImVec2(x0, by), ImVec2(x1, by + bh), border);
    }
}

// Display name: the game's own m_RoleName when it resolves — that naming is
// authoritative — then the hardcoded tables, then a synthetic label.
void UnitLabel(const game::Entity& e, EspCat id, char* out, size_t n) {
    char live[64] = {0};
    if (game::EntityName(e, live, sizeof(live)) && live[0]) {
        snprintf(out, n, "%s", live);
        return;
    }
    if (id >= CAT_TURTLE) {
        const char* mn = game::MonsterName(e.id);
        if (mn) snprintf(out, n, "%s", mn);
        else    snprintf(out, n, "Creep %d", e.id);
        return;
    }
    if (e.is_soldier) {
        snprintf(out, n, "Minion");
        return;
    }
    const char* hn = game::HeroName(e.id);
    if (hn) snprintf(out, n, "%s", hn);
    else    snprintf(out, n, "Hero %d", e.id);
}

// Which category an entity from m_ShowPlayers belongs to. Self is decided by
// guid before this is called.
EspCat PlayerCat(const game::Entity& e, bool enemy) {
    if (e.is_soldier) return enemy ? CAT_ENEMY_MINION : CAT_ALLY_MINION;
    return enemy ? CAT_ENEMY_HERO : CAT_ALLY_HERO;
}

EspCat MonsterCat(const char* cat) {
    if (!cat) return CAT_COUNT;  // unclassified id: skipped
    if (!strcmp(cat, "turtle"))    return CAT_TURTLE;
    if (!strcmp(cat, "lord"))      return CAT_LORD;
    if (!strcmp(cat, "blue_buff")) return CAT_BLUE_BUFF;
    if (!strcmp(cat, "red_buff"))  return CAT_RED_BUFF;
    if (!strcmp(cat, "litho"))     return CAT_LITHO;
    return CAT_CREEP;            // "creep" and lane "minion" share a row
}

// ---------------------------------------------------------------------------
// Minimap
// ---------------------------------------------------------------------------

void WorldToMinimap(float wx, float wz, float* mx, float* my) {
    float u, v;
    if (g_cfg.mm_rotate45) {
        // Undo the map's 45-degree world rotation so the diamond fills the square.
        u = (wx - wz) / (2.0f * g_cfg.mm_half) + 0.5f;
        v = (wx + wz) / (2.0f * g_cfg.mm_half) + 0.5f;
    } else {
        u = wx / (2.0f * g_cfg.mm_half) + 0.5f;
        v = wz / (2.0f * g_cfg.mm_half) + 0.5f;
    }
    if (g_cfg.mm_flip_x) u = 1.0f - u;
    if (g_cfg.mm_flip_y) v = 1.0f - v;
    *mx = g_cfg.mm_x + u * g_cfg.mm_w;
    *my = g_cfg.mm_y + (1.0f - v) * g_cfg.mm_h;  // screen y grows downward
}

// Same technique the reference does: rather than asking the game to reveal
// anything, project world positions onto the minimap rectangle and draw our own
// markers. Touches no engine state, so it cannot crash the client, and it works
// regardless of fog, bushes or the game's vision logic.
void DrawMinimapMarkers() {
    if (!g_cfg.map_enabled || !g_cfg.map_minimap) return;
    if (game::ScreenWidth() <= 0) return;

    const game::FrameSnapshot& snap = game::Snapshot();
    const int local = snap.local_camp;
    if (local <= 0) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    auto marker = [&](const game::Entity& e, EspCat cat_id, bool is_self) {
        if (!e.obj || e.is_dead) return;

        float mx = 0, my = 0;
        WorldToMinimap(e.x, e.z, &mx, &my);

        // Cull to the minimap rectangle plus a margin: a stale or garbage
        // position must not smear markers across the rest of the screen.
        const float m = 48.0f;
        if (mx < g_cfg.mm_x - m || mx > g_cfg.mm_x + g_cfg.mm_w + m) return;
        if (my < g_cfg.mm_y - m || my > g_cfg.mm_y + g_cfg.mm_h + m) return;

        const float hp_pct = (e.hp_max > 0.0f) ? (e.hp / e.hp_max) : 1.0f;
        const float r = g_cfg.mm_dot;
        const ImU32 col = is_self ? Col(g_cfg.cat[CAT_SELF].color)
                                  : Col(g_cfg.cat[cat_id].color);

        const ImTextureID tex = (g_cfg.map_icons && e.id > 0 && !e.is_soldier)
                                    ? icons::IconFor(e.id) : (ImTextureID)0;
        const float hs = g_cfg.mm_icon_size * 0.5f;

        if (tex) {
            // Drop shadow, dark plate, portrait, then a health-tinted ring so
            // the portrait still carries the information the dot did. The
            // shadow is what makes the plate read over bright terrain tiles.
            dl->AddCircleFilled(ImVec2(mx + 1.5f, my + 1.5f), hs + 2.0f,
                                IM_COL32(0, 0, 0, 110), 24);
            dl->AddCircleFilled(ImVec2(mx, my), hs + 1.5f, IM_COL32(0, 0, 0, 200), 24);
            dl->AddImageRounded(tex, ImVec2(mx - hs, my - hs), ImVec2(mx + hs, my + hs),
                                ImVec2(0, 0), ImVec2(1, 1),
                                is_self ? IM_COL32(190, 220, 255, 255) : IM_COL32_WHITE,
                                hs, 0);
            if (g_cfg.map_hp_ring)
                dl->AddCircle(ImVec2(mx, my), hs + 0.5f, HealthColor(hp_pct, 235), 24,
                              g_cfg.mm_ring);
        } else {
            // Ring + core so the marker reads over both light and dark tiles.
            dl->AddCircleFilled(ImVec2(mx, my), r, IM_COL32(0, 0, 0, 170), 16);
            dl->AddCircleFilled(ImVec2(mx, my), r - 1.5f, col, 16);
            if (g_cfg.map_hp_ring)
                dl->AddCircle(ImVec2(mx, my), r + 0.5f, HealthColor(hp_pct, 210), 16,
                              g_cfg.mm_ring);
        }

        if (g_cfg.map_names && !is_self) {
            char nm[48];
            UnitLabel(e, cat_id, nm, sizeof(nm));
            const ImVec2 ts = ImGui::CalcTextSize(nm);
            TextShadowed(dl, ImVec2(mx - ts.x * 0.5f,
                                    my + (tex ? hs : r) + 2.0f),
                         IM_COL32(235, 240, 250, 235), nm);
        }
    };

    for (int i = 0; i < snap.n_players; i++) {
        const game::Entity& e = snap.players[i];
        if (!e.obj) continue;

        const bool is_self = snap.has_self && snap.self.guid != 0 &&
                             e.guid == snap.self.guid;
        if (is_self) {
            if (!g_cfg.map_debug_self) continue;
            marker(e, CAT_SELF, true);
            continue;
        }
        if (e.is_dead) continue;

        const bool enemy = (e.camp != local);
        // Soldiers are their own category: heroes only, unless minions are on.
        if (e.is_soldier) {
            if (!g_cfg.map_show_minions) continue;
        } else if (!enemy && !g_cfg.map_show_allies) {
            continue;
        }
        marker(e, PlayerCat(e, enemy), false);
    }

    if (g_cfg.map_show_monsters) {
        for (int i = 0; i < snap.n_monsters; i++) {
            const game::Entity& e = snap.monsters[i];
            if (!e.obj || e.is_dead) continue;
            const char* cat = game::MonsterCategory(e);
            if (!cat) continue;
            if (!strcmp(cat, "minion") && !g_cfg.map_show_minions) continue;
            marker(e, MonsterCat(cat), false);
        }
    }
}

// ---------------------------------------------------------------------------
// World ESP
// ---------------------------------------------------------------------------

struct DrawCtx {
    ImDrawList* dl = nullptr;
    float sw = 0, sh = 0;
    const game::Entity* self = nullptr;
};

// Every unit goes through here: the category decides what is drawn and in what
// colour, the caller only decides which category it is.
void DrawUnit(const DrawCtx& ctx, const game::Entity& e, EspCat cat_id,
              float world_h) {
    const CatCfg& cc = g_cfg.cat[cat_id];
    if (!cc.enabled) return;

    // Dead handling is per category (cc.dead_mode): 0 hide, 1 category colour,
    // 2 shared dead colour. A lane minion corpse is noise; an enemy hero's is
    // information — the row decides which.
    const bool dead = e.is_dead;
    if (dead && cc.dead_mode == 0) return;

    float sx = 0, sy = 0;
    if (!game::WorldToScreenEx(e.x, e.y, e.z, e.pos_is_unity, &sx, &sy)) return;

    float dist = 0.0f;
    if (ctx.self) {
        const float dx = e.x - ctx.self->x, dy = e.y - ctx.self->y,
                    dz = e.z - ctx.self->z;
        dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    const ImU32 col = (dead && cc.dead_mode == 2) ? Col(g_cfg.col_dead)
                                                  : Col(cc.color);

    // Everything below is anchored to the sprite, so cull it when the entity is
    // well outside the viewport.
    if (sx < -200 || sx > ctx.sw + 200 || sy < -200 || sy > ctx.sh + 200) return;

    float hx = 0, hy = 0;
    const float top = e.y + world_h;
    const bool have_head = game::WorldToScreenEx(e.x, top, e.z, e.pos_is_unity,
                                                 &hx, &hy);

    float box_h = have_head ? std::fabs(sy - hy) : 60.0f;
    float box_w = box_h * (e.is_soldier ? 0.9f : 0.52f);
    if (e.radius > 0.01f && !e.is_soldier) box_w *= (0.9f + e.radius * 0.25f);
    box_w *= g_cfg.esp_box_scale;
    box_h *= g_cfg.esp_box_scale;
    if (box_h < 8.0f) box_h = 8.0f;
    // A single unit can never be taller than the screen: a larger span means
    // one of the two projected points landed near the near plane (projection
    // explosion). Skip rather than draw a screen-filling rectangle.
    if (box_h > ctx.sh * 2.0f) return;

#if RAIN_ESP_DIAG
    // Calibration line (ESP-offset hunt): position source, which projector
    // produced each point, both raw projections, and the camera state from both
    // sources. Heroes are sampled every 300 draws; head-projection failures
    // are sampled separately because they change the box height fallback.
    {
        const bool hero_cat = (cat_id == CAT_SELF || cat_id == CAT_ALLY_HERO ||
                               cat_id == CAT_ENEMY_HERO);
        static int diag = 0;
        static int headfail = 0;
        const bool sample = hero_cat && (++diag % 300 == 0);
        const bool headfail_sample = !have_head && (++headfail % 240 == 0);
        if (sample || headfail_sample) {
            char cam[256];
            game::DiagCameraLine(cam, sizeof(cam));
            char selfbuf[80];
            if (ctx.self)
                snprintf(selfbuf, sizeof(selfbuf), "self=(%.1f,%.1f,%.1f)u%d",
                         ctx.self->x, ctx.self->y, ctx.self->z,
                         ctx.self->pos_is_unity ? 1 : 0);
            else
                snprintf(selfbuf, sizeof(selfbuf), "self=none");
            LOGI("espdiag cat=%d id=%d guid=%u u%d proj=%d pos=(%.2f,%.2f,%.2f) "
                 "feet=(%.0f,%.0f) headok=%d head=(%.0f,%.0f) h=%.1f dist=%.1f %s | %s",
                 static_cast<int>(cat_id), e.id, e.guid, e.pos_is_unity ? 1 : 0,
                 game::LastProjector(), e.x, e.y, e.z, sx, sy,
                 have_head ? 1 : 0, hx, hy, box_h, dist, selfbuf, cam);
        }
    }
#endif

    const float x0 = sx - box_w * 0.5f;
    const float y0 = have_head ? (hy < sy ? hy : sy) : sy - box_h;
    const float x1 = sx + box_w * 0.5f;
    const float y1 = y0 + box_h;

    // Tracer: from the chosen origin to the entity. The endpoint anchors to
    // the box (feet / body centre / head) so the line visibly terminates on
    // the target — a line to the feet reads as pointing at the ground under a
    // tall hero. Drawn before the box so the box sits on top of the line.
    if (cc.line) {
        float ox = 0, oy = 0;
        if (g_cfg.esp_line_origin_mode == 1 && ctx.self) {
            float px = 0, py = 0;
            if (game::WorldToScreenEx(ctx.self->x, ctx.self->y, ctx.self->z,
                                      ctx.self->pos_is_unity, &px, &py)) {
                ox = px; oy = py;
            } else {
                ox = ctx.sw * 0.5f; oy = ctx.sh * 0.5f;
            }
        } else {
            ox = ctx.sw * g_cfg.esp_line_origin;
            oy = (g_cfg.esp_line_vpos == 1) ? ctx.sh : ctx.sh * 0.5f;
        }

        float ex = sx, ey = sy;
        if (have_head) {
            if (g_cfg.esp_line_anchor == 1) ey = (sy + hy) * 0.5f;  // body centre
            else if (g_cfg.esp_line_anchor == 2) ey = hy;           // head
        }
        // Clamp to the viewport so an off-screen target still gets a line
        // pointing at it rather than one that runs off the edge.
        const float m = 10.0f;
        if (ex < m) ex = m;
        if (ex > ctx.sw - m) ex = ctx.sw - m;
        if (ey < m) ey = m;
        if (ey > ctx.sh - m) ey = ctx.sh - m;
        ctx.dl->AddLine(ImVec2(ox, oy), ImVec2(ex, ey), col,
                        static_cast<float>(g_cfg.esp_thickness));
    }

    if (cc.box) {
        DrawBox(ctx.dl, x0, y0, x1, y1, col, g_cfg.esp_box_style,
                g_cfg.esp_glow_layers, static_cast<float>(g_cfg.esp_thickness));
    }

    if (cc.health && e.hp_max > 0.0f) {
        float pct = e.hp / e.hp_max;
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 1.0f) pct = 1.0f;
        DrawHealthBar(ctx.dl, g_cfg.esp_health_pos, x0, y0, x1, y1, pct);

        if (cc.health_text) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%.0f", e.hp);
            const ImVec2 ts = ImGui::CalcTextSize(buf);
            TextShadowed(ctx.dl, ImVec2(sx - ts.x * 0.5f, y1 + 4.0f),
                         IM_COL32(255, 255, 255, 230), buf);
        }
    }

    // Tags stack outward from the box edge.
    float text_y = (g_cfg.esp_name_pos == 0) ? y0 - 22.0f : y1 + 6.0f;
    const float step = (g_cfg.esp_name_pos == 0) ? -20.0f : 20.0f;

    // Portrait sits beside the box, from the drop-in icon dir.
    if (cc.icon && e.id > 0 && !e.is_soldier) {
        const ImTextureID tex = icons::IconFor(e.id);
        if (tex) {
            constexpr float icon_size = 34.0f;
            ctx.dl->AddImage(tex, ImVec2(x0 - icon_size - 3.0f, y0),
                             ImVec2(x0 - 3.0f, y0 + icon_size));
        }
    }

    if (cc.hero_name) {
        char buf[64];
        UnitLabel(e, cat_id, buf, sizeof(buf));
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        TextShadowed(ctx.dl, ImVec2(sx - ts.x * 0.5f, text_y), col, buf);
        text_y += step;
    }

    // Level tag is a player concept — entity categories carry a display label
    // instead, and a jungle creep has no level worth printing.
    if (cc.name && cat_id < CAT_TURTLE && !e.is_soldier) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Lv.%d%s", e.level, e.is_fake ? " (clone)" : "");
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        TextShadowed(ctx.dl, ImVec2(sx - ts.x * 0.5f, text_y),
                     IM_COL32(255, 255, 255, 235), buf);
        text_y += step;
    }

    if (cc.distance && ctx.self) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0fm", dist);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        TextShadowed(ctx.dl, ImVec2(sx - ts.x * 0.5f, text_y),
                     IM_COL32(220, 230, 245, 235), buf);
        text_y += step;
    }

    // Spell cooldowns from the entity's own cooldown component. Only spells
    // currently on cooldown draw; entries whose remaining time resolves as a
    // wall-clock stamp are rejected inside the snapshot until calibrated.
    if (cc.spell_cd) {
        game::SpellCD cds[4];
        const int ncd = game::SnapshotSpellCDs(e, cds, 4);
        float cd_y = (g_cfg.esp_name_pos == 0) ? y1 + 4.0f : y0 - 22.0f;
        for (int c = 0; c < ncd; c++) {
            if (!cds[c].active || cds[c].remaining <= 0.0f) continue;
            char buf[32];
            snprintf(buf, sizeof(buf), "CD %.1fs", cds[c].remaining);
            const ImVec2 ts = ImGui::CalcTextSize(buf);
            TextShadowed(ctx.dl, ImVec2(sx - ts.x * 0.5f, cd_y),
                         IM_COL32(150, 200, 255, 235), buf);
            cd_y += 18.0f;
        }
    }

    if (g_cfg.esp_indicators && (dead || e.dead_sight != 0 || !e.minimap_visible)) {
        char buf[64];
        buf[0] = '\0';
        if (dead) snprintf(buf, sizeof(buf), "DEAD");
        else if (e.dead_sight != 0) snprintf(buf, sizeof(buf), "SIGHT");
        else if (!e.minimap_visible) snprintf(buf, sizeof(buf), "HIDDEN");
        if (buf[0]) {
            const ImVec2 ts = ImGui::CalcTextSize(buf);
            const float iy = (g_cfg.esp_name_pos == 0) ? y1 + 4.0f : y0 - 26.0f;
            TextShadowed(ctx.dl, ImVec2(sx - ts.x * 0.5f, iy),
                         IM_COL32(255, 210, 80, 235), buf);
        }
    }
}

// Player entities: self, allies, enemies, and both teams' lane minions, all
// from the same list and all classified by camp.
void DrawPlayers(const DrawCtx& ctx) {
    if (!g_cfg.esp_enabled) return;
    if (game::ScreenWidth() <= 0 || game::ScreenHeight() <= 0) return;

    const game::FrameSnapshot& snap = game::Snapshot();
    const int n = snap.n_players;
    if (n <= 0) return;
    const int local = snap.local_camp;
    if (local <= 0) return;   // no camp: nothing can be classified safely

    const float global_max = g_cfg.esp_max_distance;

    for (int i = 0; i < n; i++) {
        const game::Entity& e = snap.players[i];
        if (!e.obj) continue;

        const bool is_self = snap.has_self && snap.self.guid != 0 &&
                             e.guid == snap.self.guid;
        if (is_self) {
            DrawUnit(ctx, e, CAT_SELF, UnitHeight(e));
            continue;
        }

        const bool enemy = (e.camp != local);
        const EspCat cat_id = PlayerCat(e, enemy);
        const CatCfg& cc = g_cfg.cat[cat_id];

        // The per-category cap wins when set; otherwise the global player cap.
        // Filtering happens here, before DrawUnit, so a filtered entity never
        // leaves a tracer line pointing at it.
        const float cap = (cc.max_dist > 0.0f) ? cc.max_dist : global_max;
        if (cap > 0.0f && ctx.self) {
            const float dx = e.x - ctx.self->x, dy = e.y - ctx.self->y,
                        dz = e.z - ctx.self->z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > cap) continue;
        }
        DrawUnit(ctx, e, cat_id, UnitHeight(e));
    }

    static int dbg = 0;
    if (++dbg % 180 == 1) {
        char on[CAT_COUNT + 1];
        for (int c = 0; c < CAT_COUNT; c++) on[c] = g_cfg.cat[c].enabled ? '1' : '0';
        on[CAT_COUNT] = '\0';
        float dx = 0, dy = 0;
        const bool ok = game::WorldToScreenEx(snap.players[0].x, snap.players[0].y,
                                              snap.players[0].z,
                                              snap.players[0].pos_is_unity, &dx, &dy);
        LOGI("esp: n=%d cats=%s local=%d w2s=%d scr=(%.0f,%.0f)", n, on, local, ok, dx, dy);
    }
}

// Jungle and camp entities, from BattleManager.m_ShowMonsters.
void DrawEntities(const DrawCtx& ctx) {
    if (!g_cfg.esp_enabled) return;
    if (game::ScreenWidth() <= 0 || game::ScreenHeight() <= 0) return;

    const game::FrameSnapshot& snap = game::Snapshot();
    const int n = snap.n_monsters;
    if (n <= 0) return;

    const float global_max = g_cfg.ent_max_distance;

    for (int i = 0; i < n; i++) {
        const game::Entity& e = snap.monsters[i];
        const EspCat id = MonsterCat(game::MonsterCategory(e));
        if (id == CAT_COUNT) continue;

        const CatCfg& cc = g_cfg.cat[id];
        const float cap = (cc.max_dist > 0.0f) ? cc.max_dist : global_max;
        if (cap > 0.0f && ctx.self) {
            const float dx = e.x - ctx.self->x, dy = e.y - ctx.self->y,
                        dz = e.z - ctx.self->z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > cap) continue;
        }
        DrawUnit(ctx, e, id, EntityHeight(id));
    }
}

// Range / FOV circle — a ground ring around the local hero at the active
// targeting range (VYNIX's "Range FOV" readout). The ring is sampled in world
// space and projected point-by-point, so it lies flat on the map and scales
// correctly with camera pitch. Draw-only: no engine state is touched.
void DrawRangeCircle(const DrawCtx& ctx) {
    if (!g_cfg.range_circle || !ctx.self) return;
    if (game::ScreenWidth() <= 0) return;

    // Which range to show: the fixed slider, or the first armed feature's.
    // Auto order matches the combat tab: basic aim, then skill aim, then Kimmy.
    float range = g_cfg.range_circle_fixed;
    if (g_cfg.range_circle_source == 0) {
        if (g_cfg.aim_enabled && g_cfg.aim_range > 0.0f) {
            range = g_cfg.aim_range;
        } else {
            bool found = false;
            for (int i = 0; i < SLOT_COUNT && !found; i++) {
                const SkillCfg& sk = g_cfg.skill[i];
                if (sk.enabled && sk.range > 0.0f) {
                    range = sk.range;
                    found = true;
                }
            }
            if (!found && g_cfg.kimmy_enabled && g_cfg.kimmy_range > 0.0f)
                range = g_cfg.kimmy_range;
        }
    }
    if (range <= 0.0f) return;

    const float cx = ctx.self->x, cy = ctx.self->y, cz = ctx.self->z;
    const ImU32 col = Col(g_cfg.range_circle_color);
    const float th = g_cfg.range_circle_thickness;

    // 48 segments: smooth at any practical screen size, and cheap — 48 world
    // projections per frame, each a matrix multiply.
    constexpr int kSegments = 48;
    float px[kSegments], py[kSegments];
    bool ok[kSegments];
    for (int i = 0; i < kSegments; i++) {
        const float a = (2.0f * 3.14159265358979f * i) / kSegments;
        const float wx = cx + range * cosf(a);
        const float wz = cz + range * sinf(a);
        ok[i] = game::WorldToScreenEx(wx, cy, wz, ctx.self->pos_is_unity,
                                      &px[i], &py[i]);
    }
    for (int i = 0; i < kSegments; i++) {
        const int j = (i + 1) % kSegments;
        if (!ok[i] || !ok[j]) continue;   // a segment behind the camera is skipped
        ctx.dl->AddLine(ImVec2(px[i], py[i]), ImVec2(px[j], py[j]), col, th);
    }
}

// Objective alerts — banners triggered when a jungle objective's HP drops
// recently. A standalone feature: independent of ESP and the minimap, it draws
// whenever the alert master is on, so the call is made without opening the
// menu or even seeing the pit. Each objective has its own toggle.
void DrawObjectiveAlerts() {
    if (!g_cfg.alerts_enabled) return;
    if (game::ScreenWidth() <= 0) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float sw = static_cast<float>(game::ScreenWidth());

    struct Alert { bool active; const char* text; ImU32 col; };
    const Alert alerts[] = {
        {g_cfg.alert_turtle && game::TurtleContested(),
         "TURTLE UNDER ATTACK", Col(g_cfg.cat[CAT_TURTLE].color)},
        {g_cfg.alert_lord && game::LordContested(),
         "LORD UNDER ATTACK", Col(g_cfg.cat[CAT_LORD].color)},
        {g_cfg.alert_blue_buff && game::BlueBuffContested(),
         "BLUE BUFF UNDER ATTACK", Col(g_cfg.cat[CAT_BLUE_BUFF].color)},
        {g_cfg.alert_red_buff && game::RedBuffContested(),
         "RED BUFF UNDER ATTACK", Col(g_cfg.cat[CAT_RED_BUFF].color)},
        {g_cfg.alert_litho && game::LithoContested(),
         "LITHO UNDER ATTACK", Col(g_cfg.cat[CAT_LITHO].color)},
    };

    // Text scale: multiply the font size so the banner reads at a glance. The
    // ImGui font is fixed-size, so the size is applied per draw call.
    const float scale = (g_cfg.alert_scale < 0.5f) ? 0.5f
                      : (g_cfg.alert_scale > 2.5f) ? 2.5f : g_cfg.alert_scale;
    const float fs = ImGui::GetFontSize() * scale;

    float y = 84.0f;
    for (const auto& a : alerts) {
        if (!a.active) continue;
        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, a.text);
        const float x = sw * 0.5f - ts.x * 0.5f;

        // Pulsing alpha so the banner reads as urgent without being loud.
        const float pulse = 0.75f + 0.25f * sinf(static_cast<float>(ImGui::GetTime()) * 6.0f);
        const ImU32 bg = IM_COL32(20, 10, 10, static_cast<unsigned char>(170 * pulse));
        const float pad = 12.0f * scale;
        dl->AddRectFilled(ImVec2(x - pad, y - 5.0f * scale),
                          ImVec2(x + ts.x + pad, y + ts.y + 5.0f * scale),
                          bg, 7.0f * scale);

        ImU32 tc = a.col;
        const unsigned char ta = static_cast<unsigned char>(255 * pulse);
        tc = (tc & ~IM_COL32_A_MASK) | (static_cast<ImU32>(ta) << IM_COL32_A_SHIFT);
        dl->AddText(ImGui::GetFont(), fs, ImVec2(x + 1.5f, y + 1.5f),
                    IM_COL32(0, 0, 0, 200), a.text);
        dl->AddText(ImGui::GetFont(), fs, ImVec2(x, y), tc, a.text);
        y += ts.y + 16.0f * scale;
    }
}

}  // namespace

void Draw() {
    DrawCtx ctx;
    ctx.dl = ImGui::GetForegroundDrawList();
    ctx.sw = static_cast<float>(game::ScreenWidth());
    ctx.sh = static_cast<float>(game::ScreenHeight());
    const game::FrameSnapshot& snap = game::Snapshot();
    ctx.self = snap.has_self ? &snap.self : nullptr;

    // The minimap and objective banners are overlay features: they draw even
    // when world ESP is switched off entirely.
    DrawMinimapMarkers();
    DrawRangeCircle(ctx);
    DrawEntities(ctx);
    DrawPlayers(ctx);
    DrawObjectiveAlerts();
}

}  // namespace esp
