# VYNIX LIB TRIAL — Reverse Engineering Notes

## Binary facts
| Property | 32-bit | 64-bit |
|---|---|---|
| File | `VYNIX LIB 32 BIT/librudp.bytes` | `VYNIX LIB 64 BIT/librudp.bytes` |
| Size | 10,185,444 | 10,833,088 |
| Class | ELF32 | ELF64 |
| Machine | EM_ARM (40) | EM_AARCH64 (183) |
| Type | DYN (PIE shared object) | DYN |
| SONAME | `libMyLibName.so` | `libMyLibName.so` |
| Sections | 26 | 26 |
| Dynsym | — | 301 entries, **only `JNI_OnLoad` exported FUNC** |
| Stripped | Yes (no .symtab) | Yes |

- `JNI_OnLoad` @ `0x809c54`, size 4292 bytes — sole entry point.
- libc++ statically linked (`_ZTVNSt6__ndk1*` present), hence ~10 MB size.
- `SYMBOLIC | BIND_NOW`, `FLAGS_1 = NOW` (full RELRO).
- `.rodata` = 6.9 MB (fonts + base64 UI art + string tables).

## Dependencies (DT_NEEDED)
`liblog, libandroid, libEGL, libGLESv3, libz, libdl, libc, libm`
→ Confirms an **OpenGL ES overlay renderer** using `ANativeWindow_*` + EGL.

## Rendering stack
ImGui OpenGL ES backend. Imported GL entry points include `glCreateShader`,
`glShaderSource`, `glCreateProgram`, `glGenVertexArrays`, `glBindVertexArray`,
`glBufferSubData`, `glUniformMatrix4fv`, `glScissor`, `glBlendFuncSeparate`,
`glActiveTexture`, `glDrawElements`, `glTexImage2D`, plus
`ANativeWindow_getWidth/getHeight`. ImGui internals present as plaintext:
`##MainMenuBar`, `##Tooltip_%02d`, `Debug##Default`,
`(Auto-disabled ImGuiDebugLogFlags_EventClipper to avoid spamming)`.

## Hooking framework
```
GlossHook v1.9.2, Created by XMDS.
Type:   %d
Open ELF (%s) file failed, errno: %d-%s
Hook failed, dlopen or dlsym symbol not found in android N linker and above.
__loader_dlopen
```
→ **GlossHook v1.9.2** by XMDS. Also present:
`Custimized trampoline range: %s, start: 0x%lx, end: 0x%lx`,
`Here are no trampoline instructions in free memory, we identify it as the first hook.`
→ inline/PLT hooking with trampoline allocation, `__loader_dlopen` used to
resolve symbols through the Android N+ linker namespace.

## Networking / licensing
- **libcurl statically linked** (FTP/IMAP/POP3/SMTP/RTSP/TLS/OCSP strings).
- OCSP + `Load certs from files in a directory`, `Load Account`,
  `Save Current Account` — CA bundle handling.
- Embedded **GeoIP table** (large list of IPv4 CIDR-ish literals) and full
  country-name list → region gating.
- Cloudflare-range IP `104.28.113.58` present. No plaintext HTTPS endpoint →
  license URL is assembled/obfuscated at runtime.

## Game integration
Target packages:
```
com.mobile.legends            (global)
com.mobile.legends.lite
com.mobile.legends.usa
com.mobile.v2l
com.mobilechess.us            (Magic Chess)
com.hhgame.mlbbvn             (VN)
com.vng.mlbbvn                (VN)
```
Config / working paths:
```
/data/data/com.mobile.legends/files/MLBB_SKILL
/data/user/0/com.mobile.legends/files/MLBB_SKILL
/sdcard/Android/data/com.mobile.legends/files/MLBB_SKILL
/sdcard/Download/MLBB_SKILL
/sdcard/MLBB_SKILL
/storage/emulated/0/Download/MLBB_SKILL
/storage/emulated/0/MLBB_SKILL
/files/dragon2017/OfflineReport
```
- `libil2cpp.so` referenced by name.
- **IL2CPP reflection in use**: `il2cpp_field_static_set_value`.
- JNI reflection into the app:
  `android/app/ActivityThread`, `android/content/Context`,
  `android/content/SharedPreferences`, `android/os/Build`,
  `android/os/Looper`, `android/provider/Settings$Secure`,
  plus `getBoolean`/`putBoolean` (SharedPreferences access).
- Self-inspection of `/proc/` and `/proc/%d/maps` (and `/proc/self/maps`).

## Feature evidence from strings
ESP target labels present as plaintext: `ENEMY`, `HEAD`, `NAME`, `TEAM`.
Also `Spell`, `skill2`, `(Active)`, `acc_`, `loaded`, `base`, `camera`,
`Color`, `Range`, `fast`, `warning`.
Full **MLBB hero-name table** embedded (Aamon, Akai, Aldous, Alice, Alpha,
Alucard, Angela, Argus, Arlott, Aulus, Aurora, Badang, Balmond, Bane, Barats,
Baxia, Beatrix, Belerick, Benedetta, Brody, Bruno, Carmilla, Cecilion, …) →
hero-ID → name resolution for nametags.

## Embedded art (377 unique PNGs, extracted to `analysis/vynix_assets/`)
- 2× `1000x707` MLBB-style announcement banners (e.g. "TURTLE IS UNDER ATTACK")
- ~375 square icons: **MLBB hero portraits**, **country flags** (circular),
  summoner-spell icons, item icons, role/objective icons, 100x100 and 240–323px.
→ Icon-driven menu + flag/spell/objective features, drawn in MLBB's art style.
Font strings reference Ascender Corp + SIL OFL (`scripts.sil.org/OFL`).

## Architectural summary (what Rain must mirror)
1. Single AArch64 (and ARM32) shared object, `JNI_OnLoad` entry, stripped,
   libc++ static, BIND_NOW.
2. Hooks the render path, creates an EGL/GLES context on the app's
   `ANativeWindow`, draws an **ImGui** overlay menu.
3. Reaches game state through **IL2CPP reflection**, not raw offsets.
4. Static libcurl for remote license/auth; GeoIP for region gating.
5. Reads device identifiers via JNI (`Build`, `Settings$Secure`,
   `SharedPreferences`) for HWID-style binding.
6. Inspects `/proc/<pid>/maps`.
