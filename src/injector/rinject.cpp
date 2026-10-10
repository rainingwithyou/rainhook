/*
 * rinject — hot-attach injector for the Rain module.
 *
 * Run as root on the device (adb shell su -c /data/local/tmp/rinject):
 * finds the game process, attaches via ptrace, and calls dlopen on rain.so
 * inside it — the XNISH-style second load model. Unlike the comlibs staging
 * path (which needs a game restart), this attaches to a RUNNING game, so the
 * session and config survive.
 *
 * Mechanism (classic remote dlopen):
 *   1. find the target pid by /proc scan
 *   2. PTRACE_ATTACH, waitpid, save registers
 *   3. locate libc.so in the target and parse its dynsym for dlopen
 *   4. push the path string + an RET stub address onto the target's stack
 *   5. set x0 = path, x1 = RTLD_NOW, pc = dlopen, lr = the stub; continue
 *   6. on the stub's breakpoint, restore registers, detach
 */

#include <android/log.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOG_TAG "rinject"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define TARGET_NAME "com.mobile.legends"
#define PAYLOAD "/data/local/tmp/rain.so"
#define RETURN_MAGIC 0x5241494E /* "RAIN" — the stub we plant returns here */

static pid_t FindPid(const char* name) {
    DIR* d = opendir("/proc");
    if (!d) return -1;
    struct dirent* ent;
    pid_t found = -1;
    char path[128], line[512];
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/cmdline", ent->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        if (fgets(line, sizeof(line), f)) {
            if (strstr(line, name)) found = atoi(ent->d_name);
        }
        fclose(f);
        if (found > 0) break;
    }
    closedir(d);
    return found;
}

// The dlopen symbol's offset inside a libc file, from its dynsym.
static uintptr_t DlopenOffset(const char* lib_path) {
    FILE* f = fopen(lib_path, "rb");
    if (!f) return 0;
    Elf64_Ehdr eh;
    if (fread(&eh, sizeof(eh), 1, f) != 1 || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) {
        fclose(f);
        return 0;
    }
    uintptr_t out = 0;
    for (int i = 0; i < eh.e_shnum && !out; i++) {
        Elf64_Shdr sh;
        fseek(f, eh.e_shoff + i * eh.e_shentsize, SEEK_SET);
        if (fread(&sh, sizeof(sh), 1, f) != 1) break;
        if (sh.sh_type != SHT_DYNSYM) continue;
        // walk the symtab, resolving names through its strtab
        Elf64_Shdr strsh;
        fseek(f, eh.e_shoff + sh.sh_link * eh.e_shentsize, SEEK_SET);
        if (fread(&strsh, sizeof(strsh), 1, f) != 1) break;
        char* str = static_cast<char*>(malloc(strsh.sh_size));
        fseek(f, strsh.sh_offset, SEEK_SET);
        if (fread(str, 1, strsh.sh_size, f) != strsh.sh_size) {
            free(str);
            break;
        }
        const size_t n = sh.sh_size / sizeof(Elf64_Sym);
        for (size_t k = 0; k < n; k++) {
            Elf64_Sym sym;
            fseek(f, sh.sh_offset + k * sizeof(Elf64_Sym), SEEK_SET);
            if (fread(&sym, sizeof(sym), 1, f) != 1) break;
            if (!sym.st_name || !sym.st_value) continue;
            if (strcmp(str + sym.st_name, "dlopen") != 0) continue;
            out = sym.st_value;
            break;
        }
        free(str);
    }
    fclose(f);
    return out;
}

// libc.so's base address in the TARGET process, from its maps. Also returns
// the exact file path, so dlopen is resolved from the same libc build.
static uintptr_t LibcBase(pid_t pid, char* path_out, size_t path_len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        uintptr_t s = 0, e = 0;
        unsigned long long off = 0;
        if (sscanf(line, "%lx-%lx %*4s %llx", &s, &e, &off) != 3) continue;
        if (off != 0) continue;
        const char* p = strstr(line, "/libc.so");
        if (!p) continue;
        // the match must end the path token so /libc.so does not match
        // inside /libc.so.bak
        const char* tok_end = p + strlen("/libc.so");
        if (*tok_end != '\0' && *tok_end != '\n' && *tok_end != ' ') continue;
        base = s;
        snprintf(path_out, path_len, "%.*s", (int)(tok_end - line), line);
        break;
    }
    fclose(f);
    return base;
}

int main() {
    const pid_t pid = FindPid(TARGET_NAME);
    if (pid <= 0) {
        LOGE("no process for %s", TARGET_NAME);
        return 1;
    }
    printf("target pid %d\n", pid);

    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) != 0) {
        LOGE("PTRACE_ATTACH failed: %s", strerror(errno));
        return 1;
    }
    waitpid(pid, nullptr, 0);

    struct user_regs_struct saved;
    struct iovec iov;

    /* read the target's registers */
    struct user_regs_struct regs;
    iov.iov_base = &regs;
    iov.iov_len = sizeof(regs);
    if (ptrace(PTRACE_GETREGSET, pid, (void*)1 /* NT_PRSTATUS */, &iov) != 0) {
        LOGE("GETREGSET failed: %s", strerror(errno));
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return 1;
    }
    saved = regs;

    char libc_path[256] = {0};
    const uintptr_t libc_base = LibcBase(pid, libc_path, sizeof(libc_path));
    if (!libc_base) {
        LOGE("libc.so base not found in target maps");
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return 1;
    }
    // resolve dlopen from the same libc build the target runs
    const uintptr_t dlopen_off = DlopenOffset(libc_path);
    if (!dlopen_off) {
        LOGE("dlopen not found in %s", libc_path);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return 1;
    }
    const uintptr_t dlopen_addr = libc_base + dlopen_off;
    printf("libc @ 0x%lx (%s), dlopen @ 0x%lx\n", libc_base, libc_path, dlopen_addr);

    // write the payload path just below the stack pointer, and plant a return
    // stub (an instruction that hits a signal we can catch) at the same place
    regs.sp -= 0x200;
    const size_t path_len = strlen(PAYLOAD) + 1;
    for (size_t i = 0; i < path_len; i += sizeof(long)) {
        long word = 0;
        memcpy(&word, PAYLOAD + i, (path_len - i) < sizeof(long) ? (path_len - i) : sizeof(long));
        if (ptrace(PTRACE_POKEDATA, pid, (void*)(regs.sp + i), (void*)word) != 0) {
            LOGE("POKEDATA failed: %s", strerror(errno));
            ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
            return 1;
        }
    }
    // the BRK instruction (0xd4200000) at the string end: dlopen returns to
    // lr = stub, executes BRK, and the SIGTRAP it raises is the call's
    // completion — one continue instead of single-stepping through all of
    // dlopen (which would take minutes of ptrace round-trips)
    const uintptr_t stub = regs.sp + path_len;
    if (ptrace(PTRACE_POKEDATA, pid, (void*)stub, (void*)0xd4200000UL) != 0) {
        LOGE("stub poke failed");
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return 1;
    }

    regs.regs[0] = regs.sp;            // x0 = path
    regs.regs[1] = 2;                  // x1 = RTLD_NOW
    regs.pc = dlopen_addr;
    regs.regs[30] = stub;              // lr = the RET stub
    iov.iov_base = &regs;
    iov.iov_len = sizeof(regs);
    if (ptrace(PTRACE_SETREGSET, pid, (void*)1, &iov) != 0) {
        LOGE("SETREGSET failed: %s", strerror(errno));
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return 1;
    }

    // run until the BRK at the stub fires (SIGTRAP) — the dlopen call is done
    for (int i = 0; i < 100; i++) {
        int status = 0;
        if (ptrace(PTRACE_CONT, pid, nullptr, nullptr) != 0) {
            LOGE("PTRACE_CONT failed: %s", strerror(errno));
            break;
        }
        if (waitpid(pid, &status, 0) < 0) break;
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            LOGE("target died during remote call");
            break;
        }
        if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP) break;
        iov.iov_base = &regs;
        if (ptrace(PTRACE_GETREGSET, pid, (void*)1, &iov) != 0) break;
        if (regs.pc >= stub && regs.pc < stub + 8) break;
    }

    // restore and detach
    regs = saved;
    iov.iov_base = &regs;
    ptrace(PTRACE_SETREGSET, pid, (void*)1, &iov);
    ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
    printf("injected; rain.so dlopen dispatched\n");
    LOGI("rain.so dlopen dispatched into pid %d", pid);
    return 0;
}
