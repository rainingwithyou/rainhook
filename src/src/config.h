#pragma once
// Shared feature configuration, written by the menu and read by the renderers.

// Every drawable class of entity has its own ESP row: whether it is drawn,
// its own colour, and its own set of sub-elements. Geometry — box style, scale,
// thickness, glow layers, health/name placement, tracer origin — stays global
// so a category changes what it shows and how it is coloured, not the shape
// language of the whole overlay.
enum EspCat {
    CAT_SELF = 0,       // the local player
    CAT_ALLY_HERO,
    CAT_ENEMY_HERO,
    CAT_ALLY_MINION,
    CAT_ENEMY_MINION,
    CAT_TURTLE,
    CAT_LORD,
    CAT_BUFF,
    CAT_CREEP,          // other jungle creeps (Lithowander, crab)
    CAT_COUNT
};

struct CatCfg {
    bool enabled = true;
    bool box = true;
    bool health = true;        // health bar
    bool health_text = false;  // numeric HP
    bool name = true;          // level tag (players) / label (entities)
    bool hero_name = true;     // hero / monster display name
    bool distance = false;
    bool line = false;         // tracer
    bool icon = false;         // portrait from the drop-in icon dir
    bool spell_cd = false;     // cooldowns from m_ShowCoolDownComp
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    // Dead handling, per category. The global esp_show_dead flag is gone —
    // a corpse matters differently per class (an enemy hero's death is worth
    // marking, a lane minion's is noise), so each row decides for itself.
    //   0 = hide the entity once it is dead
    //   1 = keep drawing it in its own category colour
    //   2 = draw it in the shared dead colour (col_dead)
    int dead_mode = 0;

    // Per-category distance cap in world units; 0 = fall back to the global
    // esp_max_distance / ent_max_distance. Applied before the tracer draws,
    // so a capped category never leaves a line pointing at a filtered entity.
    float max_dist = 0.0f;
};

// Skill slots that can be auto-aimed. Basic and Spell are separate from the
// three hero skills because they have their own buttons and cooldown sources.
enum SkillSlot {
    SLOT_S1 = 0,
    SLOT_S2,
    SLOT_S3,
    SLOT_BASIC,
    SLOT_SPELL,
    SLOT_COUNT
};

struct SkillCfg {
    bool enabled = false;
    // 0 = smart-cast: tap the button and let the game pick the direction.
    // 1 = drag-aim: press the button and drag toward the target's screen
    //     position, which is true directional aiming.
    int aim_mode = 0;
    float btn_x = 0.0f, btn_y = 0.0f;   // calibrated button centre, screen px
    float range = 12.0f;                // cast range, world units
    int target_mode = 0;                // target::Mode
    int priority = 2;                   // target::Priority
    bool cd_check = true;               // skip while the game says it is cooling
    float drag_len = 120.0f;            // px the aim drag travels
};

struct Config {
    // ---- ESP master ----
    bool esp_enabled = true;

    // ---- ESP categories ----
    CatCfg cat[CAT_COUNT];

    // ---- ESP geometry (shared by every category) ----
    int  esp_box_style = 0;        // 0 = 2D corner, 1 = full outline, 2 = glow
    int  esp_health_pos = 0;       // 0 = vertical left, 1 = vertical right,
                                   // 2 = horizontal up, 3 = horizontal down
    int  esp_name_pos = 0;         // 0 = above box, 1 = below box
    float esp_box_scale = 1.0f;
    int  esp_thickness = 2;
    int  esp_glow_layers = 4;      // halo passes for glow style
    // Tracer/line origin: 0 = from camera (screen center, follows the view),
    // 1 = from the local player hero (follows our own hero's screen position).
    int  esp_line_origin_mode = 0;  // 0 = camera, 1 = player
    int  esp_line_vpos = 0;         // tracer vertical anchor: 0 = center, 1 = bottom
    // Tracer line origin, used in camera mode: 0.5 = screen center horizontally.
    float esp_line_origin = 0.5f;  // horizontal origin, 0 = left, 1 = right

    float esp_max_distance = 0.0f;  // players, 0 = unlimited
    float ent_max_distance = 0.0f;  // jungle/camp entities, 0 = unlimited

    // ---- Shared ESP decoration ----
    bool esp_indicators = true;    // DEAD / SIGHT / HIDDEN markers
    // Dead handling moved to CatCfg.dead_mode (per category). This colour is
    // the shared "dead" tint that dead_mode == 2 uses.
    float col_dead[4] = {0.55f, 0.58f, 0.65f, 1.0f};

    // ---- Range / FOV circle ----
    // A translucent circle on the ground around the local hero showing the
    // active targeting range. Draw-only (no engine writes), so it is safe to
    // leave on; the radius follows whichever feature is currently armed.
    bool  range_circle = false;
    int   range_circle_source = 0;   // 0 = auto (aim > skill > kimmy), 1 = fixed
    float range_circle_fixed = 12.0f; // world units, when source == 1
    float range_circle_thickness = 2.0f;
    float range_circle_color[4] = {0.35f, 0.75f, 1.0f, 0.55f};

    // ---- Objective alerts (VYNIX-style) ----
    // Banner when Turtle/Lord HP is dropping: their HP decreased within the
    // last few seconds and at least one enemy is near the pit.
    bool alerts_enabled = true;
    bool alert_turtle = true;
    bool alert_lord = true;

    // ---- Skin changer ----
    // Writes m_ShowSkinId on the local player's ShowEntity. Presentation-side:
    // takes effect when the game re-renders the model. Only the local player is
    // touched; other entities are never written.
    bool skin_enabled = false;
    int  skin_id = 0;              // 0 = default skin
    bool skin_applied = false;     // set once a write succeeded (menu state)

    // ---- Drone view ----
    // Three coordinated changes, matching the reference implementation:
    //   drone_fov    -> Camera.fieldOfView, a pure zoom-out with no interaction
    //                   with the camera controller at all.
    //   drone_height -> SmoothFollow.offSetPos. Two write paths:
    //                     hook on (default): GlossHook detours
    //                       SmoothFollow.set_offSetPos and adds our delta to the
    //                       value the game itself is writing. This is exactly
    //                       what VYNIX does — we ride the controller's own code
    //                       path, so it can never fight us or flicker.
    //                     hook off / unresolvable: the legacy backing-field write
    //                       (read-back guarded), then the transform lift.
    // If none of those resolve, drone_height falls back to lifting the live
    // camera transform. Stock values are captured on enable and restored on
    // disable.
    bool drone_enabled = false;
    float drone_height = 0.0f;     // "Camera High": follow-offset lift
    float drone_fov = 0.0f;        // degrees added to the stock field of view
    bool drone_hook = true;        // hook set_offSetPos (VYNIX model)

    // ---- Maphack ----
    // Only flags the engine tolerates being rewritten are exposed. Forcing
    // visibleOutsideGrassInRange / m_bRendererVisible freezes then crashes the
    // client (the engine frees the presentation objects those flags gate), so
    // those writes were removed outright, not just defaulted off.
    bool map_enabled = false;
    bool map_minimap = true;       // draw enemy markers on the minimap (overlay)
    bool map_health = false;       // force health bars visible (flag write)

    // Minimap overlay geometry. MLBB's map is a square rotated 45 degrees in
    // world space (bases at +-50 on x, other corners at +-50 on z), and the
    // minimap widget draws it unrotated, so the same turn is applied here.
    // Calibration: turn on map_debug_self and compare the own-side marker
    // against the dot the game draws for you on the minimap.
    float mm_x = 0.0f, mm_y = 0.0f;        // minimap area top-left, px
    float mm_w = 268.0f, mm_h = 245.0f;    // minimap area size, px
    float mm_half = 52.0f;                 // world half-extent, units
    bool mm_rotate45 = true;
    bool mm_flip_x = false, mm_flip_y = false;
    float mm_dot = 7.0f;                   // marker radius, px
    bool map_debug_self = false;           // also mark our own entity

    // Marker content. Portraits come from the same drop-in icon directory the
    // world ESP uses; a hero with no icon file falls back to the dot, so the
    // minimap stays readable with a partial icon set. Portraits default ON now
    // that the game's own hero atlas supplies the full 0-134 range.
    bool map_icons = true;         // draw hero portraits instead of dots
    float mm_icon_size = 26.0f;    // portrait size, px
    bool map_names = false;        // hero name under each marker
    bool map_hp_ring = true;       // ring tinted by the entity's health
    float mm_ring = 2.0f;          // ring thickness, px
    bool map_show_allies = false;  // allies as well as enemies
    bool map_show_monsters = true; // turtle / lord / buffs
    bool map_show_minions = false; // lane minions (noisy, off by default)

    // ---- Automation (XNISH-pattern) ----
    // Taps are synthesized through the app's own dispatchTouchEvent, so the
    // game sees genuine touches. These features generate behavioural input —
    // BattleInputBehaviorReporter watches tap cadence — so each has its own
    // toggle, default off, and the calibrated button positions are persisted.
    bool retri_enabled = false;    // auto retribution
    bool retri_lord = true;
    bool retri_turtle = true;
    bool retri_buff = false;
    bool retri_litho = false;      // Lithowanderer / jungle creep camps
    // 0 = only while an enemy hero is near the pit, 1 = fire regardless.
    // The previous version always applied the enemy guard; the reference mod
    // exposes both, and "always" is the useful one when you are the one
    // contesting a pit nobody else has shown up to.
    int  retri_trigger = 0;
    float retri_hp_pct = 20.0f;    // cast when objective HP% at or below this
    float retri_enemy_dist = 8.0f; // only when an enemy is within this range of the pit
    float retri_btn_x = 0.0f, retri_btn_y = 0.0f;  // retri spell button, screen px

    bool aim_enabled = false;      // basic-attack assist
    // Selection policy, shared with the other combat features. Indices match
    // target::Mode / target::Priority. These replaced the old pair of
    // aim_players / aim_monsters booleans, which could not express "adaptive"
    // and left the aim priority hardcoded.
    int  aim_mode = 0;             // 0 players, 1 monsters, 2 both
    int  aim_priority = 2;         // 0 closest, 1 lowest HP, 2 lowest HP %
    float aim_range = 12.0f;       // activation range, world units
    float aim_btn_x = 0.0f, aim_btn_y = 0.0f;      // basic attack button, px

    bool combo_enabled = false;    // skill combo on trigger
    float combo_range = 10.0f;     // trigger range, world units
    int combo_priority = 2;        // target::Priority for the trigger scan
    // The four buttons a combo can touch, calibrated once each...
    float combo_s1_x = 0.0f, combo_s1_y = 0.0f;
    float combo_s2_x = 0.0f, combo_s2_y = 0.0f;
    float combo_s3_x = 0.0f, combo_s3_y = 0.0f;
    float combo_ba_x = 0.0f, combo_ba_y = 0.0f;
    // ...and the cast order over them as slot indices (0 = S1, 1 = S2,
    // 2 = S3, 3 = basic attack). Five steps because that is what the real
    // per-hero sequences are: Gusion runs S2->S1->S3->S2->S1 and Benedetta
    // S2->S1->S3->BA->BA. Storing the order rather than five absolute
    // positions means a preset survives a HUD layout change.
    int combo_seq[5] = {1, 0, 2, 1, 0};
    int combo_steps = 5;           // how many of the five the preset uses
    int combo_delay = 8;
    int combo_preset = 0;          // 0 = custom, 1 = Gusion, 2 = Benedetta

    // ---- Kimmy press-and-hold ----
    // "Quick-Clear": hold the basic-attack button so the game's own held-attack
    // targeting keeps firing and auto-locks onto whatever is in range. Two
    // flavours — one that follows the shared targeting policy, and a
    // minion-first one for clearing a lane.
    //
    // Control is the floating KM button (an ImGui button, so it works anywhere
    // and cannot be confused with a game input). The M1 anchor is captured
    // automatically from the first tap in the HUD action zone during a match.
    bool kimmy_enabled = false;
    bool kimmy_minion_mode = false;  // minion-first priority (lane clear)
    float kimmy_range = 14.0f;
    int kimmy_priority = 2;          // target::Priority
    bool kimmy_km_button = true;     // floating in-match toggle
    float kimmy_btn_x = 0.0f, kimmy_btn_y = 0.0f;  // basic attack button, px
    // 0 = hold only: the press stays on the anchor and the game's own held
    //     attack picks the direction.
    // 1 = hold + steer: the held press is dragged toward the selected target,
    //     so the game reads a drag vector and rotates the aim onto it. This is
    //     XNISH's "Kimmy Auto Aim" layered on top of the hold.
    int kimmy_aim_mode = 0;
    float kimmy_drag_len = 120.0f;   // px the steer drag travels

    // ---- Predicted Retribution damage ----
    // A flat HP% cannot know whether the cast will actually secure, because
    // Retribution damage scales with hero level and jungle stacks. The exact
    // per-level table is compiled into the game and is not recoverable
    // statically, so the coefficients are exposed with sane defaults; the
    // Status tab shows the live estimate so they can be calibrated in practice.
    bool  retri_use_model = true;
    float retri_base_dmg = 600.0f;      // damage at hero level 1
    float retri_dmg_per_level = 55.0f;  // added per hero level
    float retri_stack_dmg = 120.0f;     // added per jungle stack
    float retri_safety = 1.0f;          // >1 fires earlier, <1 later
    // Jungle stacks are not exposed as a readable field on this build, so the
    // count is manual. Hero level is read live from the local entity.
    int   retri_stacks = 0;

    // ---- Shared targeting ----
    // Ordered class preference, highest priority first; values are target::Class
    // (0 = Minion, 1 = Hero, 2 = Turtle, 3 = Lord, 4 = Buff, 5 = Creep).
    // Every combat feature selects through this, so "minions first" is set in
    // one place. Six entries now that the monster classes are split.
    int target_order[6] = {1, 3, 2, 4, 5, 0};

    // ---- Skill auto aim ----
    SkillCfg skill[SLOT_COUNT];

    // ---- Menu ----
    bool menu_open = false;
    bool watermark = true;
    float ui_scale = 1.0f;         // ImGui font/style scale
    // Size of the round launcher button that opens the menu, as a multiplier
    // of the base 64px. The button is the only thing on screen when the menu
    // is closed, so a small one is hard to hit on a high-DPI panel and a big
    // one is in the way on a small one.
    float launcher_scale = 1.0f;
};

extern Config g_cfg;

// Per-category default colours, applied once at startup before the saved config
// is loaded. The defaults are per-index, which a member initialiser on the
// array cannot express.
void ConfigDefaults(Config& c);
