# Release versioning

**Small updates get minor bumps, not major ones.**

| Update size | Version format | Example |
|---|---|---|
| 1–5 small changes (title tweak, wording, single fix) | `MAJOR.MINOR` — minor bump | v45.1, v45.2 |
| Big feature set / new subsystem / major rework | `MAJOR` — major bump | v46, v47 |

A one-line title change is `45.1`, **not** `46`. Reserve whole-number bumps
for updates that genuinely add features or rework the build.

When unsure, default to the minor bump.

---

*Retroactive note: the v43.3 → v44 → v45 sequence was published before this
convention was set. v45 (the @rainhook title change) should have been v44.1.
Starting from v45, the table above applies.*
