#!/usr/bin/env bash
# PlayShell's changes to melonDS's copy (tools/core_lib.sh runs this on the copy; the source stays as it is):
#  - its JIT's memory: melonDS reserves 16 GB of address space (for "fast memory") and a shared memory file - the PS5
#    gives neither, melonDS did not check: its base became -1 + 8 GB and the DS crashed in load_game at 0x1ffffffff.
#    Fast memory is off in PlayShell, so a plain block of the size it needs does (no shm, no fault handler).
set -e
SRC="$1"
python3 - "$SRC/src/ARMJIT_Memory.cpp" <<'EOF'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
a = s.index('    // this used to be allocated with three different mmaps')
b = s.index('    u8* basePtr = MemoryBase;\n#endif', a)
s = s[:a] + '''    // PlayShell (PS5): no 16 GB reservation, no shared memory file - fast memory is off, a plain block does
    MemoryBase = (u8*)mmap(NULL, MemoryTotalSize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (MemoryBase == (u8*)MAP_FAILED)
        MemoryBase = (u8*)calloc(1, MemoryTotalSize);
    FastMem9Start = FastMem7Start = nullptr;
    MemoryFile = -1;

''' + s[b:]
old = '''    munmap(MemoryBase, MemoryTotalSize);
    close(MemoryFile);
#endif
}'''
assert s.count(old) == 1, 'DeInit'
s = s.replace(old, '''    munmap(MemoryBase, MemoryTotalSize); // (PlayShell: the plain block; no memory file)
#endif
}''')
# DeInit restores a SIGSEGV handler melonDS never set now: keep PlayShell's crash recorder
s = s.replace('''#else
    sigaction(SIGSEGV, &OldSaSegv, nullptr);''', '''#else
    // (PlayShell: no handler of its own was set)''', 1)
open(p, 'w', encoding='utf-8').write(s)
print('melonDS: plain JIT memory (PlayShell)')
EOF
