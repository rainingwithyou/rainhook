#include "menu.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <android/log.h>

#include <imgui.h>

#include "autox.h"
#include "account.h"
#include "cfgjson.h"
#include "config.h"
#include "game.h"
#include "input.h"
#include "overlay.h"
#include "persist.h"
#include "target.h"

#define LOG_TAG "Rain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace menu {

namespace {

// ---------------------------------------------------------------------------
// shared bits
// ---------------------------------------------------------------------------

const char* kCatNames[CAT_COUNT] = {
    "Self", "Ally Hero", "Enemy Hero", "Ally Minion", "Enemy Minion",
    "Turtle", "Lord", "Buff", "Creep",
};
const char* kCatHints[CAT_COUNT] = {
    "you", "teammate", "hostile hero",
    "friendly wave", "hostile wave",
    "objective", "objective", "jungle buff", "jungle camp",
};

enum Tab { TAB_VISUAL = 0, TAB_COMBAT, TAB_MAP, TAB_STATUS, TAB_SETTINGS, TAB_COUNT };
const char* kTabNames[TAB_COUNT] = {"Visual", "Combat", "Map", "Status", "Settings"};

const char* kSlotNames[SLOT_COUNT] = {"Skill 1", "Skill 2", "Skill 3", "Basic", "Spell"};
const char* kComboSlotNames[4] = {"S1", "S2", "S3", "Basic"};

void ColorEdit(const char* id, float c[4]) {
    ImGui::ColorEdit4(id, c, ImGuiColorEditFlags_NoInputs |
                            ImGuiColorEditFlags_AlphaBar |
                            ImGuiColorEditFlags_NoLabel |
                            ImGuiColorEditFlags_NoDragDrop);
}

void ModeCombo(const char* id, int* v) {
    const char* items[] = {"Players Only", "Monsters Only", "Both (Adaptive)"};
    ImGui::Combo(id, v, items, IM_ARRAYSIZE(items));
}

void PriorityCombo(const char* id, int* v) {
    const char* items[] = {"Closest Distance", "Lowest HP", "Lowest HP %",
                           "Most Kills", "Richest", "Most Fed (bounty)",
                           "Closest to crosshair"};
    ImGui::Combo(id, v, items, IM_ARRAYSIZE(items));
}

// Ordered class preference. Swap-only rather than free combos, because
// duplicate entries would make the ranking ambiguous. Six rows now that the
// monster classes are split out of the single "Monster" bucket.
void ClassOrderEditor() {
    const char* names[] = {"Minion", "Hero", "Turtle", "Lord", "Buff", "Creep"};
    constexpr int kCount = 6;
    ImGui::TextUnformatted("Target class priority");
    ImGui::BeginDisabled(false);
    for (int i = 0; i < kCount; i++) {
        ImGui::PushID(1000 + i);
        const int v = g_cfg.target_order[i];
        ImGui::Text("%d. %-8s", i + 1, (v >= 0 && v < kCount) ? names[v] : "?");
        ImGui::SameLine();
        if (ImGui::SmallButton("Up") && i > 0) {
            const int t = g_cfg.target_order[i];
            g_cfg.target_order[i] = g_cfg.target_order[i - 1];
            g_cfg.target_order[i - 1] = t;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Down") && i < kCount - 1) {
            const int t = g_cfg.target_order[i];
            g_cfg.target_order[i] = g_cfg.target_order[i + 1];
            g_cfg.target_order[i + 1] = t;
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Visual tab
// ---------------------------------------------------------------------------

void SectionEspMaster() {
    if (!ImGui::CollapsingHeader("ESP", ImGuiTreeNodeFlags_DefaultOpen)) return;

    ImGui::Checkbox("Enable ESP##master", &g_cfg.esp_enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Status markers", &g_cfg.esp_indicators);

    // Dead handling is per category now (each row has its own mode); this
    // swatch is the shared colour that mode 2 uses.
    ImGui::TextUnformatted("Dead entities");
    ImGui::SameLine();
    ColorEdit("##col_dead", g_cfg.col_dead);
    ImGui::SameLine();
    ImGui::TextDisabled("shared dead colour (per-row mode below)");

    ImGui::SeparatorText("Box");
    ImGui::Checkbox("Box##on", &g_cfg.cat[CAT_ENEMY_HERO].box);
    // The per-category rows carry the real box toggle; this mirror exists so
    // the control is where people look for it.
    ImGui::SameLine();
    ImGui::RadioButton("2D", &g_cfg.esp_box_style, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Outline", &g_cfg.esp_box_style, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Glow", &g_cfg.esp_box_style, 2);
    if (g_cfg.esp_box_style == 2)
        ImGui::SliderInt("Glow layers", &g_cfg.esp_glow_layers, 1, 8);
    ImGui::SliderFloat("Box scale", &g_cfg.esp_box_scale, 0.4f, 2.5f, "%.2f");
    ImGui::SliderInt("Thickness", &g_cfg.esp_thickness, 1, 6);

    ImGui::SeparatorText("Health bar");
    ImGui::RadioButton("Left##hp", &g_cfg.esp_health_pos, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Right##hp", &g_cfg.esp_health_pos, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Up##hp", &g_cfg.esp_health_pos, 2);
    ImGui::SameLine();
    ImGui::RadioButton("Down##hp", &g_cfg.esp_health_pos, 3);

    ImGui::SeparatorText("Nametag");
    ImGui::RadioButton("Above##name", &g_cfg.esp_name_pos, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Below##name", &g_cfg.esp_name_pos, 1);

    ImGui::SeparatorText("Tracer");
    ImGui::RadioButton("From camera", &g_cfg.esp_line_origin_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("From player", &g_cfg.esp_line_origin_mode, 1);
    if (g_cfg.esp_line_origin_mode == 0) {
        ImGui::SliderFloat("Horizontal origin", &g_cfg.esp_line_origin, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::RadioButton("Center", &g_cfg.esp_line_vpos, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Bottom", &g_cfg.esp_line_vpos, 1);
    }

    ImGui::SeparatorText("Distance cap");
    ImGui::SliderFloat("Players##dist", &g_cfg.esp_max_distance, 0.0f, 2000.0f,
                       g_cfg.esp_max_distance <= 0.0f ? "unlimited" : "%.0f");
    ImGui::SliderFloat("Entities##dist", &g_cfg.ent_max_distance, 0.0f, 2000.0f,
                       g_cfg.ent_max_distance <= 0.0f ? "unlimited" : "%.0f");
    ImGui::TextDisabled("each category can override these (see its row)");

    ImGui::SeparatorText("Range circle");
    ImGui::Checkbox("Show range circle", &g_cfg.range_circle);
    if (g_cfg.range_circle) {
        ImGui::RadioButton("Auto (armed feature)", &g_cfg.range_circle_source, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Fixed", &g_cfg.range_circle_source, 1);
        if (g_cfg.range_circle_source == 1)
            ImGui::SliderFloat("Radius##rc", &g_cfg.range_circle_fixed, 1.0f, 40.0f, "%.1f");
        ImGui::SliderFloat("Thickness##rc", &g_cfg.range_circle_thickness, 1.0f, 6.0f, "%.1f");
        ImGui::SameLine();
        ColorEdit("##rc_col", g_cfg.range_circle_color);
    }
}

void CategoryRow(EspCat id) {
    CatCfg& cc = g_cfg.cat[id];
    ImGui::PushID(static_cast<int>(id));

    ImGui::Checkbox("##on", &cc.enabled);
    ImGui::SameLine();
    ColorEdit("##col", cc.color);
    ImGui::SameLine();

    const bool is_player = id < CAT_TURTLE;
    const bool open = ImGui::TreeNodeEx("##row", ImGuiTreeNodeFlags_SpanAvailWidth,
                                        "%s  -  %s", kCatNames[id], kCatHints[id]);
    if (open) {
        ImGui::Checkbox("Box", &cc.box);
        ImGui::SameLine();
        ImGui::Checkbox("Health bar", &cc.health);
        ImGui::SameLine();
        ImGui::Checkbox("HP value", &cc.health_text);

        ImGui::Checkbox("Display name", &cc.hero_name);
        ImGui::SameLine();
        ImGui::Checkbox("Distance", &cc.distance);
        if (is_player) {
            ImGui::SameLine();
            ImGui::Checkbox("Level tag", &cc.name);
        }

        ImGui::Checkbox("Tracer", &cc.line);
        ImGui::SameLine();
        ImGui::Checkbox("Icon", &cc.icon);
        ImGui::SameLine();
        ImGui::Checkbox("Spell CD", &cc.spell_cd);

        // Per-category dead handling: a corpse is information for a hero and
        // noise for a minion, so each row decides instead of one global flag.
        ImGui::SeparatorText("When dead");
        ImGui::RadioButton("Hide", &cc.dead_mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Category colour", &cc.dead_mode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Dead colour", &cc.dead_mode, 2);

        // Per-category distance cap, overriding the global one when set.
        ImGui::SliderFloat("Max distance", &cc.max_dist, 0.0f, 2000.0f,
                           cc.max_dist <= 0.0f ? "global" : "%.0f");
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void SectionCategories() {
    if (!ImGui::CollapsingHeader("ESP Categories", ImGuiTreeNodeFlags_DefaultOpen)) return;

    ImGui::TextWrapped(
        "Each row is one entity class: whether it draws, its colour, and which "
        "elements it shows. Box shape, scale, thickness, health placement and "
        "the tracer origin are shared and live under ESP above.");

    if (ImGui::SmallButton("All on")) {
        for (int i = 0; i < CAT_COUNT; i++) g_cfg.cat[i].enabled = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Enemies only")) {
        for (int i = 0; i < CAT_COUNT; i++)
            g_cfg.cat[i].enabled = (i == CAT_ENEMY_HERO || i == CAT_ENEMY_MINION ||
                                    i == CAT_TURTLE || i == CAT_LORD);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("All off")) {
        for (int i = 0; i < CAT_COUNT; i++) g_cfg.cat[i].enabled = false;
    }
    ImGui::Separator();

    for (int i = 0; i < CAT_COUNT; i++) CategoryRow(static_cast<EspCat>(i));
}

void SectionAlerts() {
    if (!ImGui::CollapsingHeader("Objective Alerts")) return;
    ImGui::Checkbox("Enable##alerts", &g_cfg.alerts_enabled);
    ImGui::BeginDisabled(!g_cfg.alerts_enabled);
    ImGui::Checkbox("Turtle under attack", &g_cfg.alert_turtle);
    ImGui::SameLine();
    ImGui::Checkbox("Lord under attack", &g_cfg.alert_lord);
    ImGui::EndDisabled();
    ImGui::TextWrapped(
        "Banner fires when the objective's HP drops and stays up for a few "
        "seconds after the last hit.");
}

void SectionDrone() {
    if (!ImGui::CollapsingHeader("Drone View")) return;

    ImGui::Checkbox("Drone View##toggle", &g_cfg.drone_enabled);
    ImGui::BeginDisabled(!g_cfg.drone_enabled);
    ImGui::SliderFloat("Camera High", &g_cfg.drone_height, 0.0f, 40.0f, "%.1f");
    ImGui::SliderFloat("Zoom Out (FOV)", &g_cfg.drone_fov, 0.0f, 40.0f, "%.0f deg");
    ImGui::Checkbox("Hook follow setter (VYNIX model)", &g_cfg.drone_hook);
    ImGui::EndDisabled();

    // Hook state readout: the drone degrades (hook -> field write -> transform
    // lift) rather than failing, so it matters which path is live.
    ImGui::Text("Hook: %s", game::DroneHookState());
    if (g_cfg.drone_enabled && g_cfg.drone_hook &&
        strcmp(game::DroneHookState(), "live") != 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(falling back to field write)");
    }

    ImGui::TextWrapped(
        "Camera High modifies the follow offset the camera controller reads. "
        "With the hook on, Rain intercepts the game's own setter and adjusts the "
        "value on its way in — the controller carries the change itself, so it "
        "cannot fight or flicker. Zoom Out widens the field of view. Both stock "
        "values are restored when the drone is switched off.");
}

void SectionSkin() {
    if (!ImGui::CollapsingHeader("Skin Changer")) return;

    ImGui::Checkbox("Enable##skin", &g_cfg.skin_enabled);
    ImGui::BeginDisabled(!g_cfg.skin_enabled);
    ImGui::InputInt("Skin ID", &g_cfg.skin_id);
    if (g_cfg.skin_id < 0) g_cfg.skin_id = 0;
    ImGui::SameLine();
    if (ImGui::Button("Apply to my hero")) {
        const game::FrameSnapshot& s = game::Snapshot();
        if (s.has_self && game::SetShowSkin(s.self.obj, g_cfg.skin_id))
            g_cfg.skin_applied = true;
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped(
        "Writes the client-side skin id on your own entity. Takes effect when "
        "the game re-renders the model; a skin id that is not downloaded "
        "locally renders as the default skin.");
}

void TabVisual() {
    SectionEspMaster();
    SectionCategories();
    SectionAlerts();
    SectionDrone();
    SectionSkin();
}

// ---------------------------------------------------------------------------
// Combat tab
// ---------------------------------------------------------------------------

void SectionTargeting() {
    if (!ImGui::CollapsingHeader("Targeting", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextWrapped(
        "Shared by every feature below. Classes are tried in the order listed: "
        "the first class with a valid target wins, then the priority picks "
        "within it. Monster classes are split, so \"Lord first, then heroes, "
        "then creeps\" is a list you can set.");
    ClassOrderEditor();
    ImGui::TextDisabled(
        "Kills / Richest / Most Fed read the player companion object; targets "
        "with no data there fall back to distance ordering.");
}

void SectionAim() {
    if (!ImGui::CollapsingHeader("Basic-Attack Assist")) return;

    ImGui::Checkbox("Enable##aim", &g_cfg.aim_enabled);
    ImGui::BeginDisabled(!g_cfg.aim_enabled);
    ImGui::SetNextItemWidth(180);
    ModeCombo("Target mode##aim", &g_cfg.aim_mode);
    ImGui::SetNextItemWidth(180);
    PriorityCombo("Priority##aim", &g_cfg.aim_priority);
    ImGui::SliderFloat("Range##aim", &g_cfg.aim_range, 1.0f, 40.0f, "%.0f");
    ImGui::Text("Attack button: %.0f, %.0f  (calibrate below)", g_cfg.aim_btn_x,
                g_cfg.aim_btn_y);
    ImGui::EndDisabled();
}

void SectionSkillAim() {
    if (!ImGui::CollapsingHeader("Skill Auto AIM")) return;

    ImGui::TextWrapped(
        "Per-slot. Smart-cast taps the button and lets the game pick the "
        "direction; drag-aim presses the button and drags toward the target's "
        "screen position, which is true directional aiming.");

    for (int i = 0; i < SLOT_COUNT; i++) {
        SkillCfg& sk = g_cfg.skill[i];
        ImGui::PushID(2000 + i);
        ImGui::Checkbox(kSlotNames[i], &sk.enabled);
        ImGui::SameLine();
        ImGui::TextDisabled("(%.0f, %.0f)", sk.btn_x, sk.btn_y);

        if (ImGui::TreeNode("##cfg", "settings")) {
            ImGui::RadioButton("Smart-cast", &sk.aim_mode, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Drag-aim", &sk.aim_mode, 1);
            if (sk.aim_mode == 1)
                ImGui::SliderFloat("Drag length", &sk.drag_len, 40.0f, 260.0f, "%.0f px");

            ImGui::SetNextItemWidth(180);
            ModeCombo("Target mode##sk", &sk.target_mode);
            ImGui::SetNextItemWidth(180);
            PriorityCombo("Priority##sk", &sk.priority);
            ImGui::SliderFloat("Range##sk", &sk.range, 1.0f, 40.0f, "%.0f");
            ImGui::Checkbox("Skip while on cooldown", &sk.cd_check);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void SectionKimmy() {
    if (!ImGui::CollapsingHeader("Kimmy M1 Aura")) return;

    // Live state first: this is the thing the player glances at.
    {
        const game::FrameSnapshot& s = game::Snapshot();
        const bool on_kimmy = s.has_self && s.self.id == 71;
        ImGui::TextColored(g_cfg.kimmy_enabled ? ImVec4(0.45f, 0.95f, 0.55f, 1)
                                               : ImVec4(0.6f, 0.62f, 0.68f, 1),
                           "%s", g_cfg.kimmy_enabled ? "ARMED" : "disarmed");
        ImGui::SameLine();
        ImGui::TextDisabled("| hero %s", on_kimmy ? "is Kimmy" : "is not Kimmy");
    }

    ImGui::Checkbox("Arm##kimmy", &g_cfg.kimmy_enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Show KM button in match", &g_cfg.kimmy_km_button);

    ImGui::BeginDisabled(!g_cfg.kimmy_enabled);
    ImGui::Checkbox("Minion-clear mode (minion first)", &g_cfg.kimmy_minion_mode);
    ImGui::SliderFloat("Lock range", &g_cfg.kimmy_range, 2.0f, 40.0f, "%.0f");
    ImGui::SetNextItemWidth(180);
    PriorityCombo("Priority##kimmy", &g_cfg.kimmy_priority);

    ImGui::SeparatorText("Aim");
    ImGui::RadioButton("Hold only##km", &g_cfg.kimmy_aim_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Hold + steer##km", &g_cfg.kimmy_aim_mode, 1);
    if (g_cfg.kimmy_aim_mode == 1) {
        ImGui::SliderFloat("Steer length", &g_cfg.kimmy_drag_len, 40.0f, 300.0f, "%.0f px");
        ImGui::TextDisabled("drag direction points at the selected target");
    }

    if (g_cfg.kimmy_btn_x > 0.0f) {
        ImGui::Text("M1 anchor: %.0f, %.0f", g_cfg.kimmy_btn_x, g_cfg.kimmy_btn_y);
        ImGui::SameLine();
        if (ImGui::SmallButton("Re-anchor")) {
            // Clears the anchor so the next in-match tap in the action zone
            // becomes the new one. Needed when an anchor was captured on the
            // wrong screen (hero select and mode menus also have taps in the
            // bottom-right).
            g_cfg.kimmy_btn_x = 0.0f;
            g_cfg.kimmy_btn_y = 0.0f;
            autox::ReleaseAll();
            persist::MarkDirty();
        }
    } else {
        ImGui::TextDisabled("M1 anchor: tap the attack button in a match");
    }
    ImGui::EndDisabled();

    ImGui::TextWrapped(
        "Toggle with the floating KM button in a match — click to arm, drag to "
        "move it. While armed and a target is inside the lock range, Rain holds "
        "the basic-attack button down, so the game's own held-attack targeting "
        "keeps firing and auto-locks the nearest valid target. The M1 position "
        "is captured from your first tap on the attack button in a match. Only "
        "runs while your hero is Kimmy (id 71); releases on target loss, death, "
        "or menu open.");
}

void SectionCombo() {
    if (!ImGui::CollapsingHeader("Auto Combo")) return;

    ImGui::Checkbox("Enable##combo", &g_cfg.combo_enabled);
    ImGui::BeginDisabled(!g_cfg.combo_enabled);

    if (ImGui::Button("Gusion")) {
        // S2 -> S1 -> S3 -> S2 -> S1
        const int seq[5] = {1, 0, 2, 1, 0};
        memcpy(g_cfg.combo_seq, seq, sizeof(seq));
        g_cfg.combo_steps = 5;
        g_cfg.combo_preset = 1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Benedetta")) {
        // S2 -> S1 -> S3 -> BA -> BA
        const int seq[5] = {1, 0, 2, 3, 3};
        memcpy(g_cfg.combo_seq, seq, sizeof(seq));
        g_cfg.combo_steps = 5;
        g_cfg.combo_preset = 2;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_cfg.combo_preset == 1 ? "Gusion loaded"
                          : g_cfg.combo_preset == 2 ? "Benedetta loaded"
                                                    : "custom");

    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderInt("Steps", &g_cfg.combo_steps, 1, 5))
        g_cfg.combo_preset = 0;
    ImGui::SliderFloat("Trigger range##combo", &g_cfg.combo_range, 1.0f, 30.0f, "%.0f");
    ImGui::SliderInt("Tap delay (frames)", &g_cfg.combo_delay, 1, 40);

    ImGui::SeparatorText("Cast order");
    for (int i = 0; i < 5; i++) {
        ImGui::PushID(3000 + i);
        ImGui::Text("%d.", i + 1);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        if (ImGui::Combo("##slot", &g_cfg.combo_seq[i], kComboSlotNames, 4))
            g_cfg.combo_preset = 0;
        ImGui::PopID();
    }

    ImGui::SeparatorText("Source buttons");
    ImGui::Text("S1 %.0f,%.0f   S2 %.0f,%.0f", g_cfg.combo_s1_x, g_cfg.combo_s1_y,
                g_cfg.combo_s2_x, g_cfg.combo_s2_y);
    ImGui::Text("S3 %.0f,%.0f   BA %.0f,%.0f", g_cfg.combo_s3_x, g_cfg.combo_s3_y,
                g_cfg.combo_ba_x, g_cfg.combo_ba_y);
    ImGui::TextDisabled("Calibrate each source button below.");
    ImGui::EndDisabled();
}

void SectionRetri() {
    if (!ImGui::CollapsingHeader("Auto Retribution")) return;

    ImGui::Checkbox("Enable##retri", &g_cfg.retri_enabled);
    ImGui::BeginDisabled(!g_cfg.retri_enabled);

    ImGui::SeparatorText("Objectives");
    ImGui::Checkbox("Lord", &g_cfg.retri_lord);
    ImGui::SameLine();
    ImGui::Checkbox("Turtle", &g_cfg.retri_turtle);
    ImGui::SameLine();
    ImGui::Checkbox("Buffs", &g_cfg.retri_buff);
    ImGui::SameLine();
    ImGui::Checkbox("Litho", &g_cfg.retri_litho);

    ImGui::SeparatorText("Trigger");
    ImGui::RadioButton("Enemy nearby only", &g_cfg.retri_trigger, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Always active", &g_cfg.retri_trigger, 1);
    if (g_cfg.retri_trigger == 0)
        ImGui::SliderFloat("Enemy distance", &g_cfg.retri_enemy_dist, 0.0f, 30.0f, "%.0f");

    ImGui::SeparatorText("Kill threshold");
    ImGui::Checkbox("Use damage model", &g_cfg.retri_use_model);
    if (g_cfg.retri_use_model) {
        ImGui::SliderFloat("Base damage", &g_cfg.retri_base_dmg, 100.0f, 3000.0f, "%.0f");
        ImGui::SliderFloat("Per level", &g_cfg.retri_dmg_per_level, 0.0f, 300.0f, "%.0f");
        ImGui::SliderFloat("Per jungle stack", &g_cfg.retri_stack_dmg, 0.0f, 400.0f, "%.0f");
        ImGui::SliderInt("Jungle stacks", &g_cfg.retri_stacks, 0, 40);
        ImGui::SliderFloat("Safety", &g_cfg.retri_safety, 0.5f, 1.5f, "%.2f");
        ImGui::TextDisabled("Live estimate is on the Status tab.");
    } else {
        ImGui::SliderFloat("HP %% threshold", &g_cfg.retri_hp_pct, 1.0f, 60.0f, "%.0f%%");
    }

    ImGui::Text("Retri button: %.0f, %.0f  (calibrate below)", g_cfg.retri_btn_x,
                g_cfg.retri_btn_y);
    ImGui::EndDisabled();
}

void SectionCalibration() {
    if (!ImGui::CollapsingHeader("Calibration")) return;

    ImGui::TextWrapped(
        "Tap the in-game button once over an empty part of the screen, then "
        "reopen this menu and press Capture. The last tap's coordinates become "
        "the selected button's position.");

    static int which = 0;
    const char* names[] = {
        "Retri", "Attack assist", "Kimmy basic",
        "Combo S1", "Combo S2", "Combo S3", "Combo BA",
        "Skill 1", "Skill 2", "Skill 3", "Skill Basic", "Skill Spell",
    };
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("Button", &which, names, IM_ARRAYSIZE(names));

    if (ImGui::Button("Capture last tap")) {
        float x = 0, y = 0;
        if (input::LastTapPos(&x, &y)) {
            switch (which) {
                case 0:  g_cfg.retri_btn_x = x; g_cfg.retri_btn_y = y; break;
                case 1:  g_cfg.aim_btn_x = x;   g_cfg.aim_btn_y = y;   break;
                case 2:  g_cfg.kimmy_btn_x = x; g_cfg.kimmy_btn_y = y; break;
                case 3:  g_cfg.combo_s1_x = x;  g_cfg.combo_s1_y = y;  break;
                case 4:  g_cfg.combo_s2_x = x;  g_cfg.combo_s2_y = y;  break;
                case 5:  g_cfg.combo_s3_x = x;  g_cfg.combo_s3_y = y;  break;
                case 6:  g_cfg.combo_ba_x = x;  g_cfg.combo_ba_y = y;  break;
                default: {
                    SkillCfg& sk = g_cfg.skill[which - 7];
                    sk.btn_x = x; sk.btn_y = y;
                    break;
                }
            }
            persist::MarkDirty();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Release touch now")) autox::ReleaseAll();

    switch (which) {
        case 0:  ImGui::Text("Retri      : %.0f, %.0f", g_cfg.retri_btn_x, g_cfg.retri_btn_y); break;
        case 1:  ImGui::Text("Attack     : %.0f, %.0f", g_cfg.aim_btn_x, g_cfg.aim_btn_y); break;
        case 2:  ImGui::Text("Kimmy      : %.0f, %.0f", g_cfg.kimmy_btn_x, g_cfg.kimmy_btn_y); break;
        case 3:  ImGui::Text("Combo S1   : %.0f, %.0f", g_cfg.combo_s1_x, g_cfg.combo_s1_y); break;
        case 4:  ImGui::Text("Combo S2   : %.0f, %.0f", g_cfg.combo_s2_x, g_cfg.combo_s2_y); break;
        case 5:  ImGui::Text("Combo S3   : %.0f, %.0f", g_cfg.combo_s3_x, g_cfg.combo_s3_y); break;
        case 6:  ImGui::Text("Combo BA   : %.0f, %.0f", g_cfg.combo_ba_x, g_cfg.combo_ba_y); break;
        default: {
            const SkillCfg& sk = g_cfg.skill[which - 7];
            ImGui::Text("%-10s : %.0f, %.0f", names[which], sk.btn_x, sk.btn_y);
            break;
        }
    }
    ImGui::TextDisabled("Tip: turn on 'Mark me too' in the Map tab to line the "
                        "minimap overlay up first.");
}

void TabCombat() {
    SectionTargeting();
    SectionAim();
    SectionSkillAim();
    SectionKimmy();
    SectionCombo();
    SectionRetri();
    SectionCalibration();
}

// ---------------------------------------------------------------------------
// Map tab
// ---------------------------------------------------------------------------

void SectionMinimap() {
    if (!ImGui::CollapsingHeader("Minimap", ImGuiTreeNodeFlags_DefaultOpen)) return;

    ImGui::Checkbox("Enable minimap overlay", &g_cfg.map_enabled);
    ImGui::BeginDisabled(!g_cfg.map_enabled);
    ImGui::Checkbox("Show markers", &g_cfg.map_minimap);

    ImGui::SeparatorText("Markers");
    ImGui::Checkbox("Hero portraits", &g_cfg.map_icons);
    if (g_cfg.map_icons)
        ImGui::SliderFloat("Icon size", &g_cfg.mm_icon_size, 12.0f, 48.0f, "%.0f px");
    ImGui::SliderFloat("Dot size", &g_cfg.mm_dot, 3.0f, 16.0f, "%.0f");
    ImGui::Checkbox("Health ring", &g_cfg.map_hp_ring);
    if (g_cfg.map_hp_ring)
        ImGui::SliderFloat("Ring thickness", &g_cfg.mm_ring, 1.0f, 5.0f, "%.1f");
    ImGui::Checkbox("Name labels", &g_cfg.map_names);

    ImGui::SeparatorText("Include");
    ImGui::Checkbox("Allies", &g_cfg.map_show_allies);
    ImGui::SameLine();
    ImGui::Checkbox("Monsters", &g_cfg.map_show_monsters);
    ImGui::SameLine();
    ImGui::Checkbox("Minions", &g_cfg.map_show_minions);
    ImGui::EndDisabled();
}

void SectionMinimapCalibration() {
    if (!ImGui::CollapsingHeader("Minimap Calibration")) return;

    ImGui::TextWrapped(
        "Turn on 'Mark me too' and line the cyan marker up with the dot the "
        "game draws for you in the minimap corner. Red markers are enemies.");
    ImGui::Checkbox("Mark me too", &g_cfg.map_debug_self);
    ImGui::SliderFloat("Map X", &g_cfg.mm_x, -200.0f, 600.0f, "%.0f");
    ImGui::SliderFloat("Map Y", &g_cfg.mm_y, -200.0f, 600.0f, "%.0f");
    ImGui::SliderFloat("Map W", &g_cfg.mm_w, 50.0f, 600.0f, "%.0f");
    ImGui::SliderFloat("Map H", &g_cfg.mm_h, 50.0f, 600.0f, "%.0f");
    ImGui::SliderFloat("World half", &g_cfg.mm_half, 20.0f, 120.0f, "%.1f");
    ImGui::Checkbox("Rotate 45", &g_cfg.mm_rotate45);
    ImGui::SameLine();
    ImGui::Checkbox("Flip X", &g_cfg.mm_flip_x);
    ImGui::SameLine();
    ImGui::Checkbox("Flip Y", &g_cfg.mm_flip_y);
}

void SectionMaphack() {
    if (!ImGui::CollapsingHeader("Maphack (engine flags)")) return;

    ImGui::Checkbox("Enable##map_health", &g_cfg.map_health);
    ImGui::TextWrapped(
        "Forces enemy health bars visible. This is the only engine flag still "
        "written: the bush reveal and renderer flags freeze then crash the "
        "client, because the engine frees the presentation objects they gate, "
        "so those writes were removed outright rather than defaulted off.");
    ImGui::TextWrapped(
        "The minimap markers above are drawn by Rain and touch no engine state "
        "at all, which is why they work through fog and cannot crash.");
}

void TabMap() {
    SectionMinimap();
    SectionMinimapCalibration();
    SectionMaphack();
}

// ---------------------------------------------------------------------------
// Status tab
// ---------------------------------------------------------------------------

void TabStatus() {
    ImGui::TextUnformatted("Rain");
    ImGui::Separator();
    ImGui::Text("il2cpp    : %s", game::Ready() ? "bound" : "waiting for battle scene");
    ImGui::Text("Local camp: %d (logic: %d)", game::LocalCamp(), game::LogicCamp());
    ImGui::Text("Surface   : %dx%d", game::ScreenWidth(), game::ScreenHeight());
    ImGui::Text("Overlay   : %s", overlay::IsInstalled() ? "installed" : "not installed");
    if (overlay::LastError()[0])
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Error: %s", overlay::LastError());

    const game::FrameSnapshot& snap = game::Snapshot();
    ImGui::Text("Entities  : %d players, %d monsters", snap.n_players, snap.n_monsters);

    ImGui::SeparatorText("Retribution estimate");
    if (snap.has_self) {
        const float lvl = (snap.self.level > 0) ? static_cast<float>(snap.self.level) : 1.0f;
        const float dmg = g_cfg.retri_base_dmg +
                          g_cfg.retri_dmg_per_level * (lvl - 1.0f) +
                          g_cfg.retri_stack_dmg * static_cast<float>(g_cfg.retri_stacks);
        ImGui::Text("Hero level %d -> predicted %.0f damage", snap.self.level, dmg);
        ImGui::Text("Threshold   : %.0f HP (safety %.2f)", dmg * g_cfg.retri_safety,
                    g_cfg.retri_safety);
    } else {
        ImGui::TextDisabled("no local entity yet");
    }
    if (ImGui::CollapsingHeader("Objectives")) {
        float hp = 0, mx = 0;
        if (game::TrackedObjectiveHp("turtle", &hp, &mx))
            ImGui::Text("Turtle: %.0f / %.0f   (%.1fs since hit, contested=%d)",
                        hp, mx, game::TurtleSinceHit(), game::TurtleContested());
        else ImGui::TextDisabled("Turtle: not seen");
        if (game::TrackedObjectiveHp("lord", &hp, &mx))
            ImGui::Text("Lord  : %.0f / %.0f   (%.1fs since hit, contested=%d)",
                        hp, mx, game::LordSinceHit(), game::LordContested());
        else ImGui::TextDisabled("Lord: not seen");
    }

    if (ImGui::CollapsingHeader("Entity table")) {
        if (ImGui::BeginTable("ents", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("id");
            ImGui::TableSetupColumn("camp");
            ImGui::TableSetupColumn("hp");
            ImGui::TableSetupColumn("pos");
            ImGui::TableSetupColumn("flags");
            ImGui::TableHeadersRow();
            for (int i = 0; i < snap.n_players && i < 12; i++) {
                const auto& e = snap.players[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%d", e.id);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%d", e.camp);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.0f/%.0f", e.hp, e.hp_max);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f,%.1f", e.x, e.y);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%s%s%s", e.is_soldier ? "S" : "H", e.is_dead ? "D" : "",
                            e.minimap_visible ? "V" : "-");
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Detection surface")) {
        ImGui::TextWrapped(
            "The game's own reporter state. Live values are not reachable on "
            "this build (instances are held by non-singleton managers); what is "
            "shown is which detection machinery exists and binds.");
        ImGui::Text("AFK/cheat reporter: %s",
                    game::ReporterFieldsBound() ? "fields bound" : "not found");
        ImGui::TextWrapped(
            "Known client-side systems: AntiCheatReporter, CheatAfkReporter, "
            "BattleInputBehaviorReporter (input cadence), HeroLoadDetector, "
            "PerformanceDetection, DataReportMgr. Enforcement is server-side.");
    }

    if (ImGui::CollapsingHeader("Hero icons")) {
        ImGui::TextWrapped(
            "Drop hero portraits named <id>.png into the game's external files "
            "dir (rain_icons/) — adb push reaches it without root. 16.png = "
            "Zilong and 71.png = Kimmy are calibration-verified pairs.");
        ImGui::TextWrapped(
            "adb push hero.png /storage/emulated/0/Android/data/"
            "com.mobile.legends/files/rain_icons/16.png");
    }

    if (ImGui::CollapsingHeader("Battle records")) {
        static const char* kRecDir =
            "/storage/emulated/0/Android/data/com.mobile.legends/files/dragon2017/BattleRecord";
        DIR* d = opendir(kRecDir);
        if (!d) {
            ImGui::Text("No records directory");
        } else {
            int count = 0;
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr && count < 12) {
                if (ent->d_name[0] == '.') continue;
                char path[512];
                snprintf(path, sizeof(path), "%s/%s", kRecDir, ent->d_name);
                long sz = 0;
                if (FILE* f = fopen(path, "rb")) {
                    fseek(f, 0, SEEK_END);
                    sz = ftell(f);
                    fclose(f);
                }
                ImGui::Text("%s  (%ld)", ent->d_name, sz);
                count++;
            }
            closedir(d);
            if (count == 0) ImGui::TextDisabled("(empty)");
        }
    }
}

// ---------------------------------------------------------------------------
// Settings tab
// ---------------------------------------------------------------------------

// The profile manager's state. The list is rescanned on demand (open, save,
// delete) rather than every frame — readdir on the render thread is cheap but
// not free, and nothing outside this tab changes the directory.
//
// Names are auto-generated ("profile_1", "profile_2", ...). There is no
// keyboard path on this platform: Rain reads touches, not key events, and the
// ImGui text field cannot be typed into on a phone. Renaming a profile is a
// file rename, which is the point of the format — the files are plain JSON
// the user owns, so `mv profile_1.json gusion.json` in any file manager (or
// over ADB) is the rename.
char g_profiles[32][64];
int  g_profile_count = 0;
int  g_profile_sel = -1;
bool g_profiles_dirty = true;
float g_ui_scale_applied = -1.0f;

void RescanProfiles() {
    g_profile_count = cfgjson::List(g_profiles, 32);
    g_profiles_dirty = false;
    if (g_profile_sel >= g_profile_count) g_profile_sel = g_profile_count - 1;
}

// The first "profile_N" not already taken, so repeated saves never overwrite
// an existing snapshot.
void NextProfileName(char* out, size_t n) {
    for (int i = 1; i < 1000; i++) {
        snprintf(out, n, "profile_%d", i);
        bool taken = false;
        for (int j = 0; j < g_profile_count; j++) {
            if (strcmp(g_profiles[j], out) == 0) { taken = true; break; }
        }
        if (!taken) return;
    }
    snprintf(out, n, "profile_%lld",
             static_cast<long long>(time(nullptr)));
}

void TabSettings() {
    ImGui::TextUnformatted("Menu");
    ImGui::Separator();

    if (ImGui::SliderFloat("UI scale", &g_cfg.ui_scale, 0.7f, 1.6f, "%.2f"))
        persist::MarkDirty();
    if (ImGui::SliderFloat("Launcher button size", &g_cfg.launcher_scale,
                           0.5f, 2.0f, "%.2fx"))
        persist::MarkDirty();
    ImGui::TextDisabled(
        "Size of the round button that opens this menu. Drag the button to "
        "move it.");
    ImGui::Checkbox("Watermark", &g_cfg.watermark);

    ImGui::SeparatorText("Config profiles");
    ImGui::TextWrapped(
        "Profiles are plain JSON files in %s - browse, back up, rename or "
        "copy them with any file manager, or pull the folder over ADB. "
        "Renaming a file renames the profile.",
        cfgjson::Dir());

    if (ImGui::Button("Refresh")) g_profiles_dirty = true;
    ImGui::SameLine();
    if (ImGui::Button("Save as default")) {
        // The auto-save profile: also what boots next launch.
        if (cfgjson::Save(g_cfg, "default")) g_profiles_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset everything")) {
        const bool wm = g_cfg.watermark;
        const float scale = g_cfg.ui_scale;
        const float lscale = g_cfg.launcher_scale;
        g_cfg = Config();
        ConfigDefaults(g_cfg);
        g_cfg.watermark = wm;
        g_cfg.ui_scale = scale;
        g_cfg.launcher_scale = lscale;
        persist::MarkDirty();
    }

    if (g_profiles_dirty) RescanProfiles();

    ImGui::SeparatorText("Saved profiles");
    if (g_profile_count == 0) {
        ImGui::TextDisabled("No profiles yet. Save one below.");
    }
    for (int i = 0; i < g_profile_count; i++) {
        const bool sel = (i == g_profile_sel);
        if (ImGui::Selectable(g_profiles[i], sel)) g_profile_sel = i;
        if (sel) ImGui::SetItemDefaultFocus();
    }

    ImGui::BeginDisabled(g_profile_sel < 0 || g_profile_sel >= g_profile_count);
    if (ImGui::Button("Load")) {
        // Load into the live config, then persist it as the auto-save profile
        // so the choice survives a restart without a second tap.
        if (cfgjson::Load(g_cfg, g_profiles[g_profile_sel])) {
            g_ui_scale_applied = -1.0f;  // force the UI scale to re-apply
            persist::Save(g_cfg);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Overwrite")) {
        if (cfgjson::Save(g_cfg, g_profiles[g_profile_sel])) g_profiles_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        if (cfgjson::Delete(g_profiles[g_profile_sel])) {
            g_profiles_dirty = true;
            g_profile_sel = -1;
        }
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Save new profile");
    if (ImGui::Button("New profile")) {
        char name[64];
        NextProfileName(name, sizeof(name));
        if (cfgjson::Save(g_cfg, name)) {
            g_profiles_dirty = true;
            // Select the new snapshot so Load/Overwrite/Delete point at it.
            g_profile_sel = -1;  // rescan will land on it below
            RescanProfiles();
            for (int i = 0; i < g_profile_count; i++) {
                if (strcmp(g_profiles[i], name) == 0) { g_profile_sel = i; break; }
            }
        }
    }
    ImGui::TextDisabled(
        "Snapshots the current settings as profile_N. Rename the file to "
        "rename the profile.");

    ImGui::SeparatorText("Auto-save");
    ImGui::TextWrapped(
        "Every change is also written to 'default' a couple of seconds later, "
        "and 'default' loads at boot. Named profiles are snapshots on top of "
        "that.");

    ImGui::SeparatorText("Touch");
    if (ImGui::Button("Release all touches")) autox::ReleaseAll();
    ImGui::TextDisabled("Emergency release if a synthesized press ever sticks.");

    ImGui::SeparatorText("Account");
    ImGui::TextWrapped(
        "Resets the stored guest identity: every device-ID key in the game's "
        "preferences is replaced with a fresh value, and the login caches are "
        "removed. The game restarts as a new guest and reaches the login page.");
    ImGui::TextWrapped(
        "The process is killed at the end of the reset - that is deliberate. "
        "Unity flushes its preferences on a clean exit, which would write the "
        "old identity back. A full backup goes to rain_account_backup/ first.");
    ImGui::TextWrapped(
        "Limit, stated plainly: this does not un-ban an account. A ban is "
        "server-side; a fresh guest identity is a different account, not a "
        "cleared one.");

    // Two-step: a single tap must never wipe state.
    static bool confirm_reset = false;
    if (!confirm_reset) {
        if (ImGui::Button("Reset Guest")) confirm_reset = true;
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Confirm: reset identity + wipe login")) {
            account::Reset();
            confirm_reset = false;
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) confirm_reset = false;
        ImGui::TextDisabled("The game closes itself when the reset completes. "
                            "Relaunch it and sign in.");
    }
    if (strcmp(account::LastResult(), "not attempted") != 0) {
        ImGui::TextWrapped("%s", account::LastResult());
    }

    ImGui::SeparatorText("About");
    ImGui::TextWrapped(
        "Rain - MLBB client modification. Loaded as one of the game's own "
        "hot-updated native modules. No hardcoded offsets: classes and fields "
        "resolve by name through the IL2CPP reflection API, so a game update "
        "that moves fields around does not break it.");
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------

ImVec2 g_launcher_pos(60.0f, 300.0f);
bool g_launcher_init = false;
bool g_launcher_dragged = false;

ImVec2 g_menu_pos(240.0f, 90.0f);
bool g_menu_pos_init = false;

int g_tab = TAB_VISUAL;

}  // namespace

bool Visible() { return g_cfg.menu_open; }

void DrawLauncher() {
    if (!g_launcher_init) {
        g_launcher_pos = ImVec2(game::ScreenWidth() * 0.03f, game::ScreenHeight() * 0.32f);
        g_launcher_init = true;
    }

    // Size from the config. Clamped so a bad value cannot make the button
    // unhittable or fill the screen; the window, the rounding and the inner
    // button all scale together so the circle stays a circle.
    const float scale = (g_cfg.launcher_scale > 0.4f && g_cfg.launcher_scale < 2.5f)
                            ? g_cfg.launcher_scale : 1.0f;
    const float box = 64.0f * scale;
    const float pad = 4.0f * scale;
    const float inner = 56.0f * scale;

    ImGui::SetNextWindowPos(g_launcher_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(box, box), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, box * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.09f, 0.15f, 0.80f));
    ImGui::Begin("##rain_launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.20f, 0.36f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.36f, 0.62f, 0.95f));
    const bool clicked = ImGui::Button("Rain", ImVec2(inner, inner));

    // Moveable launcher: dragging the button moves it via the window position;
    // a clean click (no drag) opens the menu. The drag flag is read before it
    // is reset on release.
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
        g_launcher_pos.x += ImGui::GetIO().MouseDelta.x;
        g_launcher_pos.y += ImGui::GetIO().MouseDelta.y;
        g_launcher_dragged = true;
    }
    if (clicked && !g_launcher_dragged) g_cfg.menu_open = !g_cfg.menu_open;
    if (!ImGui::IsItemActive()) g_launcher_dragged = false;

    ImGui::PopStyleColor(2);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void DrawFloatingButtons() {
    if (!g_cfg.kimmy_km_button) return;
    if (game::ScreenWidth() <= 0) return;

    const game::FrameSnapshot& s = game::Snapshot();
    const bool on_kimmy = s.has_self && s.self.id == 71;
    // The indicator only appears on Kimmy, where the aura can actually run.
    if (!on_kimmy) return;

    static ImVec2 km_pos(0, 0);
    static bool km_init = false;
    static bool km_dragged = false;
    if (!km_init) {
        km_pos = ImVec2(game::ScreenWidth() * 0.86f, game::ScreenHeight() * 0.18f);
        km_init = true;
    }

    ImGui::SetNextWindowPos(km_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(58, 58), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 29.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        g_cfg.kimmy_enabled ? ImVec4(0.08f, 0.28f, 0.14f, 0.90f)
                            : ImVec4(0.09f, 0.11f, 0.15f, 0.78f));
    ImGui::Begin("##rain_km", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoMove);
    ImGui::PushStyleColor(ImGuiCol_Button,
        g_cfg.kimmy_enabled ? ImVec4(0.16f, 0.58f, 0.32f, 0.95f)
                            : ImVec4(0.14f, 0.18f, 0.26f, 0.90f));
    const bool clicked = ImGui::Button(g_cfg.kimmy_enabled ? "KM*" : "KM",
                                       ImVec2(52, 52));
    // Drag to move, clean click to arm/disarm. This is the aura's only control:
    // it is an ImGui button, so it works anywhere and can never be confused
    // with a game input.
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
        km_pos.x += ImGui::GetIO().MouseDelta.x;
        km_pos.y += ImGui::GetIO().MouseDelta.y;
        km_dragged = true;
    }
    if (clicked && !km_dragged) {
        g_cfg.kimmy_enabled = !g_cfg.kimmy_enabled;
        persist::MarkDirty();
        LOGI("kimmy: aura %s (KM button)",
             g_cfg.kimmy_enabled ? "ARMED" : "DISARMED");
        if (!g_cfg.kimmy_enabled) autox::ReleaseAll();
    }
    if (!ImGui::IsItemActive()) km_dragged = false;
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void DrawWatermark() {
    if (!g_cfg.watermark) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddText(ImVec2(18, 46), IM_COL32(0, 0, 0, 180), "Rain");
    dl->AddText(ImVec2(17, 45), IM_COL32(120, 200, 255, 230), "Rain");
}

void Draw() {
    // Re-apply the UI scale only when it changes: ScaleAllSizes compounds, so
    // it must run once per value, not per frame. This also catches a scale
    // that arrived from a profile load rather than the slider.
    if (g_cfg.ui_scale != g_ui_scale_applied) {
        ImGui::GetIO().FontGlobalScale = g_cfg.ui_scale;
        g_ui_scale_applied = g_cfg.ui_scale;
    }

    if (!g_menu_pos_init) {
        g_menu_pos = ImVec2(game::ScreenWidth() * 0.14f, 80.0f);
        g_menu_pos_init = true;
    }
    ImGui::SetNextWindowSize(ImVec2(780, 720), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(g_menu_pos, ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.55f, 0.95f, 0.55f));
    ImGui::Begin("@rainhook | Hooking with You <3##main", &g_cfg.menu_open,
                 ImGuiWindowFlags_NoSavedSettings);

    // Keep the title bar reachable: a drag that ends past an edge used to leave
    // the window off-screen with no way back.
    {
        const ImVec2 p = ImGui::GetWindowPos();
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        const float w = ImGui::GetWindowWidth();
        float nx = p.x, ny = p.y;
        if (nx > ds.x - 80.0f) nx = ds.x - 80.0f;
        if (nx < 80.0f - w) nx = 80.0f - w;
        if (ny < 0.0f) ny = 0.0f;
        if (ny > ds.y - 40.0f) ny = ds.y - 40.0f;
        if (nx != p.x || ny != p.y) ImGui::SetWindowPos(ImVec2(nx, ny));
    }

    // Sidebar + content. The section list is the shape the reference mod uses
    // and the only one that stays navigable at this option count.
    const float sidebar_w = 118.0f;
    ImGui::BeginChild("##sidebar", ImVec2(sidebar_w, 0), false,
                      ImGuiWindowFlags_NoScrollbar);
    for (int i = 0; i < TAB_COUNT; i++) {
        ImGui::PushID(4000 + i);
        if (ImGui::Selectable(kTabNames[i], g_tab == i, 0, ImVec2(0, 34))) g_tab = i;
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##content", ImVec2(0, 0), false);
    switch (g_tab) {
        case TAB_VISUAL:   TabVisual();   break;
        case TAB_COMBAT:   TabCombat();   break;
        case TAB_MAP:      TabMap();      break;
        case TAB_STATUS:   TabStatus();   break;
        case TAB_SETTINGS: TabSettings(); break;
        default: break;
    }
    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    // Any frame with the menu open marks the config dirty; the persist tick
    // saves at most every 2 s, so toggles and sliders survive restarts.
    persist::MarkDirty();
}

}  // namespace menu
