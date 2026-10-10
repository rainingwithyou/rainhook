# MLBB Deep Analysis — Round 6 (2026-10-07)

Full-workspace deep pass on the physical phone (Redmi 2607DRA18G, Android 16,
MLBB **2.2.16.12322**), covering: the game's AI engine (BBAI), the behaviour
system, the asset container format, the integrity model correction, fresh APK
analysis, and live verification of Rain v38.

Companion documents produced this round:
- `analysis/BBAI-DEEP-DIVE.md` — the 91 MB AI engine
- `analysis/MLBB-BEHAVIOR-AI-SYSTEMS.md` — misconduct/AFK/AI bridge (C# side)
- `analysis/MLBB-CONTAINER-FORMAT.md` — the "MLBB" archive format
- `analysis/HERO-ICONS-SOLVED.md` — hero portraits via the game's own atlas
- `analysis/VYNIX-DEEP-DIVE.md` — full VYNIX decode (license endpoint recovered)
- `analysis/XNISH-DEEP-DIVE.md` — full XNISH decode (96 AC functions neutralized)
- `analysis/mlbb_container.py` — parser (stdlib only)

---

## 1. Headline findings

### 1.1 The integrity model is NOT what rounds 1–5 assumed

Rounds 1–5 concluded: *"`librudp.bytes` is not covered by the game's patch list,
so replacing it is stable."* **That is now wrong on 2.2.16.**

The server-delivered `Document/android/BinaryPatchMD5.xml` (55 entries) **does
contain**:

```xml
<item name="comlibs/arm64-v8a/librudp.bytes"
      md5="51bac6f79e3e13c5823e44b352686fb4" xxhash="ded282"/>
```

That md5 is the **original** `librudp` (verified: it matches
`analysis/device_2026-09-28/.../original_libs/librudp.bytes`). The file staged
on the phone right now is **Rain v38**, md5 `04acac198c430e46b20f052373bdc193`
— a mismatch — and Rain loads and runs fine (verified live, §5).

The list covers every architecture (`arm64-v8a`, `armeabi-v7a`, `x86`) and
includes `libAkSoundEngine`, `libcsharp`, `libfirst`, `libhttp_downloader`,
`liblogic`, `libmlsdk_logreport`, `libmoba`, `libmola_download`,
`libnativelogin`, `librudp`.

**Why Rain survives anyway** — the observed behaviour plus the known machinery
point to a download/repair-time check, not a continuous runtime verify:

- `TransferFileCheck` (`bCheckMd5` / `_checkmd5Thread`) is the download
  pipeline; it verifies files it is *fetching or repairing*.
- The game also carries **report-only** integrity paths —
  `IShowStruct_ISHOW_MappingMd5NotMatch`, `ISHOW_OnMD5Failed`,
  `ISHOW_TryUploadMD5Log`, `RecordMD5Writer` — i.e. a mismatch produces a
  *report*, not a block.
- A staged file that already exists is not re-downloaded, so the check never
  fires on it.

**What this changes:** the deployment is still stable *in practice*, but it is
now a **server-controlled** property, not a structural guarantee. If a future
server config marks `librudp` for a mandatory repair/verify pass, Rain's slot
gets re-fetched. The honest statement is: *"works until the server says
otherwise"* — and there is no client-side way to prevent that.

**Actionable:** the earlier "blind spot" reasoning in `docs/RAIN.md` and the
round 2–5 docs should be corrected. The `librudp` slot is *covered*; it is not
*checked on this install*.

### 1.2 BBAI — a 91 MB AI engine nobody had looked at

`libmobile_bbai.bytes` is Moonton's on-device AI system (source tree
`/root/mobile_mlai_build/mobile_bbai/mlai/`), built on ByteDance's ByteNN
runtime. It implements:

- **Matchmaking** with five strategies incl. `MatchByWinrateModel`
  (winrate tracking, `boost_player_contribution`, `checkWinRateTrendChange`)
- **Dynamic bot difficulty** — `tuneAIDifficulty`, rank→difficulty mapping,
  time→economy mapping, and `checkTeammateRobotShouldDownDiff` (your *teammate*
  bot gets easier on purpose)
- **AI surrender** — `SetNeedReqWinRateModelForAISurrender`
- **Human-AI Cooperation (`hac/`)** — AI teammates send filtered quick-signals
  and emojis; `style_mgr` gives them playstyles
- **Highlight detection** — 20+ features like `do_not_gank`, `penta_feed`,
  `intent_attack_lord`, `push_highground`
- **33 per-hero rule files** incl. `hero_rule_kimmy.cpp` +
  `hero_rule_kimmy_rotate.cpp` (the game itself special-cases Kimmy's held
  attack — the mechanism Rain's Kimmy hold rides)

Full detail in `BBAI-DEEP-DIVE.md`.

### 1.3 The behaviour/misconduct system defines "suspicious"

`BattleLogic.AI.DL` (57 types in the metadata) contains the game's behaviour
scoring: `AfkEvaluator`, `InterferenceBuffEvaluator` (buff stealing),
`JungleNoShoesEvaluator`, `RoamNoShoesEvaluator`, `IllegalEquipsEvaluator`,
`LogicLyingFlatStateCollector` (躺平 "lying flat" = giving up), and
`MisconductWindowCounter` tracking attacks/skills/teleports/**position history
in centimetres**/distance run/damage-by-target-class per window.

Reports go to `NetworkMisconductSink` (server) — enforcement is server-side.

Full detail in `MLBB-BEHAVIOR-AI-SYSTEMS.md`.

### 1.4 The asset container format is decoded

`*.unity3d` files under `Document/` are **not** Unity bundles — they are
Moonton's own `MLBB`-magic archives. Format reverse-engineered and a parser
written (`analysis/mlbb_container.py`). `BattleConfig.unity3d` yields 984 named
blobs (Skill/Bullet/Trigger/Effect tables); `Document.unity3d` yields 923
(Hero/Monster/Guide/DirtyWords/…).

Inner blobs use a `$mulonggame_table_1` table format. `AIParamConfig.xml` — one
of the inner entries — is **plaintext** and contains the AI's complete danger
model (§4).

Full detail in `MLBB-CONTAINER-FORMAT.md`.

### 1.5 VYNIX license infrastructure fully recovered

The XOR pool was mis-characterized in rounds 1–5. It is **303 strings under key
0x5A plus exactly one string under key 0x3A** — and that one string is the
license endpoint, stored *length-delimited* rather than NUL-terminated because
its ciphertext contains a raw `0x00`. That is why `strings` and every
NUL-scanning decoder missed it.

Verified byte-exact this round:

```
endpoint   https://pinikcitt.my.id/api/connect          (0x6c4aba, XOR 0x3A)
body       game=MLBB&version=1.0&user_key=%s&serial=%s&resource=%s
token      Vm8Lk7Uj2JmsjCPVPVjrLa7zgfx3uz9E
success    response == "NECROAJA"
time anchor http://worldtimeapi.org/api/timezone/Etc/UTC
trial gate 26 September 2026, 23:59 (banner; real gate is server-side)
```

VYNIX also installs **40 hooks** (prior docs said ~10), including the full
`Estimate*` anti-cheat neutralization family and `APKSignature.GetSignature` /
`IsSignatureSame` signature forgery.

### 1.6 XNISH neutralizes 96 anti-cheat functions

The XNISH payload's largest feature is not a cheat — it is **anti-detection**.
It patches 96 unique functions (100 stub sites) to `movz x0,#0; ret`, covering:

- `ACInterface::{CheckLib, MalApp, Verify, shadowapp, MetEntry1-3, MDLib,
  GetSignData, auth, VerifyMagic, …}`
- `Gumiho::{m_HasHook, m_HasInject, m_HasRepack, m_HasInvalidcall}`
- `DesyncLog::HasHooks`, `GPSDK::b_re_bd_hook_state`
- `ComFunc::{SelfMd5Check, Verify}`
- all 12 `MLSDK_LogReport_*` methods and every report sender

It also hooks `ShowEntity::CanSight` (force-visible — its maphack), the full
skin-unlock set (`SystemData::IsHaveSkin`, `IsHaveSkinForever`, …), and
`android::MotionEvent::copyFrom` for input synthesis. Its key format starts
with the literal `OK`; prefs file is `divarvian_sharedpref`; it **deletes** the
game's `libbattlelogic.bytes` and `libmlsdk_logreport.bytes` as anti-AC
sabotage.

The 0x5A "hits" in the old XNISH notes were padding noise — **0x7E is the only
key**. `libAkSoundEngine#.bytes` confirmed genuine Wwise 2021.1.12 SDK_MLBB
(build path `D:\wwise\Wwise_2021.1.12_SDK_MLBB\`).

### 1.7 Hero icons solved and deployed

The round 3–5 blocker (VYNIX's unknown icon→name runtime map) was the wrong
problem. The **game ships its own portrait atlas** —
`UI/android/Atlas_Hero_Head.unity3d`, a real Unity bundle with 147 128×128
`Texture2D` objects named `HeroHead000`…`HeroHead134`. The filename *is* the
`m_ID` mapping. Extracted 145 portraits, verified against both live anchors
(Zilong 16, Kimmy 71), pushed to the phone, and Rain picked them up live:

```
I/Rain: icons: loaded 144 new hero icons from
    .../files/rain_icons (144 total)
```

Full detail in `HERO-ICONS-SOLVED.md`.

The installed APK set is **byte-identical** to the 2026-10-02 pull
(base `7d9324fa…`, split `4d0c154d…`), but six additional splits that were not
previously analysed exist:

| Split | Contents |
|---|---|
| `split_Danmaku` | `com.moba.unityplugin.BarrageManager` + `master.flame.danmaku` — the in-game barrage/bullet-chat overlay |
| `split_GNASDK` | ByteDance GSDK **TTNet/Cronet** — MNA/MPA network services, diagnosis |
| `split_GSDKRTC` | ByteDance **RealX** RTC — WebRTC audio, `MediaCodecAAC{En,De}coder` (voice chat) |
| `split_QiniuShortVideo` | Qiniu short-video SDK with a **screen-recording service** (`RecordScreenService`, `MediaProjection`) |
| `split_SDKVK` | VK SDK — `com.vk.api.sdk` (social login/share) |
| `split_PlayAssetPack` | empty stub (manifest only) |

New permissions confirmed on base: `FOREGROUND_SERVICE_MEDIA_PROJECTION` (the
Qiniu screen recorder), `DETECT_SCREEN_CAPTURE`, plus the usual set.

---

## 3. Live verification (2026-10-07, 20:15 local)

Brought MLBB to the foreground and watched Rain boot:

```
Rain: constructor entry
il2cpp: images now 70
il2cpp: bound class UnityEngine::Input
input: UnityEngine.Input bound
il2cpp: bound class ::ShowEntity
game: entity name field = m_RoleName
game: ShowEntity fields hp=420 hpMax=424 camp=208 guid=392 id=396
      type=120 pos=652 radius=824 minimap=679
il2cpp: bound class ::BattleManager / LogicBattleManager / ShowBattleCamera
il2cpp: bound class UnityEngine::Camera / Transform
game: Unity Camera bound (transform=0x775f2c80d0)
overlay: ImGui ready, surface 1968x908
game: monsters field ok=1 list=0x7407debc60 kind=0 count=0
```

Every class bound on 2.2.16 **with the same offsets as the v37 phone session** —
the no-hardcoded-offsets design holds across builds.

**Menu verified by screenshot** (not just logcat): launcher tapped at physical
(135, 613) opened the window; all five tabs render —

- **Visual**: ESP master, Status markers, Draw dead, box style (2D/Outline/Glow),
  scale, thickness, health-bar position, nametag position, tracer origin,
  distance caps
- **Combat**: Targeting (class priority with Up/Down), Basic-Attack Assist,
  Skill Auto AIM, Kimmy M1 Aura, Auto Combo, Auto Retribution, Calibration
- **Map**: minimap overlay, markers, hero portraits, dot size, health ring,
  name labels, include filters, Minimap Calibration, Maphack (engine flags)
- **Status**: binding state, `Local camp: -1 (logic: -1)`, `Surface: 1968x908`,
  `Overlay: installed`, entity counts, Retribution estimate, Objectives,
  Entity table, Detection surface, Hero icons, Battle records
- **Settings**: present

Screenshots: `analysis/live_game_1007.png`, `live_menu_1007b.png`,
`live_combat3_1007.png`, `live_status2_1007.png`.

**Deployment verified:** staged `librudp.bytes` md5 == `build/v38/librain.so`
md5 (`04acac19…`). `build/verify/librain.so` is the same file and **has a
CMakeCache** — that is the reproducible v38 build directory.

**Note on the tap-through:** tapping a menu control also sends that tap to the
game underneath (documented behaviour — no input hook). This was observed live:
a sidebar tap also opened the game's Koleksi screen behind the menu.

---

## 4. AI configuration tables (plaintext)

`Document.unity3d` → `AIParamConfig.xml` gives the game's **complete AI danger
model**. Selected values:

**Danger weights** (`AHeroDangerWeight`, limit 200):
- Be attacked by tower: +100 (comment says n×60, value is 100)
- Each enemy hero nearby: +10
- Each enemy hero attacking you: +50

**Personality coefficient** — cautious `0.8` / bold `1.2` / normal `1.0`

**Role coefficient** (how much danger each role feels):
tank 0.7 · fighter 0.8 · assassin 0.9 · mage 1.2 · marksman 1.3 · support 1.0

**Role danger cap**: tank 130 · fighter 120 · assassin 110 · mage 100 ·
marksman 100 · support 110

**HP→danger**: 85%→50 · 70%→60 · 50%→70 · 25%→90 · 10%→100 · 5%→110

**Target selection** (`ASelTargetPriority`): distance weight max 100 (+10 per
unit of distance), role priority tank 10 · fighter 12 · assassin 15 · mage 15 ·
marksman 20 · support 11; each enemy near the target −10 to −40; each ally near
+10 to +40.

**Attack thresholds**: attack a hero at ≤50% HP; go "crazy attack" at ≤15%.

**Retreat**: cancel recall if an enemy is within 12 units; move home within 10.

**Behaviour trees** (`ATreeName`): 46 pre-created trees — `RankAI` splits into
`RankAI_Kill / Poke / Defense / GoSupport`; `RankAloneAI_Retreat`,
`_RetreatByTower`, `_Support`, `_Forward`; a `HighGuideAI6803_Alucard_Feeding`
variant (an AI that feeds, for tutorials); `Summon_Wukong_AI` etc.

**Grass positions**: 4 bushes at (−15,4), (−3,−4), (15,−4), (3,4).

**Hero-specific**: hero 25 gets a skill remap 20000→20002;
hero 125 (Zhuxin) has summon-move skill ids 12502/12511/12510.

Also plaintext in the same container: `HeroCombo.bytes` (35 hero→combo-id
entries — the game's own combo registry), `AIName.bytes` (bot name pools:
human-style "Austin/Daisy/Mark" and gamer-style "ShadowPulse/MemeLord"),
`DirtyWords_*.xml` (chat filter, by region: dfjz/vng/…).

---

## 5. What all of this means for Rain

1. **The `librudp` slot is now server-controlled** (§1.1). Nothing to change in
   code, but the docs' stability claim must be corrected, and a fallback
   deployment (rinject hot-attach) is worth keeping current.

2. **The detection surface is behavioural.** BBAI's grading
   (`repeat_skill`, `ControlStillTime`) and the misconduct system
   (`MisconductWindowCounter`) watch *play patterns*. Rain's automation
   (aim/combo/retri/Kimmy) is exactly the kind of input those windows measure —
   which is why default-off + individual toggles is the right posture. ESP /
   minimap / drone generate no input and sit outside this surface entirely.

3. **The game's own AI special-cases Kimmy** (`hero_rule_kimmy.cpp` +
   `_rotate`) — independent confirmation that held-attack is a real engine
   path, which is what Rain's Kimmy feature relies on.

4. **The AI sees the same entity fields Rain reads.** Every ESP field
   (hp/hp_max/pos/camp/radius/type) is a BBAI model input feature
   (`feat_creeps/`, `feat_heros/`). That means the game is doing heavy
   per-frame work on the same objects — worth remembering if a future Rain
   feature adds hooks that touch those structures.

5. **`AIParamConfig.xml` is free calibration data.** The real danger weights,
   retreat distances, and attack thresholds are in plaintext — useful if Rain
   ever wants an "AI is about to retreat/engage" readout, or to tune the
   auto-retri enemy-proximity guard to match how bots actually behave.

6. **The container parser opens the whole config tree** — 372 `*.unity3d` in
   `Document/` alone. `Skill_*.bytes` decode would give the game's real skill
   parameters (damage, range, cooldown), removing the guesswork from Rain's
   retribution model and XNISH-style auto-retri.

---

## 6. Open items (updated)

1. **Correct the `librudp` integrity claim** in `docs/RAIN.md` and rounds 2–5
   (§1.1). The slot is covered by the server patch list; it survives because
   the check is download/repair-time (and/or report-only), not continuous.
2. **Keep `rinject` current** as the fallback if a future server config forces
   a repair — it hot-attaches to a running game, no force-stop.
3. **Decode `Skill_*.bytes`** from BattleConfig to replace retribution
   coefficients with real values.
4. **Hero icons: DONE** — deployed and live (§1.7). Re-run the extractor after
   a game update to pick up new heroes.
5. **git init + .gitignore** — still absent.
6. **BBAI `/Main/PredictSignature`** — the AI models are signature-checked;
   interesting as an example of the game's real integrity pattern, and worth
   remembering if Rain ever ships data files.
7. **Consider the AC surface Rain does not touch.** XNISH neutralizes 96 AC
   functions to survive; Rain touches none of them and writes no reports. That
   is a real architectural advantage and worth keeping — do not add hooks to
   `ACInterface`/`Gumiho`/`DesyncLog`/report senders.
8. **`metainfo_v1` decode** — the 211-entry master index would let a tool walk
   every config table without guessing record layouts.

---

## 7. Round 6 in one paragraph

The game's hidden systems are now mapped: a 91 MB on-device AI engine (BBAI)
driving matchmaking, dynamic bot difficulty, AI surrender, and human-like
teammate communication; a behaviour-scoring pipeline that defines and reports
AFK/buff-stealing/feeding; a decoded asset container format opening 1,900+
config tables; and — most importantly for the project — proof that the
`librudp` slot **is** covered by the server patch list and survives only
because the check is download/repair-time, which corrects a core assumption the
project has carried since round 2. Rain v38 was verified live on the phone with
hero portraits newly deployed. VYNIX's license server was fully recovered and
XNISH's 96-function anti-cheat neutralization was mapped.

---

*Round 6, 2026-10-07. Phone: 5d39a8f2, MLBB 2.2.16.12322, Rain v38 staged and
verified live. Pulls: `analysis/deep8_pull_1007/` (APKs, splits, Document +
BattleConfig containers, extracted tables, hero atlas + rain_icons).*
