// Command-line harness (run through `adb shell`): loads an ARM32 library into
// the guest, runs its constructors and optionally calls an exported function.
//
//   zbtest <lib.so> [-v] [call <symbol> [u32 args...]]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../cpu.h"
#include "../runtime.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <lib.so> [-v] [call <symbol> [args...]]\n", argv[0]);
        return 2;
    }
    zb::g_log_to_stderr = true;
    int i = 2;
    if (i < argc && strcmp(argv[i], "-v") == 0) {
        zb::g_debug = true;
        i++;
    }

    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    std::vector<zb::u8> data;
    zb::u8 buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    fclose(f);

    zb::runtime::init();
    const char* slash = strrchr(argv[1], '/');
    zb::elf::Module* m = zb::runtime::load_library(slash ? slash + 1 : argv[1], data.data(), data.size());
    zb::elf::run_initializers(m);
    printf("constructors done\n");

    if (i + 1 < argc && strcmp(argv[i], "call") == 0) {
        zb::gaddr fn = zb::elf::lookup(argv[i + 1]);
        if (!fn) {
            fprintf(stderr, "symbol %s not found\n", argv[i + 1]);
            return 1;
        }
        zb::cpu::Args args;
        for (int k = i + 2; k < argc; ++k) args.u32v(static_cast<zb::u32>(strtoul(argv[k], nullptr, 0)));
        zb::u64 r = zb::cpu::call(fn, args);
        printf("%s -> 0x%08x (r1 0x%08x)\n", argv[i + 1], static_cast<zb::u32>(r), static_cast<zb::u32>(r >> 32));
    }
    return 0;
}
