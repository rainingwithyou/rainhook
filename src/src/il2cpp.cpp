#include "il2cpp.h"

#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cinttypes>
#include <ctime>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOG_TAG "Rain"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace il2cpp {

Api api{};

JavaVM* g_vm = nullptr;

void set_vm(JavaVM* vm) { g_vm = vm; }
JavaVM* get_vm() { return g_vm; }

// ---------------------------------------------------------------------------
// ELF / mapping helpers
// ---------------------------------------------------------------------------

namespace {

struct Mapping {
    uintptr_t start;
    uintptr_t end;
    char path[256];
};

// Parse /proc/self/maps for the first mapping of `lib_name`, which is the
// load base (its file offset is 0). MLBB's own loader maps liblogic.so outside
// the linker's namespace, so this is the only reliable way to find it.
uintptr_t FindLibBase(const char* lib_name, bool check_slashes) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;

    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        uintptr_t s = 0, e = 0;
        unsigned long long off = 0;
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %*4s %llx", &s, &e, &off) != 3) continue;
        if (off != 0) continue;

        const char* p = strstr(line, lib_name);
        if (!p) continue;
        // require the match to be the tail of a path component so that
        // "liblogic.so" does not match inside "liblogic.so.bak"
        if (check_slashes) {
            size_t n = strlen(lib_name);
            if (p[n] != '\0' && p[n] != '\n') continue;
        }
        base = s;
        break;
    }
    fclose(f);
    return base;
}

// Resolve an exported symbol from a mapped ELF.
//
// dlsym is not usable: MLBB maps liblogic.so with its own loader, so the library
// is not registered with the linker.
//
// Section headers are also not usable. They sit past the last PT_LOAD's file
// extent (e_shoff is within 8 KB of EOF), so those pages are never mapped and
// reading them yields garbage. Everything must come from the *program* headers,
// which are mapped as part of the first segment.

struct DynTable {
    uintptr_t base = 0;
    const Elf64_Sym* symtab = nullptr;
    const char* strtab = nullptr;
    size_t strsz = 0;
    size_t symcount = 0;
};

// DT_* address entries are absolute in a bionic-loaded library, but MLBB's own
// loader may leave them as link-time virtual addresses. Accept either.
inline uintptr_t Fixup(uintptr_t v, uintptr_t base) {
    return (v >= base) ? v : v + base;
}

size_t SymCountFromGnuHash(uintptr_t base, uintptr_t gh) {
    const auto* h = reinterpret_cast<const uint32_t*>(gh);
    const uint32_t nbuckets = h[0];
    const uint32_t symoffset = h[1];
    const uint32_t bloom_size = h[2];
    const uintptr_t buckets = gh + 16 + bloom_size * 8;
    const uint32_t* b = reinterpret_cast<const uint32_t*>(buckets);
    uint32_t maxidx = 0;
    for (uint32_t i = 0; i < nbuckets; i++) {
        if (b[i] > maxidx) maxidx = b[i];
    }
    if (maxidx < symoffset) return symoffset;
    // Walk the chain of the last bucket to find the end of the symbol table.
    const uintptr_t chains = buckets + nbuckets * 4;
    const uint32_t* c = reinterpret_cast<const uint32_t*>(chains);
    uint32_t idx = maxidx;
    while (true) {
        const uint32_t v = c[idx - symoffset];
        idx++;
        if (v & 1u) break;
        if (idx > (1u << 24)) break;  // safety
    }
    (void)base;
    return idx;
}

bool ReadDynamic(uintptr_t base, DynTable* out) {
    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(base);
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return false;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) return false;
    if (!eh->e_phoff || !eh->e_phnum) return false;

    const auto* ph = reinterpret_cast<const Elf64_Phdr*>(base + eh->e_phoff);
    uintptr_t dyn = 0;
    size_t dynsz = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = base + ph[i].p_vaddr;
            dynsz = ph[i].p_memsz;
        }
    }
    if (!dyn || dynsz < sizeof(Elf64_Dyn)) return false;

    uintptr_t symtab = 0, strtab = 0, strsz = 0, hash = 0, gnuhash = 0;
    const auto* d = reinterpret_cast<const Elf64_Dyn*>(dyn);
    for (size_t i = 0; i < dynsz / sizeof(Elf64_Dyn); i++) {
        if (d[i].d_tag == DT_NULL) break;
        switch (d[i].d_tag) {
            case DT_SYMTAB:   symtab = d[i].d_un.d_ptr; break;
            case DT_STRTAB:   strtab = d[i].d_un.d_ptr; break;
            case DT_STRSZ:    strsz = d[i].d_un.d_val; break;
            case DT_HASH:     hash = d[i].d_un.d_ptr; break;
            case DT_GNU_HASH: gnuhash = d[i].d_un.d_ptr; break;
            default: break;
        }
    }
    if (!symtab || !strtab) return false;

    out->base = base;
    out->symtab = reinterpret_cast<const Elf64_Sym*>(Fixup(symtab, base));
    out->strtab = reinterpret_cast<const char*>(Fixup(strtab, base));
    out->strsz = strsz ? strsz : (1u << 20);

    if (hash) {
        const auto* h = reinterpret_cast<const uint32_t*>(Fixup(hash, base));
        out->symcount = h[1];  // nchain == number of symbols
    } else if (gnuhash) {
        out->symcount = SymCountFromGnuHash(base, Fixup(gnuhash, base));
    } else {
        out->symcount = 0;
    }
    if (out->symcount == 0 || out->symcount > (1u << 22)) {
        out->symcount = out->strsz / sizeof(Elf64_Sym);
        if (out->symcount == 0) out->symcount = 4096;
    }
    return true;
}

bool g_dyn_ready = false;
DynTable g_dyn;

void* SymbolFromBase(uintptr_t base, const char* name) {
    if (!base) return nullptr;
    if (!g_dyn_ready) {
        g_dyn_ready = ReadDynamic(base, &g_dyn);
        LOGI("il2cpp: dynamic table %s (symtab=%p strtab=%p n=%zu)",
             g_dyn_ready ? "ok" : "FAILED", (void*)g_dyn.symtab,
             (void*)g_dyn.strtab, g_dyn.symcount);
        if (!g_dyn_ready) return nullptr;
    }
    for (size_t i = 0; i < g_dyn.symcount; i++) {
        const Elf64_Sym& s = g_dyn.symtab[i];
        if (!s.st_name || !s.st_value) continue;
        if (ELF64_ST_TYPE(s.st_info) != STT_FUNC) continue;
        if (s.st_name >= g_dyn.strsz) continue;
        if (strcmp(g_dyn.strtab + s.st_name, name) != 0) continue;
        return reinterpret_cast<void*>(base + s.st_value);
    }
    return nullptr;
}

}  // namespace

void* ResolveFromLib(const char* lib_name, const char* sym) {
    uintptr_t base = FindLibBase(lib_name, true);
    if (!base) return nullptr;
    return SymbolFromBase(base, sym);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool Init(char* err, size_t errlen) {
    auto need = [&](const char* sym) -> void* {
        void* p = ResolveFromLib("liblogic.so", sym);
        if (!p) LOGI("il2cpp: missing %s", sym);
        return p;
    };

#define BIND(field, name) api.field = reinterpret_cast<decltype(api.field)>(need(name))

    BIND(domain_get, "il2cpp_domain_get");
    BIND(domain_get_assemblies, "il2cpp_domain_get_assemblies");
    BIND(domain_assembly_open, "il2cpp_domain_assembly_open");
    BIND(assembly_get_image, "il2cpp_assembly_get_image");
    BIND(image_get_name, "il2cpp_image_get_name");
    BIND(image_get_class_count, "il2cpp_image_get_class_count");
    BIND(image_get_class, "il2cpp_image_get_class");
    BIND(class_from_name, "il2cpp_class_from_name");
    BIND(class_get_name, "il2cpp_class_get_name");
    BIND(class_get_namespace, "il2cpp_class_get_namespace");
    BIND(class_get_field_from_name, "il2cpp_class_get_field_from_name");
    BIND(class_get_fields, "il2cpp_class_get_fields");
    BIND(class_get_method_from_name, "il2cpp_class_get_method_from_name");
    BIND(class_get_methods, "il2cpp_class_get_methods");
    BIND(class_get_parent, "il2cpp_class_get_parent");
    BIND(class_get_type, "il2cpp_class_get_type");
    BIND(field_get_name, "il2cpp_field_get_name");
    BIND(field_get_offset, "il2cpp_field_get_offset");
    BIND(field_get_type, "il2cpp_field_get_type");
    BIND(field_get_value, "il2cpp_field_get_value");
    BIND(field_set_value, "il2cpp_field_set_value");
    BIND(field_static_get_value, "il2cpp_field_static_get_value");
    BIND(field_static_set_value, "il2cpp_field_static_set_value");
    BIND(method_get_name, "il2cpp_method_get_name");
    BIND(object_new, "il2cpp_object_new");
    BIND(object_get_class, "il2cpp_object_get_class");
    BIND(string_new, "il2cpp_string_new");
    BIND(runtime_invoke, "il2cpp_runtime_invoke");
    BIND(thread_attach, "il2cpp_thread_attach");
    BIND(type_get_object, "il2cpp_type_get_object");
#undef BIND

    if (!api.domain_get || !api.class_from_name || !api.image_get_class ||
        !api.class_get_field_from_name || !api.field_get_offset) {
        snprintf(err, errlen, "core il2cpp API unavailable (liblogic.so not found?)");
        return false;
    }

    Il2CppDomain* d = api.domain_get();
    if (!d) {
        snprintf(err, errlen, "il2cpp_domain_get returned null");
        return false;
    }
    if (api.thread_attach) api.thread_attach(d);

    LOGI("il2cpp: API resolved, domain=%p", (void*)d);
    return true;
}

// ---------------------------------------------------------------------------
// Class / field lookup
// ---------------------------------------------------------------------------

namespace {
struct ClassCacheEntry {
    char key[160];
    Il2CppClass* klass;
};
ClassCacheEntry g_class_cache[256];
size_t g_class_cache_n = 0;

Il2CppImage* g_images[192];
size_t g_image_count = 0;
uint64_t g_images_last_scan_ms = 0;

uint64_t NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// MLBB uses HybridCLR: the game assembly (and therefore classes like ShowEntity)
// is loaded after startup, so the image set has to be refreshed rather than
// captured once.
void RefreshImages(bool force) {
    const uint64_t now = NowMs();
    if (!force && g_image_count && now - g_images_last_scan_ms < 1000) return;
    g_images_last_scan_ms = now;

    size_t n = 0;
    const Il2CppAssembly** asms = api.domain_get_assemblies(api.domain_get(), &n);
    size_t count = 0;
    for (size_t i = 0; i < n && count < 192; i++) {
        Il2CppImage* img = api.assembly_get_image(asms[i]);
        if (img) g_images[count++] = img;
    }
    if (count != g_image_count) {
        LOGI("il2cpp: images now %zu (was %zu)", count, g_image_count);
        for (size_t i = 0; i < count; i++) {
            const char* nm = api.image_get_name ? api.image_get_name(g_images[i]) : "?";
            LOGI("  img[%zu] %s", i, nm ? nm : "(null)");
        }
    }
    g_image_count = count;
}
}  // namespace

Il2CppClass* FindClass(const char* namespc, const char* name) {
    char key[160];
    snprintf(key, sizeof(key), "%s::%s", namespc ? namespc : "", name);
    for (size_t i = 0; i < g_class_cache_n; i++)
        if (strcmp(g_class_cache[i].key, key) == 0) return g_class_cache[i].klass;

    Il2CppClass* found = nullptr;
    RefreshImages(false);

    for (size_t i = 0; i < g_image_count && !found; i++) {
        if (!api.image_get_class_count || !api.image_get_class) break;
        size_t c = api.image_get_class_count(g_images[i]);
        for (size_t k = 0; k < c; k++) {
            Il2CppClass* kl = api.image_get_class(g_images[i], k);
            if (!kl) continue;
            const char* kn = api.class_get_name(kl);
            if (!kn || strcmp(kn, name) != 0) continue;
            if (namespc && *namespc) {
                const char* kns = api.class_get_namespace(kl);
                if (!kns || strcmp(kns, namespc) != 0) continue;
            }
            found = kl;
            break;
        }
    }

    // Only positive results are cached: a negative one is almost always "the
    // assembly has not loaded yet", and caching it would latch forever.
    if (found && g_class_cache_n < 256) {
        snprintf(g_class_cache[g_class_cache_n].key, sizeof(key), "%s::%s",
                 namespc ? namespc : "", name);
        g_class_cache[g_class_cache_n].klass = found;
        g_class_cache_n++;
        LOGI("il2cpp: bound class %s", key);
    }
    return found;
}

Il2CppClass* FindClassByMethod(const char* method, int argc) {
    RefreshImages(true);
    if (!api.class_get_method_from_name) return nullptr;
    for (size_t i = 0; i < g_image_count; i++) {
        if (!api.image_get_class_count || !api.image_get_class) break;
        size_t c = api.image_get_class_count(g_images[i]);
        for (size_t k = 0; k < c; k++) {
            Il2CppClass* kl = api.image_get_class(g_images[i], k);
            if (!kl) continue;
            if (api.class_get_method_from_name(kl, method, argc)) return kl;
        }
    }
    return nullptr;
}

Field GetField(Il2CppClass* klass, const char* name) {
    Field f;
    if (!klass) return f;
    f.info = api.class_get_field_from_name(klass, name);
    // Walk up the hierarchy: ShowEntity's attributes live on its parents.
    Il2CppClass* k = klass;
    while (!f.info && k) {
        k = api.class_get_parent ? api.class_get_parent(k) : nullptr;
        if (k) f.info = api.class_get_field_from_name(k, name);
    }
    if (f.info && api.field_get_offset) f.offset = api.field_get_offset(f.info);
    return f;
}

uintptr_t ReadStaticPtr(FieldInfo* f) {
    if (!f || !api.field_static_get_value) return 0;
    uintptr_t v = 0;
    api.field_static_get_value(f, &v);
    return v;
}

bool ReadIl2CppString(Il2CppString* s, char* out, size_t outlen) {
    // s is an already-peeked copy of the string object — no PlausiblePtr gate,
    // because the copy lives on the local stack. The caller peaks at least
    // kObjHeader + 4 + max_chars*2 bytes.
    if (!s || !out || outlen == 0) return false;
    int32_t len = Read<int32_t>(s, kObjHeader);
    if (len <= 0 || len > 512) return false;
    const uint16_t* chars = reinterpret_cast<const uint16_t*>(
        reinterpret_cast<uintptr_t>(s) + kObjHeader + 4);
    size_t n = static_cast<size_t>(len) < outlen - 1 ? static_cast<size_t>(len) : outlen - 1;
    for (size_t i = 0; i < n; i++) out[i] = chars[i] < 0x80 ? static_cast<char>(chars[i]) : '?';
    out[n] = '\0';
    return true;
}

}  // namespace il2cpp
