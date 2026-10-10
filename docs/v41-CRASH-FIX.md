# v41 — Second-Match Crash Fix

**Date:** 2026-10-08
**Symptom:** first match fine; second match (ranked or practice) crashes at the
end of the loading screen.
**Cause:** use-after-free I introduced in v40.
**Status:** fixed, deployed, verified with two back-to-back matches.

---

## The bug

v40 added a ShowPlayer check so the new KDA/gold/bounty targeting priorities had
data. The check needs the object's class pointer, and I wrote it to call the
IL2CPP accessor on the **live** object:

```cpp
// v40 — WRONG
bool IsShowPlayerObject(void* o) {
    Il2CppClass* k = api.object_get_class(reinterpret_cast<Il2CppObject*>(o));
    ...
}
...
if (IsShowPlayerObject(o)) {      // 'o' is the LIVE pointer
```

`il2cpp_object_get_class` dereferences the pointer it is given. That is exactly
the raw dereference the project's peek boundary exists to prevent — and the
comment directly above the function even said *"done on the already-peeked
copy"*. I wrote the intent and passed the wrong variable.

## Why it only crashed on the second match

The peek boundary (`process_vm_readv` into a stack copy) is what makes every
other read safe: on a freed or unmapped page it returns `EFAULT` and the entity
is skipped. A raw dereference instead takes the SIGSEGV.

`Fill()` is called per entity inside a 256-slot loop, during
`SnapshotPlayers`. On the **first** match the entities are freshly created and
alive, so the raw read lands on valid memory. Between matches the game tears the
battle scene down and rebuilds it, and the entity list briefly still holds
entries the game has already destroyed. Match 2's load walks that list:

```
match 1 ends → teardown → queue match 2 → load walks a list with dead entries
                                        → raw deref on a freed object → SIGSEGV
```

That is why it was first-game-clean and second-game-fatal, and why a single
practice match never reproduced it.

## The fix

Read the class pointer from the peeked copy. An `Il2CppObject`'s first member
*is* its `Il2CppClass*`, and that member is already inside the local buffer, so
no live memory is touched at all:

```cpp
// v41 — correct
bool IsShowPlayerObject(const void* img) {
    if (!g_show_player_class || !img) return false;
    uintptr_t k = 0;
    memcpy(&k, img, sizeof(k));          // class ptr from the COPY
    if (!PtrOk(k)) return false;
    for (Il2CppClass* cur = reinterpret_cast<Il2CppClass*>(k); cur;
         cur = api.class_get_parent ? api.class_get_parent(cur) : nullptr) {
        if (cur == g_show_player_class) return true;
    }
    return false;
}
...
if (IsShowPlayerObject(img)) {           // the peeked copy
```

The `PtrOk` gate rejects a garbage class pointer before the parent walk, so a
corrupt entry degrades to "not a ShowPlayer" instead of walking a bad chain.

Signature changed `void*` → `const void*` so the compiler rejects passing the
live object again — the mistake cannot be repeated by accident.

## Verification

Two full matches back to back, same process throughout:

| | match 1 | match 2 |
|---|---|---|
| process PID | 25443 | **25443** (never restarted) |
| result | VICTORY (surrender) | loaded clean, played |
| ESP | `n=2 local=1 w2s=1` | `n=2 local=1 w2s=1` |
| faults | none | none |

The process starttime is identical across both matches, which is the proof: a
crash would have produced a new PID (or no process at all).

Deployed as `comlibs/arm64-v8a/librudp.bytes`, md5
`20b220f355ed7bc70abe621937f992d8` (`build/v41/librain.so`).
Profile unchanged: 7 deps, BIND_NOW, 2 exports, stripped.

## Same-class risk found but not changed

`ClassifyContainer` (game.cpp, pre-existing from v38) also calls
`api.object_get_class` on a live pointer. It is lower risk than the v40 bug
because it dereferences a *container* object read from the manager rather than
per-entity objects in a 256-slot loop during teardown — one object, long-lived,
not part of the churn. It is the same category of hazard, though, and worth
converting to a peeked read the next time that code is touched.

## Lesson for the codebase

The peek boundary is only a guarantee if **every** access goes through it. The
original design was correct and the v40 addition silently punched a hole in it.
Any future helper that needs something from a managed object should take the
peeked buffer, not the live pointer — and the `const void*` signature change
here is the pattern to copy.
