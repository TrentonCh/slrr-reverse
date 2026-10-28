#!/usr/bin/env python3
"""Query the Build 940 Ghidra project (headless, via pyghidra).

Usage:
  python tools/ghidra940.py refs 0x564684            # who references a VA
  python tools/ghidra940.py decompile 0x4201F0 [...]  # decompile function(s) containing VAs
  python tools/ghidra940.py func 0x4201F0             # function summary (name, range, callers)
  python tools/ghidra940.py strings VMThread          # data strings containing text, with refs

Env: SLRR_GHIDRA (install dir), SLRR_GHIDRA_PROJECT (project dir), SLRR_GHIDRA_NAME.
Run with plain `python` (pyghidra is in the user site-packages).
"""
import os
import sys

GHIDRA = os.environ.get("SLRR_GHIDRA", r"E:\Tools\ghidra_12.1.4_PUBLIC_20260921")
PROJECT = os.environ.get("SLRR_GHIDRA_PROJECT", r"C:\Users\trent\source\repos\TrentonCh\slrr-ghidra")
PNAME = os.environ.get("SLRR_GHIDRA_NAME", "slrr940")
BINARY = os.environ.get("SLRR_GHIDRA_EXE", r"D:\Program Files\Steam\steamapps\common\Street Legal Racing Redline\StreetLegal_Redline.exe")
os.environ.setdefault("JAVA_HOME", r"C:\Program Files\Eclipse Adoptium\jdk-25.0.4.101-hotspot")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd, args = sys.argv[1], sys.argv[2:]
    import pyghidra
    pyghidra.start(install_dir=GHIDRA)
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import ConsoleTaskMonitor

    do_analyze = (cmd == "analyze")
    with pyghidra.open_program(BINARY, project_location=PROJECT,
                               project_name=PNAME, analyze=do_analyze) as flat:
        if do_analyze:
            prog0 = flat.getCurrentProgram()
            print("analysis done: functions=%d instructions=%d" % (
                prog0.getFunctionManager().getFunctionCount(), prog0.getListing().getNumInstructions()))
            return 0
        prog = flat.getCurrentProgram()
        af = prog.getAddressFactory()
        fm = prog.getFunctionManager()
        rm = prog.getReferenceManager()
        listing = prog.getListing()

        def addr(s):
            return af.getAddress(s if isinstance(s, str) else hex(s))

        def fn_at(a):
            f = fm.getFunctionAt(a)
            return f if f else fm.getFunctionContaining(a)

        if cmd == "refs":
            for s in args:
                a = addr(s)
                print("== refs to", a)
                for r in rm.getReferencesTo(a):
                    f = fn_at(r.getFromAddress())
                    print("   from %s  %s  in %s" % (r.getFromAddress(), r.getReferenceType(),
                          f.getName() + "@" + str(f.getEntryPoint()) if f else "?"))
        elif cmd == "func":
            for s in args:
                f = fn_at(addr(s))
                if not f:
                    print("no function at", s)
                    continue
                print("== %s entry=%s body=%s size=%d" % (f.getName(), f.getEntryPoint(), f.getBody(), f.getBody().getNumAddresses()))
                callers = set()
                for r in rm.getReferencesTo(f.getEntryPoint()):
                    c = fn_at(r.getFromAddress())
                    if c:
                        callers.add("%s@%s" % (c.getName(), c.getEntryPoint()))
                print("   callers:", sorted(callers))
                callees = set()
                for c in f.getCalledFunctions(ConsoleTaskMonitor()):
                    callees.add("%s@%s" % (c.getName(), c.getEntryPoint()))
                print("   callees:", sorted(callees))
        elif cmd == "decompile":
            di = DecompInterface()
            di.openProgram(prog)
            for s in args:
                f = fn_at(addr(s))
                if not f:
                    print("no function at", s)
                    continue
                res = di.decompileFunction(f, 120, ConsoleTaskMonitor())
                print("// ==== %s @ %s (requested %s)" % (f.getName(), f.getEntryPoint(), s))
                if res and res.decompileCompleted():
                    print(res.getDecompiledFunction().getC())
                else:
                    print("// decompile failed:", res.getErrorMessage() if res else "?")
        elif cmd == "scan":
            # find code/data bytes equal to the LE u32 of each VA; report containing functions
            import struct as _st
            mem = prog.getMemory()
            for s in args:
                v = int(s, 16)
                pat = bytes(_st.pack("<I", v))
                print("== scan for", hex(v))
                for blk in mem.getBlocks():
                    if not blk.isInitialized():
                        continue
                    import jpype
                    n = int(blk.getSize())
                    jbuf = jpype.JArray(jpype.JByte)(n)
                    blk.getBytes(blk.getStart(), jbuf)
                    buf = bytes(bytearray((b & 0xFF) for b in jbuf))
                    i = buf.find(pat)
                    while i != -1:
                        a = blk.getStart().add(i)
                        f = fn_at(a)
                        print("   %s in %s  fn=%s" % (a, blk.getName(), ("%s@%s" % (f.getName(), f.getEntryPoint())) if f else "-"))
                        i = buf.find(pat, i + 1)
        elif cmd == "info":
            print("functions:", fm.getFunctionCount(), " instructions:", listing.getNumInstructions(),
                  " defined data:", listing.getNumDefinedData())
            for s in args:
                a = addr(s)
                ins = listing.getInstructionContaining(a)
                dat = listing.getDataContaining(a)
                blk = prog.getMemory().getBlock(a)
                print("  %s block=%s insn=%s data=%s fn=%s" % (a, blk.getName() if blk else "-",
                      ins.getMinAddress().toString() + " " + ins.toString() if ins else "-",
                      dat.getMinAddress().toString() + " " + str(dat.getDataType()) if dat else "-",
                      fn_at(a).getName() if fn_at(a) else "-"))
        elif cmd == "asmrefs":
            # for each code reference to the target, print the instructions leading to it
            n_before = 8
            for s in args:
                a = addr(s)
                print("== code refs to", a)
                for r in rm.getReferencesTo(a):
                    fa = r.getFromAddress()
                    f = fn_at(fa)
                    print("  -- from %s in %s" % (fa, f.getName() if f else "?"))
                    ins = listing.getInstructionContaining(fa)
                    chain = []
                    cur = ins
                    for _ in range(n_before):
                        if cur is None:
                            break
                        chain.append(cur)
                        cur = cur.getPrevious()
                    for i in reversed(chain):
                        print("     %s  %s" % (i.getMinAddress(), i))
        elif cmd == "strings":
            needle = args[0]
            for d in listing.getDefinedData(True):
                v = d.getValue()
                if v is None or not isinstance(v, str):
                    continue
                if needle in v:
                    refs = []
                    for r in rm.getReferencesTo(d.getAddress()):
                        f = fn_at(r.getFromAddress())
                        refs.append("%s@%s" % (f.getName(), f.getEntryPoint()) if f else str(r.getFromAddress()))
                    print("%s  %r  refs=%s" % (d.getAddress(), v[:80], sorted(set(refs))[:6]))
        elif cmd == "range":
            # range LO HI: decompile every function whose entry lies in [LO, HI)
            di = DecompInterface()
            di.openProgram(prog)
            lo, hi = int(args[0], 16), int(args[1], 16)
            for f in fm.getFunctions(addr(lo), True):
                if f.getEntryPoint().getOffset() >= hi:
                    break
                res = di.decompileFunction(f, 120, ConsoleTaskMonitor())
                print("// ==== %s @ %s" % (f.getName(), f.getEntryPoint()))
                if res and res.decompileCompleted():
                    print(res.getDecompiledFunction().getC())
        elif cmd == "asm":
            # asm VA: the instructions of the function containing VA
            for s in args:
                f = fn_at(addr(s))
                if not f:
                    print("no function at", s)
                    continue
                print("// ==== %s @ %s" % (f.getName(), f.getEntryPoint()))
                for ins in listing.getInstructions(f.getBody(), True):
                    print("  %s  %s" % (ins.getMinAddress(), ins))
        elif cmd == "sym":
            # symbols whose name contains the text, with their address
            st = prog.getSymbolTable()
            for s in st.getAllSymbols(True):
                if args[0] in s.getName(True):
                    print("%s  %s" % (s.getAddress(), s.getName(True)))
        elif cmd == "vtables":
            # vtables TEXT [count]: the first entries of every vftable whose class name contains TEXT
            mem = prog.getMemory()
            n = int(args[1]) if len(args) > 1 else 12
            st = prog.getSymbolTable()
            for s in st.getAllSymbols(True):
                nm = s.getName(True)
                if args[0] in nm and nm.endswith("::vftable"):
                    a = s.getAddress()
                    row = []
                    for i in range(n):
                        v = mem.getInt(a.add(4 * i)) & 0xFFFFFFFF
                        row.append("%08X" % v)
                    print("%s %s  %s" % (a, nm, " ".join(row)))
        elif cmd == "dwords":
            # dwords VA [count]: u32 values at VA, with the function each one names
            mem = prog.getMemory()
            a = addr(args[0])
            for i in range(int(args[1]) if len(args) > 1 else 16):
                v = mem.getInt(a.add(4 * i)) & 0xFFFFFFFF
                f = fm.getFunctionAt(addr(v)) if v >= 0x400000 else None
                print("  +0x%02X  %08X  %s" % (4 * i, v, f.getName() if f else ""))
        else:
            print("unknown command", cmd)
            return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
