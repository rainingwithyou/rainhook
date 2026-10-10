#pragma once
// IL2CPP reflection layer.
//
// liblogic.so exports the full IL2CPP runtime API (275 functions) but MLBB loads
// it with its own loader rather than the dynamic linker, so dlsym(RTLD_DEFAULT)
// will not find it. We locate the mapping via /proc/self/maps and resolve symbols
// by walking the ELF dynamic symbol table ourselves.

#include <cstdint>
#include <cstddef>
#include <jni.h>
#include <initializer_list>

struct Il2CppClass;
struct Il2CppObject;
struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct FieldInfo;
struct MethodInfo;
struct Il2CppString;
struct Il2CppArray;
struct Il2CppType;

namespace il2cpp {

// ---- raw runtime entry points (resolved from liblogic.so) ----
struct Api {
    Il2CppDomain* (*domain_get)();
    const Il2CppAssembly** (*domain_get_assemblies)(const Il2CppDomain*, size_t*);
    Il2CppAssembly* (*domain_assembly_open)(const Il2CppDomain*, const char*);
    Il2CppImage* (*assembly_get_image)(const Il2CppAssembly*);

    const char* (*image_get_name)(const Il2CppImage*);
    size_t (*image_get_class_count)(const Il2CppImage*);
    Il2CppClass* (*image_get_class)(const Il2CppImage*, size_t);

    Il2CppClass* (*class_from_name)(const Il2CppImage*, const char*, const char*);
    const char* (*class_get_name)(const Il2CppClass*);
    const char* (*class_get_namespace)(const Il2CppClass*);
    FieldInfo* (*class_get_field_from_name)(Il2CppClass*, const char*);
    FieldInfo* (*class_get_fields)(Il2CppClass*, void**);
    const MethodInfo* (*class_get_method_from_name)(Il2CppClass*, const char*, int);
    const MethodInfo* (*class_get_methods)(Il2CppClass*, void**);
    Il2CppClass* (*class_get_parent)(Il2CppClass*);
    Il2CppType* (*class_get_type)(Il2CppClass*);

    const char* (*field_get_name)(FieldInfo*);
    size_t (*field_get_offset)(FieldInfo*);
    Il2CppType* (*field_get_type)(FieldInfo*);
    void (*field_get_value)(Il2CppObject*, FieldInfo*, void*);
    void (*field_set_value)(Il2CppObject*, FieldInfo*, void*);
    void (*field_static_get_value)(FieldInfo*, void*);
    void (*field_static_set_value)(FieldInfo*, void*);

    const char* (*method_get_name)(const MethodInfo*);
    Il2CppObject* (*object_new)(Il2CppClass*);
    Il2CppClass* (*object_get_class)(Il2CppObject*);
    Il2CppString* (*string_new)(const char*);
    Il2CppObject* (*runtime_invoke)(const MethodInfo*, void*, void**, Il2CppObject**);
    void* (*thread_attach)(Il2CppDomain*);
    // class -> System.Type, so a class can be handed to reflection-driven APIs
    // such as UnityEngine.Object.FindObjectOfType to fetch a live instance.
    Il2CppObject* (*type_get_object)(const Il2CppType*);
};

extern Api api;

// The JVM, captured by the entry point. Unity's current activity is reachable
// from it (UnityPlayer.currentActivity) — the bridge for touch synthesis and
// SharedPreferences persistence.
void set_vm(JavaVM* vm);
JavaVM* get_vm();

// Resolve liblogic.so (mapper-aware) and populate `api`. Returns false with a
// reason in `err` if anything essential is missing.
bool Init(char* err, size_t errlen);

// ---- helpers -------------------------------------------------------------

// Class lookup across every loaded image (MLBB splits types over three modules).
Il2CppClass* FindClass(const char* namespc, const char* name);

// Find the first class across all loaded images that declares a method with
// this name (argc -1 matches any overload). For targets whose class name has
// moved between builds but whose method name survived.
Il2CppClass* FindClassByMethod(const char* method, int argc);

// Cached field offset for a class; 0 means "not found" (offset 0 is the header).
struct Field {
    FieldInfo* info = nullptr;
    size_t offset = 0;
    bool ok() const { return info != nullptr; }
};
Field GetField(Il2CppClass* klass, const char* name);

// Static field value as a pointer-width scalar.
uintptr_t ReadStaticPtr(FieldInfo* f);

template <typename T>
inline T Read(void* base, size_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + off);
}

template <typename T>
inline void Write(void* base, size_t off, T v) {
    *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + off) = v;
}

inline bool PlausiblePtr(uintptr_t p) {
    // Reject nulls, small ints and kernel-ish range. The ceiling is the
    // userspace pointer limit for the ABI: 47 bits on 64-bit, the full
    // 32-bit space minus the kernel half on 32-bit.
#if defined(__LP64__)
    return p > 0x10000 && p < 0x0000800000000000ULL;
#else
    return p > 0x10000;
#endif
}

// IL2CPP object header: [klass][monitor]. Managed arrays add [bounds][length].
// Both entries are pointers, so the header is 16 bytes on 64-bit and 8 on
// 32-bit — every field offset below shifts with it.
#if defined(__LP64__)
static constexpr size_t kObjHeader = 0x10;
static constexpr size_t kArrayHeader = 0x20;  // + [bounds][length], 8-byte aligned
#else
static constexpr size_t kObjHeader = 0x08;
static constexpr size_t kArrayHeader = 0x10;  // + [bounds][length], 4-byte aligned
#endif
static constexpr size_t kListItems = kObjHeader;             // List<T>._items
static constexpr size_t kListSize = kObjHeader + sizeof(void*);  // List<T>._size

// The k-th pointer-sized slot after the object header. Managed fields that
// hold references are laid out as consecutive pointer slots, so this is how
// the dictionary probes address them on either ABI.
static constexpr size_t PtrSlot(int k) {
    return kObjHeader + static_cast<size_t>(k) * sizeof(void*);
}

inline Il2CppArray* AsArray(void* p) { return reinterpret_cast<Il2CppArray*>(p); }

inline int32_t ArrayLength(void* arr) {
    // length follows [klass][monitor][bounds]; bounds is a pointer.
    return Read<int32_t>(arr, kObjHeader + sizeof(void*));
}

inline void* ArrayData(void* arr) {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(arr) + kArrayHeader);
}

// Managed Dictionary<TKey, TValue>. IL2CPP lays Dictionary out as
// [header][buckets][entries][count][version][freeList][freeCount]...
// buckets/entries are pointers (8 bytes each on 64-bit, 4 on 32-bit); the
// trailing fields are ints. Entry is a struct
// {int hashCode; int next; TKey key; TValue value} — stride depends on
// TKey/TValue (both 4-byte ints here: 16 bytes, value at +8).
static constexpr size_t kDictBuckets = kObjHeader;
static constexpr size_t kDictEntries = kObjHeader + sizeof(void*);
static constexpr size_t kDictCount = kObjHeader + 2 * sizeof(void*);

// Iterate an int-keyed dictionary of reference values. Calls fn(key, value)
// for every live entry; free slots (hashCode < 0) are skipped. Returns the
// number of entries visited. The entry stride depends on TValue's width, so
// it is probed: the stride whose key/value fields validate across all
// entries wins. Callers snapshot under the same lifetime rules as arrays.
template <typename Fn>
inline int DictEach(void* dict, Fn fn) {
    if (!dict || !PlausiblePtr(reinterpret_cast<uintptr_t>(dict))) return 0;
    const int32_t count = Read<int32_t>(dict, kDictCount);
    if (count <= 0 || count > 4096) return 0;
    void* entries = Read<void*>(dict, kDictEntries);
    if (!PlausiblePtr(reinterpret_cast<uintptr_t>(entries))) return 0;

    for (int stride : {24, 16, 32}) {
        const size_t val_off = (stride == 16) ? 12 : 16;
        // validate the whole table first: keys are spell ids (small ints),
        // values are objects or null
        bool ok = true;
        for (int i = 0; i < count && ok; i++) {
            void* e = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(entries)
                                              + kArrayHeader + static_cast<size_t>(i) * stride);
            const int32_t key = Read<int32_t>(e, 8);
            void* val = Read<void*>(e, val_off);
            if (key <= 0 || key > 100000) { ok = false; break; }
            if (val && !PlausiblePtr(reinterpret_cast<uintptr_t>(val))) { ok = false; break; }
        }
        if (!ok || count <= 0) continue;

        int visited = 0;
        for (int i = 0; i < count; i++) {
            void* e = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(entries)
                                              + kArrayHeader + static_cast<size_t>(i) * stride);
            const int32_t hash = Read<int32_t>(e, 0);
            if (hash < 0) continue;  // free slot
            const int32_t key = Read<int32_t>(e, 8);
            void* val = Read<void*>(e, val_off);
            if (key <= 0) continue;
            fn(key, val);
            visited++;
        }
        if (visited > 0) return visited;
    }
    return 0;
}

// Borrowed string view; reads from ALREADY-PEEKED bytes. The caller peaks the
// string object (header + length + chars) into local memory first and passes
// that copy — reading a live string directly can fault on a freed object.
// Copy at least kObjHeader + 4 + expected_max_chars*2 bytes.
bool ReadIl2CppString(Il2CppString* s, char* out, size_t outlen);

}  // namespace il2cpp
