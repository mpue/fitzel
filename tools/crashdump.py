"""Read a Windows minidump: the exception, and a stack with names on it.

    python tools/crashdump.py [dump] [exe] [--all]

With no arguments it takes the newest sandbox dump in %LOCALAPPDATA%\\CrashDumps
and build/release/bin/sandbox.exe, which is the case that comes up.

Why this exists: the editor is /SUBSYSTEM:WINDOWS in Release, so a crash prints
nothing anywhere -- all it leaves is a minidump. There is no cdb on this machine
and no reason to install one for the two things that actually settle a crash:

  1. the exception record -- what went wrong, at which address, and (for an
     access violation) whether it was a read or a write and of what. A near-null
     address is a null dereference and the offset names the field; a wild one is
     a dangling pointer or a smashed stack. That distinction is most of the
     answer, and it is four fields in the file.
  2. a stack. Minidumps carry the crashing thread's stack MEMORY but no unwind,
     so rather than implement one this scans it for qwords that land inside the
     executable. Every return address on the stack is in that list, along with
     leftovers from earlier calls -- noisy, but in order, and the top few frames
     are almost always the story.

Names come from dbghelp.dll, the symbol engine Windows already has -- the one
the debuggers themselves use. (llvm-symbolizer ships with MSVC and looks like
the easier answer, but the build here does not read these PDBs: every address
comes back "??".) So the Release build has to carry a PDB -- see the /Zi block
in CMakeLists.txt. Without one the addresses still print, which is how this was
used before those symbols existed: 0xD38 over sizeof(std::vector<int>) is index
141, and that was enough to find a use-after-free.
"""
import ctypes
import ctypes.wintypes as wt
import glob
import os
import struct
import sys

STREAM_THREAD_LIST = 3
STREAM_MODULE_LIST = 4
STREAM_EXCEPTION = 6

MODULE_ENTRY = 108   # sizeof(MINIDUMP_MODULE)
THREAD_ENTRY = 48    # sizeof(MINIDUMP_THREAD)
CTX_RIP, CTX_RSP, CTX_RBP = 0xF8, 0x98, 0xA0   # CONTEXT_AMD64 offsets

EXC_NAMES = {
    0xC0000005: "ACCESS_VIOLATION",
    0xC000001D: "ILLEGAL_INSTRUCTION",
    0xC0000094: "INT_DIVIDE_BY_ZERO",
    0xC00000FD: "STACK_OVERFLOW",
    0xC0000374: "HEAP_CORRUPTION",
    0xC0000409: "STACK_BUFFER_OVERRUN / __fastfail",
    0x80000003: "BREAKPOINT",
}


SYMOPT_UNDNAME        = 0x00000002
SYMOPT_DEFERRED_LOADS = 0x00000004
SYMOPT_LOAD_LINES     = 0x00000010
MAX_SYM_NAME          = 2000


class SymbolInfo(ctypes.Structure):
    """SYMBOL_INFO, with its trailing name buffer inlined at full length."""
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG),
                ("Reserved", ctypes.c_ulonglong * 2), ("Index", wt.ULONG),
                ("Size", wt.ULONG), ("ModBase", ctypes.c_ulonglong),
                ("Flags", wt.ULONG), ("Value", ctypes.c_ulonglong),
                ("Address", ctypes.c_ulonglong), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG),
                ("MaxNameLen", wt.ULONG), ("Name", ctypes.c_char * MAX_SYM_NAME)]


class ImagehlpLine64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p),
                ("LineNumber", wt.DWORD), ("FileName", ctypes.c_char_p),
                ("Address", ctypes.c_ulonglong)]


class Symbols:
    """dbghelp, holding one module at the base the dump says it was loaded at.

    Loading it there means the addresses scavenged off the stack go straight in:
    no rebasing step, and so no arithmetic to get wrong.
    """

    SYMBOL_INFO_HEADER = 88   # sizeof(SYMBOL_INFO) without the name buffer

    def __init__(self, exe, base):
        self.ok = False
        try:
            self.dbg = ctypes.WinDLL("dbghelp.dll")
        except OSError:
            return
        self.proc = ctypes.c_void_p(-1)     # any unique handle will do
        self.dbg.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES |
                               SYMOPT_DEFERRED_LOADS)
        if not self.dbg.SymInitialize(self.proc, None, False):
            return
        self.dbg.SymLoadModuleEx.restype = ctypes.c_ulonglong
        self.ok = bool(self.dbg.SymLoadModuleEx(
            self.proc, None, exe.encode(), None,
            ctypes.c_ulonglong(base), 0, None, 0))

    def _info(self):
        si = SymbolInfo()
        si.SizeOfStruct = self.SYMBOL_INFO_HEADER
        si.MaxNameLen = MAX_SYM_NAME
        return si

    def at(self, addr):
        """(function, file:line) for an absolute address; (None, None) if unknown."""
        if not self.ok:
            return None, None
        si = self._info()
        disp = ctypes.c_ulonglong(0)
        if not self.dbg.SymFromAddr(self.proc, ctypes.c_ulonglong(addr),
                                    ctypes.byref(disp), ctypes.byref(si)):
            return None, None
        line = ImagehlpLine64()
        line.SizeOfStruct = ctypes.sizeof(ImagehlpLine64)
        ldisp = wt.DWORD(0)
        loc = None
        if self.dbg.SymGetLineFromAddr64(self.proc, ctypes.c_ulonglong(addr),
                                         ctypes.byref(ldisp), ctypes.byref(line)):
            f = line.FileName.decode("utf-8", "replace") if line.FileName else "?"
            loc = f"{os.path.basename(f)}:{line.LineNumber}"
        return si.Name.decode("utf-8", "replace"), loc

    def address_of(self, name):
        """Where a named function sits -- the cheap way to prove a PDB loaded."""
        if not self.ok:
            return None
        si = self._info()
        if not self.dbg.SymFromName(self.proc, name.encode(), ctypes.byref(si)):
            return None
        return si.Address


def mdstring(d, rva):
    (n,) = struct.unpack_from("<I", d, rva)
    return d[rva + 4:rva + 4 + n].decode("utf-16-le", "replace")


def newest_dump():
    root = os.path.join(os.environ.get("LOCALAPPDATA", ""), "CrashDumps")
    hits = glob.glob(os.path.join(root, "sandbox.exe.*.dmp"))
    return max(hits, key=os.path.getmtime) if hits else None


def report(path, exe, show_all=False):
    if not os.path.isfile(path):
        sys.exit(f"no such dump: {path}")
    if not os.path.isfile(exe):
        sys.exit(f"no such executable: {exe}"
                 " -- give one as the second argument")
    d = open(path, "rb").read()
    sig, _ver, nstreams, dirrva = struct.unpack_from("<IIII", d, 0)
    if sig != 0x504D444D:
        sys.exit(f"{path}: not a minidump")

    streams = {}
    for i in range(nstreams):
        st, size, rva = struct.unpack_from("<III", d, dirrva + i * 12)
        streams[st] = (size, rva)

    mods = []
    if STREAM_MODULE_LIST in streams:
        _, rva = streams[STREAM_MODULE_LIST]
        (n,) = struct.unpack_from("<I", d, rva)
        for i in range(n):
            base, size, _sum, _ts, namerva = struct.unpack_from(
                "<QIIII", d, rva + 4 + i * MODULE_ENTRY)
            mods.append((base, size, mdstring(d, namerva)))

    def owner(addr):
        for base, size, name in mods:
            if base <= addr < base + size:
                return os.path.basename(name), addr - base
        return None, 0

    print(f"=== {os.path.basename(path)} ===")
    tid = None
    if STREAM_EXCEPTION in streams:
        _, rva = streams[STREAM_EXCEPTION]
        (tid,) = struct.unpack_from("<I", d, rva)
        code, _flags, _rec, addr = struct.unpack_from("<IIQQ", d, rva + 8)
        nparam = struct.unpack_from("<I", d, rva + 32)[0]
        params = struct.unpack_from("<15Q", d, rva + 40)
        name, off = owner(addr)
        where = f"{name}+0x{off:X}" if name else "(unknown module)"
        print(f"thread   {tid}")
        print(f"code     0x{code:08X}  {EXC_NAMES.get(code, '?')}")
        print(f"address  0x{addr:016X}  {where}")
        if code == 0xC0000005 and nparam >= 2:
            kind = {0: "read", 1: "write", 8: "execute"}.get(params[0], params[0])
            bad = params[1]
            note = "  (near null -- a null dereference; the offset is the field)" \
                if bad < 0x10000 else ""
            print(f"tried to {kind} 0x{bad:016X}{note}")
        _cs, ctxrva = struct.unpack_from("<II", d, rva + 160)
        rip = struct.unpack_from("<Q", d, ctxrva + CTX_RIP)[0]
        rsp = struct.unpack_from("<Q", d, ctxrva + CTX_RSP)[0]
        rbp = struct.unpack_from("<Q", d, ctxrva + CTX_RBP)[0]
        print(f"rip 0x{rip:016X}  rsp 0x{rsp:016X}  rbp 0x{rbp:016X}")

    target = os.path.basename(exe).lower()
    base = size = None
    for b, s, name in mods:
        if os.path.basename(name).lower() == target:
            base, size = b, s
    if base is None:
        print(f"\n{target} is not in this dump -- nothing to symbolize.")
        return
    print(f"\n{target} loaded at 0x{base:016X}")

    stack = None
    if STREAM_THREAD_LIST in streams:
        _, rva = streams[STREAM_THREAD_LIST]
        (n,) = struct.unpack_from("<I", d, rva)
        for i in range(n):
            off = rva + 4 + i * THREAD_ENTRY
            (t,) = struct.unpack_from("<I", d, off)
            start, dsize, drva = struct.unpack_from("<QII", d, off + 24)
            if t == tid:
                stack = (start, d[drva:drva + dsize])
    if not stack:
        print("no stack memory for the crashing thread")
        return

    start, buf = stack
    seen, frames = set(), []
    for i in range(0, len(buf) - 8, 8):
        (v,) = struct.unpack_from("<Q", buf, i)
        if base <= v < base + size and (v - base) not in seen:
            seen.add(v - base)
            frames.append((start + i, v - base))

    print(f"stack 0x{start:012X} .. 0x{start + len(buf):012X}"
          f"  --  {len(frames)} candidate frames\n")

    sym = Symbols(os.path.abspath(exe), base)
    named = skipped = 0
    for sp, r in frames:
        fn, loc = sym.at(base + r)
        if not fn:
            continue
        # A scavenged stack is mostly not frames: vtable pointers, string
        # literals and other data that happen to live in the module. Those
        # resolve to a name but to no LINE, because they are not code -- so by
        # default only the ones with a source position are shown, which is the
        # project's own frames and the crash is nearly always in those. Pass
        # --all when the answer is somewhere in the libraries.
        if loc is None and not show_all:
            skipped += 1
            continue
        print(f"  0x{sp:012X}  +0x{r:<8X} {fn}")
        if loc:
            print(f"{'':16}{loc}")
        named += 1
    if skipped:
        print(f"\n  ({skipped} more resolved to data or to code with"
              f" no line info -- pass --all to see them)")
    if named == 0:
        print("  nothing resolved -- raw offsets, in stack order:")
        for sp, r in frames[:40]:
            print(f"  0x{sp:012X}  +0x{r:X}")
        print("\nRead it against the build that produced it: a dump"
              " from an older sandbox.exe resolves to nothing, because"
              " everything moved when it was relinked. And Release needs"
              " the /Zi block in CMakeLists.txt to leave a PDB at all.")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--all"]
    show_all = "--all" in sys.argv
    dump = args[0] if len(args) > 0 else newest_dump()
    exe = args[1] if len(args) > 1 else "build/release/bin/sandbox.exe"
    if not dump:
        sys.exit("no dump given and none found in %LOCALAPPDATA%\\CrashDumps")
    report(dump, exe, show_all)
