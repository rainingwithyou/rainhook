#include "cfgjson.h"

#include <android/log.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define RAIN_MKDIR(p) _mkdir(p)
#define RAIN_UNLINK(p) _unlink(p)
#else
#include <unistd.h>
#define RAIN_MKDIR(p) mkdir(p, 0770)
#define RAIN_UNLINK(p) unlink(p)
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "config.h"

#define LOG_TAG "Rain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace cfgjson {

namespace {

constexpr const char* kDefaultDir =
    "/sdcard/Android/data/com.mobile.legends/files/rain_configs";

// The directory the profiles live in. A dev/test override is honoured so the
// round-trip can be exercised on a host build; on device the game's own
// external files dir is used, which the process owns and any file manager
// can browse.
const char* DirImpl() {
    const char* env = getenv("RAIN_CFG_DIR");
    return (env && *env) ? env : kDefaultDir;
}

void EnsureDir() {
    struct stat st;
    if (stat(DirImpl(), &st) == 0) return;
    RAIN_MKDIR(DirImpl());
}

// ---------------------------------------------------------------------------
// Writer
//
// The schema mirrors the module structure: one top-level object per feature
// area ("esp", "retribution", "kimmy", ...), each holding its own enable
// flag and settings. Keys are stable; the loader ignores anything it does
// not know, so an old file loads into a newer build cleanly.
// ---------------------------------------------------------------------------

struct KV {
    std::string k;
    std::string v;
};

std::string Esc(const char* s) {
    std::string o = "\"";
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; p++) {
        switch (*p) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (*p < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", *p);
                    o += b;
                } else {
                    o += static_cast<char>(*p);
                }
        }
    }
    o += '"';
    return o;
}

std::string Num(double v) {
    char b[32];
    if (v == static_cast<double>(static_cast<long long>(v)) &&
        v > -1e15 && v < 1e15) {
        snprintf(b, sizeof(b), "%lld", static_cast<long long>(v));
    } else {
        snprintf(b, sizeof(b), "%.6g", v);
    }
    return b;
}

KV KB(const char* k, bool v) { return {k, v ? "true" : "false"}; }
KV KI(const char* k, int v)  { return {k, std::to_string(v)}; }
KV KF(const char* k, float v) { return {k, Num(v)}; }
KV KO(const char* k, std::string obj) { return {k, std::move(obj)}; }

KV KC(const char* k, const float c[4]) {
    return {k, "[" + Num(c[0]) + ", " + Num(c[1]) + ", " +
                   Num(c[2]) + ", " + Num(c[3]) + "]"};
}

KV KA(const char* k, const int* a, int n) {
    std::string s = "[";
    for (int i = 0; i < n; i++) {
        if (i) s += ", ";
        s += std::to_string(a[i]);
    }
    s += "]";
    return {k, s};
}

std::string Obj(int depth, const std::vector<KV>& kvs) {
    const std::string ind(static_cast<size_t>(depth) * 2, ' ');
    std::string o = "{\n";
    for (size_t i = 0; i < kvs.size(); i++) {
        o += ind + "  " + Esc(kvs[i].k.c_str()) + ": " + kvs[i].v;
        if (i + 1 < kvs.size()) o += ",";
        o += "\n";
    }
    o += ind + "}";
    return o;
}

std::string Arr(int depth, const std::vector<std::string>& items) {
    const std::string ind(static_cast<size_t>(depth) * 2, ' ');
    std::string o = "[\n";
    for (size_t i = 0; i < items.size(); i++) {
        o += ind + "  " + items[i];
        if (i + 1 < items.size()) o += ",";
        o += "\n";
    }
    o += ind + "]";
    return o;
}

// Category names, index-matched to EspCat. Stable identifiers: renaming one
// orphans that row's settings in existing files.
const char* kCatNames[CAT_COUNT] = {
    "self", "ally_hero", "enemy_hero", "ally_minion", "enemy_minion",
    "turtle", "lord", "buff", "creep",
};

std::string CatObj(const CatCfg& cc, int depth) {
    return Obj(depth, {
        KB("enabled", cc.enabled),
        KB("box", cc.box),
        KB("health_bar", cc.health),
        KB("health_text", cc.health_text),
        KB("level_tag", cc.name),
        KB("hero_name", cc.hero_name),
        KB("distance", cc.distance),
        KB("tracer", cc.line),
        KB("portrait", cc.icon),
        KB("spell_cd", cc.spell_cd),
        KC("color", cc.color),
        KI("dead_mode", cc.dead_mode),
        KF("max_dist", cc.max_dist),
    });
}

std::string SkillObj(const SkillCfg& sk, int depth) {
    return Obj(depth, {
        KB("enabled", sk.enabled),
        KI("aim_mode", sk.aim_mode),
        KF("btn_x", sk.btn_x),
        KF("btn_y", sk.btn_y),
        KF("range", sk.range),
        KI("target_mode", sk.target_mode),
        KI("priority", sk.priority),
        KB("cd_check", sk.cd_check),
        KF("drag_len", sk.drag_len),
    });
}

std::string Emit(const Config& c) {
    std::vector<std::string> skills;
    for (int i = 0; i < SLOT_COUNT; i++) skills.push_back(SkillObj(c.skill[i], 2));

    std::vector<KV> esp = {
        KB("enabled", c.esp_enabled),
        KB("indicators", c.esp_indicators),
        KI("box_style", c.esp_box_style),
        KI("health_pos", c.esp_health_pos),
        KI("name_pos", c.esp_name_pos),
        KF("box_scale", c.esp_box_scale),
        KI("thickness", c.esp_thickness),
        KI("glow_layers", c.esp_glow_layers),
        KI("tracer_origin_mode", c.esp_line_origin_mode),
        KI("tracer_vpos", c.esp_line_vpos),
        KF("tracer_origin", c.esp_line_origin),
        KF("max_distance", c.esp_max_distance),
        KF("entity_max_distance", c.ent_max_distance),
        KC("dead_color", c.col_dead),
    };
    // categories as a nested object
    std::vector<KV> catKvs;
    for (int i = 0; i < CAT_COUNT; i++)
        catKvs.push_back(KO(kCatNames[i], CatObj(c.cat[i], 3)));
    esp.push_back(KO("categories", Obj(2, catKvs)));

    const std::vector<KV> root = {
        KI("version", 1),
        KO("esp", Obj(1, esp)),
        KO("range_circle", Obj(1, {
            KB("enabled", c.range_circle),
            KI("source", c.range_circle_source),
            KF("fixed_radius", c.range_circle_fixed),
            KF("thickness", c.range_circle_thickness),
            KC("color", c.range_circle_color),
        })),
        KO("alerts", Obj(1, {
            KB("enabled", c.alerts_enabled),
            KB("turtle", c.alert_turtle),
            KB("lord", c.alert_lord),
        })),
        KO("skin", Obj(1, {
            KB("enabled", c.skin_enabled),
            KI("skin_id", c.skin_id),
        })),
        KO("drone", Obj(1, {
            KB("enabled", c.drone_enabled),
            KF("height", c.drone_height),
            KF("fov", c.drone_fov),
            KB("hook", c.drone_hook),
        })),
        KO("maphack", Obj(1, {
            KB("enabled", c.map_enabled),
            KB("minimap_flags", c.map_minimap),
            KB("health_flags", c.map_health),
        })),
        KO("minimap", Obj(1, {
            KF("x", c.mm_x), KF("y", c.mm_y),
            KF("w", c.mm_w), KF("h", c.mm_h),
            KF("world_half", c.mm_half),
            KB("rotate45", c.mm_rotate45),
            KB("flip_x", c.mm_flip_x), KB("flip_y", c.mm_flip_y),
            KF("dot", c.mm_dot),
            KB("debug_self", c.map_debug_self),
            KB("icons", c.map_icons),
            KF("icon_size", c.mm_icon_size),
            KB("names", c.map_names),
            KB("hp_ring", c.map_hp_ring),
            KF("ring", c.mm_ring),
            KB("show_allies", c.map_show_allies),
            KB("show_monsters", c.map_show_monsters),
            KB("show_minions", c.map_show_minions),
        })),
        KO("retribution", Obj(1, {
            KB("enabled", c.retri_enabled),
            KB("lord", c.retri_lord),
            KB("turtle", c.retri_turtle),
            KB("buff", c.retri_buff),
            KB("litho", c.retri_litho),
            KI("trigger", c.retri_trigger),
            KF("hp_pct", c.retri_hp_pct),
            KF("enemy_dist", c.retri_enemy_dist),
            KF("btn_x", c.retri_btn_x), KF("btn_y", c.retri_btn_y),
            KB("use_model", c.retri_use_model),
            KF("base_dmg", c.retri_base_dmg),
            KF("dmg_per_level", c.retri_dmg_per_level),
            KF("stack_dmg", c.retri_stack_dmg),
            KF("safety", c.retri_safety),
            KI("stacks", c.retri_stacks),
        })),
        KO("aim", Obj(1, {
            KB("enabled", c.aim_enabled),
            KI("mode", c.aim_mode),
            KI("priority", c.aim_priority),
            KF("range", c.aim_range),
            KF("btn_x", c.aim_btn_x), KF("btn_y", c.aim_btn_y),
        })),
        KO("combo", Obj(1, {
            KB("enabled", c.combo_enabled),
            KF("range", c.combo_range),
            KI("priority", c.combo_priority),
            KF("s1_x", c.combo_s1_x), KF("s1_y", c.combo_s1_y),
            KF("s2_x", c.combo_s2_x), KF("s2_y", c.combo_s2_y),
            KF("s3_x", c.combo_s3_x), KF("s3_y", c.combo_s3_y),
            KF("ba_x", c.combo_ba_x), KF("ba_y", c.combo_ba_y),
            KA("seq", c.combo_seq, 5),
            KI("steps", c.combo_steps),
            KI("delay", c.combo_delay),
            KI("preset", c.combo_preset),
        })),
        KO("kimmy", Obj(1, {
            KB("enabled", c.kimmy_enabled),
            KB("minion_mode", c.kimmy_minion_mode),
            KF("range", c.kimmy_range),
            KI("priority", c.kimmy_priority),
            KB("km_button", c.kimmy_km_button),
            KF("btn_x", c.kimmy_btn_x), KF("btn_y", c.kimmy_btn_y),
            KI("aim_mode", c.kimmy_aim_mode),
            KF("drag_len", c.kimmy_drag_len),
        })),
        KO("targeting", Obj(1, {
            KA("order", c.target_order, 6),
        })),
        KO("skills", Arr(1, skills)),
        KO("menu", Obj(1, {
            KB("watermark", c.watermark),
            KF("ui_scale", c.ui_scale),
        })),
    };
    return Obj(0, root);
}

// ---------------------------------------------------------------------------
// Parser
//
// A small strict JSON reader. The file is trusted input (the user wrote it or
// Rain did), but a malformed file must fail cleanly rather than apply partial
// garbage, so every parse error aborts the load and leaves the live config
// untouched.
// ---------------------------------------------------------------------------

struct Val {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Val> arr;
    std::vector<std::pair<std::string, Val>> obj;

    const Val* find(const char* k) const {
        if (t != OBJ) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

struct Parser {
    const char* p;
    const char* end;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }

    bool lit(const char* s) {
        const size_t n = strlen(s);
        if (static_cast<size_t>(end - p) < n || strncmp(p, s, n) != 0) return false;
        p += n;
        return true;
    }

    static void Utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool str(std::string& out) {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                p++;
                if (p >= end) return false;
                switch (*p) {
                    case '"':  out += '"';  p++; break;
                    case '\\': out += '\\'; p++; break;
                    case '/':  out += '/';  p++; break;
                    case 'b':  out += '\b'; p++; break;
                    case 'f':  out += '\f'; p++; break;
                    case 'n':  out += '\n'; p++; break;
                    case 'r':  out += '\r'; p++; break;
                    case 't':  out += '\t'; p++; break;
                    case 'u': {
                        if (end - p < 5) return false;
                        unsigned cp = 0;
                        for (int i = 1; i <= 4; i++) {
                            const char h = p[i];
                            unsigned d;
                            if (h >= '0' && h <= '9') d = static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') d = static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') d = static_cast<unsigned>(h - 'A' + 10);
                            else return false;
                            cp = (cp << 4) | d;
                        }
                        Utf8(out, cp);
                        p += 5;
                        break;
                    }
                    default: return false;
                }
            } else {
                out += *p++;
            }
        }
        if (p >= end) return false;
        p++;  // closing quote
        return true;
    }

    bool num(double& out) {
        char* q = nullptr;
        out = strtod(p, &q);
        if (q == p || q > end) return false;
        p = q;
        return true;
    }

    bool val(Val& v) {
        ws();
        if (p >= end) return false;
        switch (*p) {
            case '{': {
                v.t = Val::OBJ;
                p++;
                ws();
                if (p < end && *p == '}') { p++; return true; }
                while (true) {
                    ws();
                    std::string key;
                    if (!str(key)) return false;
                    ws();
                    if (p >= end || *p != ':') return false;
                    p++;
                    Val child;
                    if (!val(child)) return false;
                    v.obj.emplace_back(std::move(key), std::move(child));
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == '}') { p++; return true; }
                    return false;
                }
            }
            case '[': {
                v.t = Val::ARR;
                p++;
                ws();
                if (p < end && *p == ']') { p++; return true; }
                while (true) {
                    Val child;
                    if (!val(child)) return false;
                    v.arr.push_back(std::move(child));
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == ']') { p++; return true; }
                    return false;
                }
            }
            case '"':
                v.t = Val::STR;
                return str(v.str);
            case 't':
                if (!lit("true")) return false;
                v.t = Val::BOOL; v.b = true;
                return true;
            case 'f':
                if (!lit("false")) return false;
                v.t = Val::BOOL; v.b = false;
                return true;
            case 'n':
                if (!lit("null")) return false;
                v.t = Val::NUL;
                return true;
            default: {
                v.t = Val::NUM;
                return num(v.num);
            }
        }
    }
};

// ---- typed getters: apply only when the key is present and the right type ----

void GB(const Val* o, const char* k, bool& out) {
    const Val* v = o ? o->find(k) : nullptr;
    if (v && v->t == Val::BOOL) out = v->b;
}

void GI(const Val* o, const char* k, int& out) {
    const Val* v = o ? o->find(k) : nullptr;
    if (v && v->t == Val::NUM) out = static_cast<int>(v->num);
}

void GF(const Val* o, const char* k, float& out) {
    const Val* v = o ? o->find(k) : nullptr;
    if (v && v->t == Val::NUM) out = static_cast<float>(v->num);
}

void GC(const Val* o, const char* k, float c[4]) {
    const Val* v = o ? o->find(k) : nullptr;
    if (!v || v->t != Val::ARR || v->arr.size() != 4) return;
    for (int i = 0; i < 4; i++) {
        if (v->arr[i].t != Val::NUM) return;
    }
    for (int i = 0; i < 4; i++) c[i] = static_cast<float>(v->arr[i].num);
}

void GA(const Val* o, const char* k, int* a, int n) {
    const Val* v = o ? o->find(k) : nullptr;
    if (!v || v->t != Val::ARR) return;
    const int take = std::min(n, static_cast<int>(v->arr.size()));
    for (int i = 0; i < take; i++)
        if (v->arr[i].t == Val::NUM) a[i] = static_cast<int>(v->arr[i].num);
}

void ApplyCat(const Val* co, CatCfg& cc) {
    GB(co, "enabled", cc.enabled);
    GB(co, "box", cc.box);
    GB(co, "health_bar", cc.health);
    GB(co, "health_text", cc.health_text);
    GB(co, "level_tag", cc.name);
    GB(co, "hero_name", cc.hero_name);
    GB(co, "distance", cc.distance);
    GB(co, "tracer", cc.line);
    GB(co, "portrait", cc.icon);
    GB(co, "spell_cd", cc.spell_cd);
    GC(co, "color", cc.color);
    GI(co, "dead_mode", cc.dead_mode);
    GF(co, "max_dist", cc.max_dist);
}

void ApplySkill(const Val* so, SkillCfg& sk) {
    GB(so, "enabled", sk.enabled);
    GI(so, "aim_mode", sk.aim_mode);
    GF(so, "btn_x", sk.btn_x);
    GF(so, "btn_y", sk.btn_y);
    GF(so, "range", sk.range);
    GI(so, "target_mode", sk.target_mode);
    GI(so, "priority", sk.priority);
    GB(so, "cd_check", sk.cd_check);
    GF(so, "drag_len", sk.drag_len);
}

void Apply(const Val& root, Config& c) {
    if (const Val* esp = root.find("esp")) {
        GB(esp, "enabled", c.esp_enabled);
        GB(esp, "indicators", c.esp_indicators);
        GI(esp, "box_style", c.esp_box_style);
        GI(esp, "health_pos", c.esp_health_pos);
        GI(esp, "name_pos", c.esp_name_pos);
        GF(esp, "box_scale", c.esp_box_scale);
        GI(esp, "thickness", c.esp_thickness);
        GI(esp, "glow_layers", c.esp_glow_layers);
        GI(esp, "tracer_origin_mode", c.esp_line_origin_mode);
        GI(esp, "tracer_vpos", c.esp_line_vpos);
        GF(esp, "tracer_origin", c.esp_line_origin);
        GF(esp, "max_distance", c.esp_max_distance);
        GF(esp, "entity_max_distance", c.ent_max_distance);
        GC(esp, "dead_color", c.col_dead);
        if (const Val* cats = esp->find("categories")) {
            for (int i = 0; i < CAT_COUNT; i++)
                ApplyCat(cats->find(kCatNames[i]), c.cat[i]);
        }
    }
    if (const Val* rc = root.find("range_circle")) {
        GB(rc, "enabled", c.range_circle);
        GI(rc, "source", c.range_circle_source);
        GF(rc, "fixed_radius", c.range_circle_fixed);
        GF(rc, "thickness", c.range_circle_thickness);
        GC(rc, "color", c.range_circle_color);
    }
    if (const Val* al = root.find("alerts")) {
        GB(al, "enabled", c.alerts_enabled);
        GB(al, "turtle", c.alert_turtle);
        GB(al, "lord", c.alert_lord);
    }
    if (const Val* sk = root.find("skin")) {
        GB(sk, "enabled", c.skin_enabled);
        GI(sk, "skin_id", c.skin_id);
    }
    if (const Val* dr = root.find("drone")) {
        GB(dr, "enabled", c.drone_enabled);
        GF(dr, "height", c.drone_height);
        GF(dr, "fov", c.drone_fov);
        GB(dr, "hook", c.drone_hook);
    }
    if (const Val* mh = root.find("maphack")) {
        GB(mh, "enabled", c.map_enabled);
        GB(mh, "minimap_flags", c.map_minimap);
        GB(mh, "health_flags", c.map_health);
    }
    if (const Val* mm = root.find("minimap")) {
        GF(mm, "x", c.mm_x); GF(mm, "y", c.mm_y);
        GF(mm, "w", c.mm_w); GF(mm, "h", c.mm_h);
        GF(mm, "world_half", c.mm_half);
        GB(mm, "rotate45", c.mm_rotate45);
        GB(mm, "flip_x", c.mm_flip_x); GB(mm, "flip_y", c.mm_flip_y);
        GF(mm, "dot", c.mm_dot);
        GB(mm, "debug_self", c.map_debug_self);
        GB(mm, "icons", c.map_icons);
        GF(mm, "icon_size", c.mm_icon_size);
        GB(mm, "names", c.map_names);
        GB(mm, "hp_ring", c.map_hp_ring);
        GF(mm, "ring", c.mm_ring);
        GB(mm, "show_allies", c.map_show_allies);
        GB(mm, "show_monsters", c.map_show_monsters);
        GB(mm, "show_minions", c.map_show_minions);
    }
    if (const Val* rt = root.find("retribution")) {
        GB(rt, "enabled", c.retri_enabled);
        GB(rt, "lord", c.retri_lord);
        GB(rt, "turtle", c.retri_turtle);
        GB(rt, "buff", c.retri_buff);
        GB(rt, "litho", c.retri_litho);
        GI(rt, "trigger", c.retri_trigger);
        GF(rt, "hp_pct", c.retri_hp_pct);
        GF(rt, "enemy_dist", c.retri_enemy_dist);
        GF(rt, "btn_x", c.retri_btn_x); GF(rt, "btn_y", c.retri_btn_y);
        GB(rt, "use_model", c.retri_use_model);
        GF(rt, "base_dmg", c.retri_base_dmg);
        GF(rt, "dmg_per_level", c.retri_dmg_per_level);
        GF(rt, "stack_dmg", c.retri_stack_dmg);
        GF(rt, "safety", c.retri_safety);
        GI(rt, "stacks", c.retri_stacks);
    }
    if (const Val* am = root.find("aim")) {
        GB(am, "enabled", c.aim_enabled);
        GI(am, "mode", c.aim_mode);
        GI(am, "priority", c.aim_priority);
        GF(am, "range", c.aim_range);
        GF(am, "btn_x", c.aim_btn_x); GF(am, "btn_y", c.aim_btn_y);
    }
    if (const Val* cb = root.find("combo")) {
        GB(cb, "enabled", c.combo_enabled);
        GF(cb, "range", c.combo_range);
        GI(cb, "priority", c.combo_priority);
        GF(cb, "s1_x", c.combo_s1_x); GF(cb, "s1_y", c.combo_s1_y);
        GF(cb, "s2_x", c.combo_s2_x); GF(cb, "s2_y", c.combo_s2_y);
        GF(cb, "s3_x", c.combo_s3_x); GF(cb, "s3_y", c.combo_s3_y);
        GF(cb, "ba_x", c.combo_ba_x); GF(cb, "ba_y", c.combo_ba_y);
        GA(cb, "seq", c.combo_seq, 5);
        GI(cb, "steps", c.combo_steps);
        GI(cb, "delay", c.combo_delay);
        GI(cb, "preset", c.combo_preset);
    }
    if (const Val* km = root.find("kimmy")) {
        GB(km, "enabled", c.kimmy_enabled);
        GB(km, "minion_mode", c.kimmy_minion_mode);
        GF(km, "range", c.kimmy_range);
        GI(km, "priority", c.kimmy_priority);
        GB(km, "km_button", c.kimmy_km_button);
        GF(km, "btn_x", c.kimmy_btn_x); GF(km, "btn_y", c.kimmy_btn_y);
        GI(km, "aim_mode", c.kimmy_aim_mode);
        GF(km, "drag_len", c.kimmy_drag_len);
    }
    if (const Val* tg = root.find("targeting")) {
        GA(tg, "order", c.target_order, 6);
    }
    if (const Val* sk = root.find("skills")) {
        if (sk->t == Val::ARR) {
            const int take = std::min(static_cast<int>(sk->arr.size()),
                                      static_cast<int>(SLOT_COUNT));
            for (int i = 0; i < take; i++)
                if (sk->arr[i].t == Val::OBJ) ApplySkill(&sk->arr[i], c.skill[i]);
        }
    }
    if (const Val* mn = root.find("menu")) {
        GB(mn, "watermark", c.watermark);
        GF(mn, "ui_scale", c.ui_scale);
    }
}

std::string PathFor(const char* name, const char* ext) {
    std::string p = DirImpl();
    p += '/';
    p += name;
    p += ext;
    return p;
}

}  // namespace

const char* Dir() {
    // Function-local static: C++11 guarantees thread-safe initialisation, and
    // the value is constant afterwards.
    static const std::string d = DirImpl();
    return d.c_str();
}

bool NameOk(const char* name) {
    if (!name || !*name) return false;
    const size_t len = strlen(name);
    if (len > 48) return false;
    if (name[0] == '.') return false;
    if (strstr(name, "..")) return false;
    for (const char* p = name; *p; p++) {
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (ch < 0x20 || ch == 0x7F) return false;
        if (ch == '/' || ch == '\\') return false;
    }
    return true;
}

int List(char names[][64], int max) {
    EnsureDir();
    DIR* d = opendir(Dir());
    if (!d) return 0;

    std::vector<std::string> found;
    while (struct dirent* e = readdir(d)) {
        const char* n = e->d_name;
        const size_t len = strlen(n);
        if (len > 5 && strcmp(n + len - 5, ".json") == 0) {
            std::string stem(n, len - 5);
            if (NameOk(stem.c_str())) found.push_back(std::move(stem));
        }
    }
    closedir(d);

    std::sort(found.begin(), found.end());
    int n = 0;
    for (const auto& s : found) {
        if (n >= max) break;
        snprintf(names[n], 64, "%s", s.c_str());
        n++;
    }
    return n;
}

bool Save(const Config& cfg, const char* name) {
    if (!NameOk(name)) return false;
    EnsureDir();

    const std::string doc = Emit(cfg);
    const std::string path = PathFor(name, ".json");
    const std::string tmp = PathFor(name, ".json.tmp");

    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        LOGI("cfgjson: cannot open %s for write", tmp.c_str());
        return false;
    }
    bool ok = fwrite(doc.data(), 1, doc.size(), f) == doc.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        RAIN_UNLINK(tmp.c_str());
        return false;
    }
    // Atomic swap: a process death mid-write leaves the previous file intact.
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        RAIN_UNLINK(tmp.c_str());
        return false;
    }
    LOGI("cfgjson: saved '%s' (%zu bytes)", name, doc.size());
    return true;
}

bool Load(Config& cfg, const char* name) {
    if (!NameOk(name)) return false;

    const std::string path = PathFor(name, ".json");
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    std::string doc;
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) doc.append(buf, r);
    fclose(f);

    Parser ps{doc.c_str(), doc.c_str() + doc.size()};
    Val root;
    if (!ps.val(root) || root.t != Val::OBJ) {
        LOGI("cfgjson: '%s' is not valid JSON, ignored", name);
        return false;
    }

    // All-or-nothing: parse first, apply second, so a truncated file cannot
    // half-apply over the live config.
    Apply(root, cfg);
    LOGI("cfgjson: loaded '%s' (%zu bytes)", name, doc.size());
    return true;
}

bool Delete(const char* name) {
    if (!NameOk(name)) return false;
    const std::string path = PathFor(name, ".json");
    if (RAIN_UNLINK(path.c_str()) != 0) return false;
    LOGI("cfgjson: deleted '%s'", name);
    return true;
}

}  // namespace cfgjson
