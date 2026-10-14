# v45.2 — jungle ESP, standalone alerts, tracer anchor (2026-10-14)

Three requested fixes in one build.

## 1. Jungle ESP — the monster ID table was wrong

The old table used the reference mod's documented ids (`2002` Lord, `2003`
Turtle, `2004/2005` Buff, `2056/2072` Litho). A live practice match shows the
game now uses a **completely different id space** — `1007-1016` plus
`2093/2094` — so every jungle category classified as unknown and nothing drew.
Captured live, keyed by HP and map position:

| Id | HP | Position | Camp |
|---|---|---|---|
| 1009 | 7900 | (-41.2, 0) | Turtle (top-left pit) |
| 1010 | 7900 | (41.2, 0) | Lord (bottom-right pit) |
| 1015/1016 | 7300 | (±35.5, ±11) | Blue rune buff |
| 1013/1014 | 5700 | (±26.5, ±21.5) | Red rune buff |
| 1007/1008 | 4500 | (±11.2, ±35.9) | Lithowanderer / small camps |
| 2093/2094 | 2183 | (±31.9, ±6.1) | Crab's Treasure |
| 1011/1012 | 40000 | (±52, 0) | base crystals — not jungle, unmapped |

The buff colour split is inferred from distance-to-base (blue = inner camp).
If a live match shows them swapped, it is a two-line swap in the table.

**New ESP rows**: the old single `buff` row is now `blue_buff`, `red_buff`,
and `litho` — each with its own colour, toggle, and height. Existing profiles
migrate automatically: the old `buff` key is read as a fallback for both buff
rows when the new keys are absent.

## 2. Alerts — a standalone feature

Alerts were a two-toggle add-on to the ESP section. They are now their own
Visual-tab section, independent of ESP and the minimap:

* per-objective toggles: **Turtle, Lord, Blue buff, Red buff, Lithowanderer**
* **hold time** slider (1–15 s) — how long the banner stays after the last hit
* **text scale** slider (0.5–2.5x)
* banners use each objective's ESP colour

The buff/litho banners track the lowest-HP live member of each camp and fire
on its HP dropping, exactly like the turtle/lord banners.

## 3. Tracer — anchor and draw order

Two defects:

* The line ended at the entity's **feet**, which on a tall hero reads as
  pointing at the ground under them. It now ends at the **body centre** by
  default, with a per-user choice: **Feet / Body / Head** (new setting
  `esp.tracer_anchor`).
* The line was drawn before the box computed its geometry; the draw order is
  now tracer → box → labels, so the box sits on top of the line.

## Verified

* Config round-trip on device: the new fields persist —
  `alerts.{blue_buff,red_buff,litho,window,scale}`, categories
  `{blue_buff,red_buff,litho}` present, old `buff` key migrated away,
  `tracer_anchor` saved.
* Menu shows the new Tracer "Ends at" row and the expanded alerts section.
* Live match: monster ids captured and classified (the previous build logged
  them; this build maps them).

## Known limits

* The blue/red buff colour assignment is inferred, not visually confirmed in
  this session (the practice AI kept the buffs out of reach). Both are rune
  buffs and both draw; only the colour label could be swapped.
* The alert banners were not observed firing on a buff in this session —
  the HP-drop plumbing is the same code path as the verified turtle/lord
  banners, but a live buff steal is the confirming test.
