# Lobby Freeze — Root Cause & Fix (2026-10-11)

**Symptom:** with the drone enabled, the game froze a few seconds into the
lobby. The overlay stopped drawing, the render thread went silent, the process
stayed alive.

**Cause:** the drone's instance resolver ran a full Unity scene scan every
frame. **Fix:** v43.1 — scan only when needed, throttle it, latch hook
failures. **Verified on device.**

---

## Diagnosis

### The freeze reproduced

Two screenshots 3 seconds apart were byte-identical while the process was
alive (`ps` showed `com.mobile.legends:UnityKillsMe` running). Rain's
render-thread log stopped dead and never resumed:

```
10-11 10:13:09.222 ... Rain: game: monsters field ok=1 ... count=0
10:15:00  (current time — six minutes of silence)
```

### The A/B test

The only unusual thing in the config was the drone:

```json
"drone": { "enabled": true, "height": 0, "fov": 32, "hook": false }
```

| Config | Result |
|---|---|
| Drone **on** (fov 32, height 0, hook off) | Froze within ~30 s of lobby |
| Drone **off** | Alive 2+ minutes, Rain logging continuously |
| Drone **on** again | Reproduced the stall |

### The code

`game.cpp`, `ApplyDrone()`:

```cpp
// Class/field lookup retries while unbound (SmoothFollow arrives with the
// battle scene); the instance lookup is throttled harder because it is a
// scene scan.
static int resolve_age = 1000;
if (!g_sf_class_done && resolve_age++ > 120) { ... }
ResolveDroneInstance();          // <-- every frame, unthrottled
```

`ResolveDroneInstance()` runs `FindObjectOfType(SmoothFollow)` — a full scene
walk — whenever no instance is cached:

```cpp
Il2CppClass* obj_cls = FindClass("UnityEngine", "Object");
const MethodInfo* m_find = api.class_get_method_from_type(...);
void* inst = api.runtime_invoke(m_find, nullptr, params, &exc);
```

The comment claimed the lookup was "throttled harder". It was not throttled at
all.

**Why the lobby specifically:** `SmoothFollow` is a battle-scene component. In
the lobby there is no instance, so every frame scanned the entire scene, found
nothing, and scanned again next frame. The render thread starved — the log
`sf=0` in every drone line was the scan failing, 60 times a second.

---

## The fix (v43.1)

Only the **field-write fallback** needs an instance. The hook path works from
the class's `MethodInfo`, and the FOV path never touches `SmoothFollow`. So:

```cpp
// Only the field-write fallback needs an instance: the hook path works from
// the class's MethodInfo alone, and the FOV path never touches SmoothFollow.
const bool want_instance = (g_cfg.drone_height > 0.01f) &&
                           !(g_cfg.drone_hook && g_drone_hook_ready);
if (g_sf_inst) {
    ResolveDroneInstance();          // liveness re-check: a peek, not a scan
} else if (want_instance) {
    static int scan_age = 1000;
    if (scan_age++ > 300) {          // ~5 s at 60 fps
        scan_age = 0;
        ResolveDroneInstance();
    }
}
```

Plus a failure latch on the hook, which had the same shape of problem — a
build where `set_offSetPos` cannot be hooked would retry the method lookup and
the executable-range scan every frame:

```cpp
bool g_drone_hook_failed = false;    // non-transient failure: stop retrying
```

The menu now reports `unavailable on this build` instead of `not installed`
when that latch is set.

---

## Verification

| Check | Result |
|---|---|
| Old config (drone on, fov 32, height 0, hook off) | **2.5+ minutes stable** — previously froze in ~30 s |
| Screen updating | Confirmed via screenshot diff at 4 s intervals |
| Rain logging | Continuous (`10:39:06` vs current `10:39:07`) |
| Overlay responsive | Menu opens (full sidebar), watermark draws |
| Binary hygiene | 7 deps, BIND_NOW, 2 exports, stripped |

Shipped as **v43.1**, both ABIs, md5 `cd3ae999…` (arm64) / `a539e3e5…` (v7a).

---

## Lesson

The drone's three paths have very different costs:

| Path | Per-frame work |
|---|---|
| FOV (zoom) | one camera invoke — cheap, no instance needed |
| Hook (height, preferred) | nothing; the detour runs in the game's own code |
| Field write (height, fallback) | needs an instance → scene scan if missing |

The bug was treating the third path's need as universal. A cheap gate
(`want_instance`) plus a throttle turns a per-frame scene walk into one scan
per five seconds, and only in the case that actually consumes the result.

The same pattern is worth checking anywhere a resolver is called from a
per-frame path: **if the lookup can fail, assume it will fail every frame
until it succeeds.**
