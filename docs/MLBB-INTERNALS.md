# MLBB Internals — Reverse Engineering Notes

Target: `com.mobile.legends` (global MLBB), arm64-v8a running under LDPlayer's
Houdini ARM→x86_64 translation on an x86_64 host.

## 1. Module layout

MLBB splits itself into three IL2CPP *modules*, each a native lib plus its own
metadata blob. Names are resolved **across** modules via tagged indices.

| module tag | native lib | metadata | C# assembly | types |
|---|---|---|---|---|
| 1 | `liblogic.so` (26.3 MB) | `global-metadata.dat` | — | — |
| 2 | `libfirst.so` (9.6 MB) | `global-first-metadata.dat` | — | — |
| 3 | `libcsharp.so` (99.3 MB) | `global-csharp-metadata.dat` | `Assembly-CSharp.dll` | 20 003 |

Extra images in the csharp module: `GameEntryAOT.dll` (14 types),
`HybirdAOT.dll` (358 types).

Hot-updated libs live in `/data/data/com.mobile.legends/app_libs/`
(`liblogic.so`, `libfirst.so`, `libcsharp.so`, `librudp.so`, `libnativelogin.so`,
`libAkSoundEngine.so`, …). They are loaded by MLBB's **own loader**, not the
dynamic linker — visible in `/proc/<pid>/maps` at low guest addresses
(`liblogic.so` @ `0x07094000`, `libfirst.so` @ `0x09094000`, `librudp.so` @
`0x0a094000`) while `libcsharp.so` sits at a normal high address.

`libil2cpp.so` (384 KB) is only a stub; **`liblogic.so` exports the real
IL2CPP runtime API — 275 `il2cpp_*` functions**, including the full reflection
surface used by Rain:

```
il2cpp_domain_get, il2cpp_domain_get_assemblies, il2cpp_domain_assembly_open,
il2cpp_assembly_get_image, il2cpp_image_get_name, il2cpp_image_get_class,
il2cpp_image_get_class_count, il2cpp_class_from_name,
il2cpp_class_get_name, il2cpp_class_get_namespace,
il2cpp_class_get_field_from_name, il2cpp_class_get_fields,
il2cpp_class_get_method_from_name, il2cpp_class_get_methods,
il2cpp_field_get_name, il2cpp_field_get_offset, il2cpp_field_get_type,
il2cpp_field_get_value, il2cpp_field_set_value, il2cpp_field_static_get_value,
il2cpp_field_static_set_value, il2cpp_method_get_name,
il2cpp_object_new, il2cpp_string_new, il2cpp_runtime_invoke, ...
```

`libfirst.so` also exports game-framework natives: `AgentObjBase_*`,
`BlackboardVar_*`, `MTSandBox_*`, `MTApplication_*`, `NGUIText_*`, `MTCamera`,
`MTTransform`, `MLBBPipelineFactory`, `MTAutoInjector`, `BattleMSAAInjector`.

Protection stack shipped in `lib/arm64/`: `libshadowhook.so`, `libbytehook.so`,
`libnpth_*.so` (ByteDance NPTH), `libsigner.so`, `libEncryptor.so`,
`libmonitorcollector-lib.so`, `libbytebench.so`, `libResources_z*.so`.

## 2. Metadata format (custom)

Stock IL2CPP metadata, modified:

* `sanity = 0xFAB11BAF`, **`version = 1024`** (not a stock revision).
* Section table = a list of **`(size, cumulative_end)` pairs**, not `(offset, size)`.
  Section *k* spans `[cum_end_k - size_k, cum_end_k)`.
* One extra 3-byte section at index 0 shifts the stock order by **+1**:

```
  0  (3-byte header tail)
  1  stringLiteral          2  stringLiteralData       3  string  <- identifier table
  4  events                 5  properties              6  methods
  7  parameterDefaultValues 8  fieldDefaultValues      9  fieldAndParameterDefaultValueData
 10  fieldMarshaledSizes   11  parameters             12  fields
 13  genericParameters     14  genericParameterConstraints
 15  genericContainers     16  nestedTypes            17  interfaces
 18  vtableMethods         19  interfaceOffsets       20  typeDefinitions
 21  images                22  assemblies             23  fieldRefs
 24  referencedAssemblies  25  attributeInfo          26  attributeTypes
 27  unresolvedVirtualCallParameterTypes
 28  unresolvedVirtualCallParameterRanges
```

* **Tagged indices** — the core customisation. Every string/table index is:

```
    value = (module_tag << 24) | offset
    module_tag: 1 = logic, 2 = first, 3 = csharp
    -1 = absent
```

  Verified: tag 1 resolves cleanly only in the logic table (4806/4806),
  tag 2 only in first (705/705), tag 3 only in csharp (54489/54489).
  Tagged references are used for class/field/method **names** *and* for
  cross-module **table** indices (`fieldStart`, `methodStart`, `typeStart`).

* `Il2CppTypeDefinition` here is **92 bytes** (stock is 88 — one extra int32):

```
  i32[0] nameIndex        i32[1] namespaceIndex     i32[2] byvalTypeIndex
  i32[3] declaringTypeIndex  i32[4] parentIndex     i32[5] elementTypeIndex
  i32[6] genericContainerIndex   i32[7] reserved(-1)  i32[8] flags
  i32[9] fieldStart       i32[10] methodStart       i32[11] eventStart
  i32[12] propertyStart   i32[13] nestedTypesStart  i32[14] interfacesStart
  i32[15] vtableStart     i32[16] interfaceOffsetsStart
  u16[0] method_count  u16[1] property_count  u16[2] field_count  u16[3] event_count
  u16[4] nested_type_count  u16[5] vtable_count  u16[6] interfaces_count
  u16[7] interface_offsets_count        u32 bitfield, u32 token
```

* `Il2CppFieldDefinition` = 12 bytes `{nameIndex, typeIndex, token}`.
* `Il2CppMethodDefinition` stride = 64.
* `Il2CppImageDefinition` = 40 bytes.

Validated end-to-end: `AssemblyManager` → fields `['_instance','m_assemblies']`,
methods `['get_Instance','AddAssembly','GetTypes','.ctor','Quit']`;
`GameLoader` → namespace `HybridCLR`, fields include `gameEntryTypeName`,
`assemblyPath`, `s_initedOpt`.

Metadata lives on disk under
`/data/media/0/Android/data/com.mobile.legends/files/dragon2017/assets/UnityData_NEW/Managed/Metadata/`
with **real content**. The copies under `files/il2cpp/Metadata/` are **zeroed**
(0 bytes) after load — anti-dump.

> Pull these with a **device-side copy then `adb pull`**. `adb shell "cat file"`
> passing through a PTY expands every `\n` to `\r\n` and corrupts binaries
> (the 20 MB csharp blob inflates by 39 382 bytes).

## 3. Battle entity model

Namespaces in the csharp module: `Battle` (1 227 types), `MTTDProto` (1 643),
`muf.statescriptx` (675, Behaviac AI), `Hades` (164), `MCNet` (131).

### `Battle.ShowEntity` — the battlefield unit  *(type idx 3929, 393 fields, 804 methods)*

Client-side representation of every unit on the map. Field indices below are
positions in the field table; **byte offsets are resolved at runtime** with
`il2cpp_class_get_field_from_name` + `il2cpp_field_get_offset`.

| field idx | name | use |
|---|---|---|
| 18 | `m_iType` | unit kind |
| 11 / 28 / 32 | `IsSoldier` / `IsSpecialSoldier` / `IsCacheHero` | unit class flags |
| 54 | `m_EntityCampType` | **team** |
| 148 | `m_bSameCampType` | is-local-team flag |
| 83 / 84 | `m_uGuid` / `m_ID` | unique id / config id |
| 85 | `m_Level` | level |
| 86 / 90 / 91 | `m_HpLast` / **`m_Hp`** / **`m_HpMax`** | health |
| 92–104 | `m_MechArmorHp*`, `m_AdditionHp1/2*`, `m_ExtraHp*` | shields / extra bars |
| 141 | `m_vCachePosition` | **position (cached)** |
| 142 | `m_vUnityCachePosition` | position (Unity space) |
| 167 | `m_EntityObjPos` | entity object position |
| 165 / 166 | `bornPosVec3` / `bornDirVec3` | spawn point |
| 214 | `<LastPosition>k__BackingField` | last position |
| 175 | `m_fPlayerRadius` | **collision radius → box ESP sizing** |
| 203 | `m_HeadPosOffset` | head anchor for nametags |
| 120 / 130 | `m_dMoveSpeed` / `m_dRunSpeed` | movement |
| 153 / 155 | `m_SkinId` / `m_ShowSkinId` | skin |
| 188 | `m_bIsFakeHero` | clone/illusion flag |
| 81 / 181 | `m_iGrassId` / `m_GrassIds` | **bush occupancy** |
| 144 | `m_bUnityBloodVisible` | **health-bar visibility** |
| 146 | `m_bUnityMinimapVisible` | **minimap visibility → maphack** |
| 41 | `m_bUpdateOwnerMiniMapIconVisible` | minimap icon visibility |
| 183 / 184 | `m_DeadSightState` / `m_bDeadSightStateEye` | dead-sight vision |
| 246 / 295 | `visibleOutsideGrassInRange` / `ignoreVisibleOutsideGrassInRange` | **grass reveal → maphack** |
| 136 | `_iSkillInvisibility` | invisibility from skills |
| 266 | `m_bRendererVisible` | renderer state |
| 77 / 78 | `m_bSkillHide` / `m_bSkillHideInvalid` | skill-driven hide |
| 192 | `m_bLockBlood` | frozen HP bar |
| 261 | `m_ShaderReplacementBridge` | shader swap bridge (→ "shaders" module) |

### `Battle.BattleManager` — entity registry  *(type idx 4032, 45 fields, 121 methods)*

| field idx | name | use |
|---|---|---|
| 0 | `Instance` | **static singleton** |
| 15 | `m_LocalPlayerShow` | local player's `ShowEntity` |
| 16 | `m_LocalShowPlayers` | local-team players |
| 18 | `m_dicPlayerShow` | **dictionary of player entities** |
| 19 | `m_dicMonsterShow` | monsters |
| 20 | `m_ShowPlayers` | **player `ShowEntity` collection (ESP source)** |
| 21 | `m_ShowMonsters` | monster collection |
| 17 | `m_ShowPlayerSummons` | summons |
| 22 / 23 | `m_MainTowerDead` / `_enemyMainTower` | objective state |

### `Battle.LogicPlayer` / `Battle.LogicFighter` — server-side logic
`LogicPlayer` (347 fields, 513 methods), `LogicFighter` (206 fields, 715 methods).
`LogicFighter` carries `m_v2RealPos`, `m_dRealPosX/Y`, `fBornPosX/Y`,
`m_bDisregardTerrainMove`, movement/AI state. Use `ShowEntity` for ESP.

### `ShowBattleCamera` — camera  *(type idx 4061, 31 fields, 51 methods)*

| field idx | name |
|---|---|
| 0 | `Instance` (static singleton) |
| 2 / 3 / 5 | `m_CamPos` / `m_defaultCamPos` / `m_InitCamPos` |
| 4 / 8 | `m_CamRotVect` / `m_CamRot` |
| 14 | `m_iFieldOfView` |
| 16 | `m_screenPtCastDis` |
| 18 | `_uCameraPlayerId` |
| 26 | `m_bCameraMoving` |

The view-projection matrix is not exposed here — get it from the Unity
`UnityEngine.Camera` (`worldToCameraMatrix` × `projectionMatrix`) and project
manually, which avoids per-frame Unity calls.

### Related enums
`eEntityType` (3441), `EntityCampType` (3456), `EntityType` (19374),
`WEntityType` (18539), `ConcurrentCampType` (4089), `CheckCampType` (19424).

## 4. Tooling notes for this emulator

* Device ABI is **x86_64**; MLBB runs **arm64-v8a** under **Houdini**
  (`/system/lib64/arm64/nb/`, `libhoudini.so`).
* Guest ARM memory is directly **readable** from an x86_64 Frida agent
  (`Memory.readByteArray(ptr("0x07094000"))` returns the ELF header), but
  **calling** ARM functions from an x64 agent is not possible.
* The bundled `frida-server` / `frida-server16` are x86_64. A downloaded
  **arm64 frida-server runs under Houdini** (`--version` works) but **fails to
  daemonise** — no process, nothing listening. Not a viable agent.
* Consequence: dynamic introspection must be done by code running *inside* the
  game as arm64 (i.e. Rain itself), or by offline metadata parsing (above).

## 5. Reference implementation — VYNIX (see VYNIX-ANALYSIS.md)

Rain mirrors VYNIX's architecture: single arm64 `.so`, `JNI_OnLoad` entry,
stripped, libc++ static, `BIND_NOW`; ImGui + OpenGL ES overlay on the app's
`ANativeWindow`; **GlossHook v1.9.2 (XMDS)** for inline hooking; IL2CPP
reflection for game access; static libcurl for remote licence; `/proc/<pid>/maps`
self-inspection; MLBB hero-name table for nametags; 377 embedded PNG art assets
(hero portraits, country flags, spell/objective icons) driving an icon menu.
