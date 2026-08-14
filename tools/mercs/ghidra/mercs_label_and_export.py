# Attribute functions to their original source file, then export decompiled C.
# @category Mercs
# @runtime Jython
#
# SLUS_209.32 is stripped, so a plain decompile gives thousands of FUN_00xxxxxx
# with nothing to say which system any of them belongs to. But the binary is
# full of assert and log strings that name a source file and a line -
# "RsHud.cpp [15485]" - and an assert lives inside the function it guards.
# So the function that REFERENCES such a string is part of that file.
#
# That turns 104 file names into a map over the whole text section: functions
# get grouped into a namespace per source file, and the decompiled output is
# written one .c per original module instead of one undifferentiated dump.
#
# It is attribution, not proof. A function that never asserts gets nothing, and
# an inlined helper is attributed to whoever inlined it. Those land in
# _unattributed.c rather than being guessed at.
import os
import re

from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.program.model.symbol import SourceType
from ghidra.util.task import ConsoleTaskMonitor

SRC_RE = re.compile(r'([A-Za-z0-9_./\\-]+\.(?:cpp|c|h|hpp|inl))')

out_dir = os.environ.get('MERCS_GHIDRA_OUT', r'E:\Game Development\xcom-c\mercs_extracted\decompiled')
if not os.path.isdir(out_dir):
    os.makedirs(out_dir)

listing = currentProgram.getListing()
refmgr = currentProgram.getReferenceManager()
fm = currentProgram.getFunctionManager()
st = currentProgram.getSymbolTable()
monitor = ConsoleTaskMonitor()

# ---- 1. attribute functions to source files via the strings they reference ---
attributed = {}                 # function entry address -> file name
data_iter = listing.getDefinedData(True)
n_strings = 0
while data_iter.hasNext():
    d = data_iter.next()
    val = d.getValue()
    if val is None:
        continue
    s = str(val)
    m = SRC_RE.search(s)
    if not m:
        continue
    n_strings += 1
    src = m.group(1).replace('\\', '/').split('/')[-1]
    for ref in refmgr.getReferencesTo(d.getAddress()):
        fn = fm.getFunctionContaining(ref.getFromAddress())
        if fn is None:
            continue
        attributed[fn.getEntryPoint()] = src

print('[mercs] %d source-naming strings, %d functions attributed'
      % (n_strings, len(attributed)))

# Group them in the symbol tree so the module structure is browsable in the GUI.
namespaces = {}
for fn in fm.getFunctions(True):
    src = attributed.get(fn.getEntryPoint())
    if not src:
        continue
    safe = src.replace('.', '_')
    ns = namespaces.get(safe)
    if ns is None:
        ns = st.getNamespace(safe, None)
        if ns is None:
            ns = st.createNameSpace(None, safe, SourceType.ANALYSIS)
        namespaces[safe] = ns
    try:
        fn.setParentNamespace(ns)
    except Exception:
        pass

# ---- 2. decompile, writing one file per attributed module -------------------
ifc = DecompInterface()
opts = DecompileOptions()
ifc.setOptions(opts)
ifc.openProgram(currentProgram)

buckets = {}
done = failed = 0
for fn in fm.getFunctions(True):
    if monitor.isCancelled():
        break
    res = ifc.decompileFunction(fn, 60, monitor)
    if res is None or not res.decompileCompleted():
        failed += 1
        continue
    src = attributed.get(fn.getEntryPoint(), '_unattributed')
    buckets.setdefault(src, []).append(
        '/* %s  @ %s */\n%s\n' % (fn.getName(), fn.getEntryPoint(),
                                  res.getDecompiledFunction().getC()))
    done += 1
    if done % 500 == 0:
        print('[mercs] decompiled %d functions' % done)

for src, chunks in buckets.items():
    stem = src.replace('.', '_')
    path = os.path.join(out_dir, stem + '.c')
    f = open(path, 'w')
    f.write('/* Decompiled from SLUS_209.32 (Mercenaries, PS2).\n'
            '   Attributed to %s by the assert strings its functions reference.\n'
            '   %d function(s). Reconstructed pseudo-C, not original source. */\n\n'
            % (src, len(chunks)))
    for c in chunks:
        f.write(c)
        f.write('\n')
    f.close()

print('[mercs] wrote %d module file(s) to %s' % (len(buckets), out_dir))
print('[mercs] %d functions decompiled, %d failed' % (done, failed))
