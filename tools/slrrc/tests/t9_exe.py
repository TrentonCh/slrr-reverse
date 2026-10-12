#!/usr/bin/env python3
"""t9_exe.py - milestone M9: the two rules that were read in the Build 940 exe,
each proven on what the corpus holds (notes/exe_reads.md).

    python -I tests/t9_exe.py [-v] [--update]

FLOAT LITERALS.  The scanner of the exe converts a float token with one
routine, in SSE single precision: setting B of slrrc/floatlit.py.

  exe files   the 136 class files of the corpus that the Build 940 exe itself
              wrote (tests/baseline/m5_exe940.tsv) are byte-identical under
              setting B, with no per-file choice.  Setting A, the default
              until the exe was read, reproduces 90 of them.
  model       floatlit under B equals the exact model (tests/floatmodel.py:
              fractions, every rounding written out) for every distinct float
              literal text of the corpus
  exe bytes   the exe file of the corpus is the one the notes were read in
              (its SHA-256), and the instructions and constants they quote
              stand at their addresses: the float action of the scanner, the
              sscanf wrapper, the exit for a first character that is no
              digit, the integer part, the digit loop MULSS / MULSS / ADDSS,
              the constant 0.1f, the two strings of a pack path.  The file is
              read as bytes and never run
  example     0.0001 is 0x38D1B716 in the shipped Baiern_oil_pan.class (an
              earlier build) and 0x38D1B718 in LP_V12_oil_pan_racing.class
              (written by this exe): the bits of A and of B
  exits       no literal of the corpus is of a form the two exits of the
              routine change (no digit before the point; more than 1,024
              fraction digits), so they move no shipped file

PACK NAMES.  The pool spelling of a pack is the spelling of its first
registration in the pack table of the session (slrrc/packs.py: PackTable).
The table is replayed as a session builds it: system.rpk, the pack scans of
the constructor of GameLogic, then the sources.

  scans       the scan list of the package is the list of rpkScan calls of
              sl/Scripts/game/src/GameLogic.java; System.rpkScan is a script
              method (it has a tree in System.class and the exe has no such
              string), System.openLib is native
  spellings   after the sources are compiled the table has a record with
              exactly the spelling of each of the 126 packs of the shipped
              pools (tests/pack_oracle.py)
  nodes       every resource id node of the consistent pairs has the pack
              name and the id the rule gives for a literal of its line (the
              literals read with lib/lexer.py, not with the package)
  missing     the pack without a file compiles to system.rpk id 0, which is
              the one rid entry with local id 0 in all shipped pools; no
              shipped local id exceeds 16 bits
  order       the answers for all literals of all sources are the same when
              the sources are compiled in reverse order, and when each source
              is compiled against a table of its own.  No order can give
              another answer: every pack that the start of the game and the
              scans do not register has one spelling in all literals and all
              external reference tables
  oracle      ... and they are the answers of the 126-entry table over the
              pack files (the rule the tests used before the exe was read)
  controls    without the scans the table reproduces fewer spellings, and
              how many depends on the order: the scans are part of the rule
  two forms   packs.resolve() over the two questions of the interface gives
              what the functions of the exe give when they are followed
              literally (global id, then the record of the id), for every
              literal and for made-up ids with a low word of 0 and with high
              words on every pack file
  reader      the reader of the external reference tables (slrrc/install.py)
              against a second one written here, on every pack file
  table       find-or-add as read: A to Z folded and nothing else, the first
              spelling stays, a name is cut to 63 characters, a pack without
              a file keeps its record

Headline:
  M9 exe rules     floats: 136/136 class files the exe wrote byte-identical
  under B (90 under A), B equals the exact model on 15954/15954 literal
  texts; packs: 126/126 spellings of the shipped pools, 9058/9058 rid nodes,
  9952/9952 literals the same in any order of compilation (119 and 123
  spellings without the scans); checks N/N
"""
import hashlib
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness              # noqa: E402
import lexer                # noqa: E402  lib
import oracle               # noqa: E402  lib
import tufa                 # noqa: E402  lib
import floatmodel as fm     # noqa: E402
import pack_oracle          # noqa: E402

import slrrc                        # noqa: E402
from slrrc import emit              # noqa: E402
from slrrc import floatlit          # noqa: E402
from slrrc import install           # noqa: E402
from slrrc import packs             # noqa: E402
from slrrc import scanner as sc     # noqa: E402

EXE = floatlit.EXE_MODE
GAMELOGIC = 'sl/Scripts/game/src/GameLogic.java'
SYSTEM_CLASS = 'system/scripts/lang/System.class'
EXE_FILE = 'StreetLegal_Redline.exe'    # read as bytes, never run
EXE_SHA256 = 'f44fb88a4335c657e20701cdb84afb8d1d85f7c3aceb93d30113e6e6e44fefc2'
NATIVE = 0x40               # the flag of a native method row (notes/declarations.md 4)

# (address, bytes, what they are): the instructions and constants
# notes/exe_reads.md quotes, as they stand in the file of the Build 940 exe
EXE_BYTES = (
    (0x552F2E, '53 e88c640100 83c404 b84c010000 d91d14257a00',
     'the float action of the scanner: PUSH yytext; CALL 0x5693C0; return token 0x14C; '
     'FSTP float [yylval]'),
    (0x5693C7, 'c745fc00000000 50 6820645b00',
     'the wrapper: result preset to 0.0; PUSH its address; PUSH "%f"'),
    (0x5B6420, '256600', '"%f"'),
    (0x52754B, '3c30 0f8cb9000000 3c39 0f8fb1000000',
     'no digit first: CMP AL,0x30; JL 0x52760C; CMP AL,0x39; JG 0x52760C (nothing is stored)'),
    (0x527564, '0fbec0 8d0c89 42 8d49e8 8d0c48',
     'the integer part: MOVSX EAX,AL; LEA ECX,[ECX+ECX*4]; INC EDX; LEA ECX,[ECX-0x18]; '
     'LEA ECX,[EAX+ECX*2]  (ip = ip*10 + c - 48 in 32 bits)'),
    (0x527577, '660f6ec1 f30fe6c0 c1e91f 803a2e f20f5804cdb03f5900 660f5ac8',
     'MOVD XMM0,ECX; CVTDQ2PD; SHR ECX,31; CMP [EDX],"."; ADDSD XMM0,[ECX*8+0x593FB0]; '
     'CVTPD2PS XMM1,XMM0  (v = (float)ip, ip unsigned)'),
    (0x593FB0, '0000000000000000 000000000000f041', 'the doubles 0.0 and 4294967296.0'),
    (0x527598, 'f30f101dd4355900 0f28d3', 'MOVSS XMM3,[0x5935D4]; MOVAPS XMM2,XMM3  (s = 0.1f)'),
    (0x5935D4, 'cdcccc3d', 'the float 0.1f'),
    (0x5275A7, '8b4d10', 'MOV ECX,[EBP+0x10]: the number of fraction digits to read'),
    (0x5275C0, '660f6ec0 8a02 0f5bc0 f30f59c2 f30f59d3 f30f58c8',
     'the digit loop: MOVD XMM0,EAX; MOV AL,[EDX]; CVTDQ2PS XMM0,XMM0; MULSS XMM0,XMM2; '
     'MULSS XMM2,XMM3; ADDSS XMM1,XMM0'),
    (0x527601, 'f30f1108', 'MOVSS [EAX],XMM1: the result'),
    (0x5B5028, '5c00', 'the separator of a pack path'),
    (0x5B502C, '2e72706b00', '".rpk"'),
)


def exe_bytes_differ(data):
    """The rows of EXE_BYTES the exe file does not hold: [(address, what)]."""
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    base = struct.unpack_from('<I', data, pe + 24 + 28)[0]
    n_sections = struct.unpack_from('<H', data, pe + 6)[0]
    first = pe + 24 + struct.unpack_from('<H', data, pe + 20)[0]
    bad = []
    for va, want, what in EXE_BYTES:
        want = bytes.fromhex(want.replace(' ', ''))
        have = b''
        for i in range(n_sections):
            vsize, rva, rsize, raw = struct.unpack_from('<IIII', data, first + i * 40 + 8)
            if rva <= va - base < rva + max(vsize, rsize):
                at = raw + va - base - rva
                have = data[at:at + len(want)]
        if have != want:
            bad.append('0x%X %s: %s' % (va, what, have.hex()))
    return bad


class Checks(object):
    def __init__(self):
        self.results = []           # (what, None or why it failed)

    def check(self, what, cond, detail=''):
        self.results.append((what, None if cond else (str(detail) or 'failed')))


# ---------------------------------------------------------------------------
# float literals
# ---------------------------------------------------------------------------

def exe_written():
    out = {}
    for line in harness.Baseline('m5_exe940.tsv').load() or []:
        cls, modes = line.split('\t')
        out[cls] = modes
    return out


def float_nodes(blobs):
    """The bits of every float node of a class file."""
    out = []
    for b in blobs:
        for nodes in b.trees:
            for nd in nodes:
                if nd.op == emit.OP_FLOAT:
                    out.append(nd.imm)
    return out


def float_part(g, opts):
    """(class files the exe wrote, identical under B, identical under A,
    literal texts, texts on which B is the exact model, the passing files)."""
    prov = harness.provider()
    pk = harness.packs()
    listed = exe_written()
    n = same_b = same_a = 0
    passed = []
    texts = set()
    by_cls = {}
    for e in harness.all_sources():
        text = harness.load_source(e)
        for t in sc.scan(text, slrrc.Diagnostics(False)):
            if t.kind == sc.T_FLOAT:
                texts.add(text[t.pos:t.pos + t.length])
        if e.cls in listed:
            by_cls[e.cls] = e
    bad = []
    for cls in sorted(listed):
        e = by_cls.get(cls)
        if e is None:
            bad.append('%s: not a pair of the corpus' % cls)
            continue
        n += 1
        text = harness.load_source(e)
        theirs = oracle.load_class_bytes(e)
        res = slrrc.compile_source(text, e.source, prov, pk,
                                   slrrc.Options(float_mode=EXE, strict=True))
        if res.ok and res.class_file == theirs:
            same_b += 1
            passed.append(cls)
        else:
            bad.append('%s: not byte-identical under setting %s' % (cls, EXE))
        res = slrrc.compile_source(text, e.source, prov, pk,
                                   slrrc.Options(float_mode='A', strict=True))
        same_a += res.ok and res.class_file == theirs
    g.check('the class files the Build 940 exe wrote are byte-identical under setting %s, '
            'every one' % EXE, n == same_b == len(listed) == 136 and not bad, '; '.join(bad[:3]))
    only_b = sum(1 for m in listed.values() if 'A' not in m and 'C' not in m and m != '=')
    g.check('... and setting A, the default before the exe was read, reproduces fewer of '
            'them', same_a == 90 and only_b == 46, '%d under A, %d that neither A nor C '
            'reproduces' % (same_a, only_b))
    texts = sorted(texts)
    exact = 0
    odd = []
    for tx in texts:
        ip, fp = sc.float_digits(tx)
        if floatlit.float_bits(EXE, ip, fp) == fm.convert(EXE, tx):
            exact += 1
        else:
            odd.append(tx)
    g.check('setting %s equals the exact model on every float literal text of the corpus'
            % EXE, exact == len(texts) and len(texts) > 15000, repr(odd[:3]))
    no_int = [tx for tx in texts if sc.float_digits(tx)[0] == '']
    longest = max([len(sc.float_digits(tx)[1]) for tx in texts] or [0])
    g.check('no float literal of the corpus is of a form the two exits of the routine '
            'change', not no_int and longest < floatlit.MAX_FRACTION_DIGITS,
            '%d without a digit before the point, longest fraction %d digits' % (
                len(no_int), longest))
    with open(os.path.join(oracle.corpus_root(), EXE_FILE), 'rb') as f:
        exe = f.read()
    bad = exe_bytes_differ(exe)
    g.check('the exe of the corpus is the Build 940 exe the notes were read in, and holds '
            'the %d instruction and constant sequences they quote' % len(EXE_BYTES),
            hashlib.sha256(exe).hexdigest() == EXE_SHA256 and not bad, '; '.join(bad[:3]))
    g.check('0.0001: the bits of A and of B', floatlit.float_bits('A', '0', '0001') ==
            0x38D1B716 and floatlit.float_bits('B', '0', '0001') == 0x38D1B718)
    found = {}
    for e in harness.oracle_pairs():
        if e.name in ('Baiern_oil_pan', 'LP_V12_oil_pan_racing'):
            found[e.name] = (e.cls in listed, float_nodes(oracle.load_class(e)))
    old = found.get('Baiern_oil_pan', (None, []))
    new = found.get('LP_V12_oil_pan_racing', (None, []))
    g.check('... in a class file as shipped (Baiern_oil_pan) and in one the exe wrote '
            '(LP_V12_oil_pan_racing)',
            old[0] is False and 0x38D1B716 in old[1] and 0x38D1B718 not in old[1] and
            new[0] is True and 0x38D1B718 in new[1] and 0x38D1B716 not in new[1],
            '%r / %r' % (old[0], new[0]))
    return n, same_b, same_a, len(texts), exact, passed


# ---------------------------------------------------------------------------
# pack names
# ---------------------------------------------------------------------------

def source_literals():
    """[(index entry, [(line, written pack name, id)])] for every source, the
    resource id literals read with lib/lexer.py."""
    out = []
    for e in harness.all_sources():
        lits = []
        for t in lexer.tokenize(harness.load_source(e)):
            if t.kind == 'rid':
                pack, lid = lexer.rid_parts(t.text)
                lits.append((t.line, pack, lid))
        out.append((e, lits))
    return out


def answers(table, sources):
    """{(source, position): (name, id, why)}: every literal resolved against
    `table`, the sources in the order given."""
    out = {}
    for e, lits in sources:
        k = 0
        for line, pack, lid in lits:
            out[(e.source, k)] = packs.resolve(table, packs.pack_path(pack), lid)
            k += 1
    return out


def spellings_reproduced(table, pool_spellings):
    return sum(1 for p in pool_spellings if table.name_of(p) == p)


def second_reader(path):
    """The external reference names of a pack file, read without the
    package: None when it is no pack."""
    with open(path, 'rb') as f:
        data = f.read(16)
        if len(data) < 16 or data[:4] != b'RPAK':
            return None
        version, n1, n2 = struct.unpack('<III', data[4:16])
        out = []
        for _ in range(n1 + n2):
            rec = f.read(0x40)
            if len(rec) < 0x40:
                break
            out.append(rec[4:].split(b'\0')[0].decode('latin-1'))
        return out


def scan_calls(text):
    """(the literal arguments of System.rpkScan in a source text, the base
    of the loop that scans every directory of a path)."""
    fixed = re.findall(r'System\.rpkScan\(\s*"((?:[^"\\]|\\.)*)"\s*\)', text)
    fixed = [s.replace('\\\\', '\\') for s in fixed]
    loop = re.search(r'String\s+path\s*=\s*"((?:[^"\\]|\\.)*)"\s*;\s*'
                     r'String\s+name\s*=\s*ff\.first\(\s*path\s*\+\s*"\*"\s*,\s*'
                     r'FindFile\.DIRS_ONLY\s*\)\s*;\s*while\s*\(\s*name\s*\)\s*\{\s*'
                     r'System\.rpkScan\(\s*path\s*\+\s*name\s*\+\s*"\\\\"\s*\)', text)
    base = loop.group(1).replace('\\\\', '\\') if loop else None
    return fixed, base


def pack_part(g, opts):
    prov = harness.provider()
    root = oracle.corpus_root()
    listing = harness.pack_files()
    pool = sorted(prov.pack_spellings.values())
    sources = source_literals()
    n_lit = sum(len(lits) for _, lits in sources)

    # ---- the scans are the ones of the script
    with open(os.path.join(root, GAMELOGIC.replace('/', os.sep)), 'rb') as f:
        fixed, base = scan_calls(f.read().decode('latin-1'))
    g.check('the scan list of the package is the list of rpkScan calls of GameLogic.java',
            tuple(fixed) == packs.SCANS_940 and (base,) == packs.SUB_SCANS_940,
            '%r / %r' % (fixed, base))
    rows = {}               # method of System -> (it is native, nodes of its tree)
    for blob in oracle.load_class(SYSTEM_CLASS):
        if blob.name == 'java.lang.System':
            for _, _, m in blob.methods():
                n_nodes = len(blob.trees[m.tree]) if m.tree != tufa.NONE else 0
                rows[blob.utf8(m.name)] = (bool(m.flags & NATIVE), n_nodes)
    with open(os.path.join(root, EXE_FILE), 'rb') as f:
        exe = f.read()
    scan = rows.get('rpkScan', (None, 0))
    g.check('System.rpkScan is a script method (not native, a tree with nodes in '
            'System.class, no such string in the exe); System.openLib is native',
            scan[0] is False and scan[1] > 20 and rows.get('openLib') == (True, 0) and
            b'rpkScan' not in exe and b'openLib' in exe,
            'rpkScan %r, openLib %r' % (rows.get('rpkScan'), rows.get('openLib')))

    # ---- the session, then every source in index order
    table = harness.packs()
    n_boot = len(table.records)
    ans = answers(table, sources)
    same = spellings_reproduced(table, pool)
    passed = ['pack ' + p for p in pool if table.name_of(p) == p]
    g.check('the oracle table is what the shipped pools hold (126 packs, one spelling each)',
            sorted(pack_oracle.TABLE_940) == pool and len(pool) == 126)
    g.check('after the sources are compiled the replayed table has a record with the '
            'spelling of every pack of the shipped pools', same == len(pool) == 126,
            repr([(p, table.name_of(p)) for p in pool if table.name_of(p) != p][:3]))
    by = {}
    for p in pool:
        who = packs.ORIGIN_NAMES[table.records[table.find(p)].origin]
        by[who] = by.get(who, 0) + 1
    g.check('who registered the 126 first: the start of the game 1, the scans 61, an '
            'external reference table 12, a literal 52',
            by == {'boot': 1, 'openLib': 61, 'external table': 12, 'literal': 52}, repr(by))

    # ---- every rid node of the consistent pairs
    nodes = hit = entries = zero = 0
    over = 0
    zero_in = set()
    wrong = []
    for e in oracle.iter_entries():
        if e.cls is None:
            continue
        for blob in oracle.load_class(e):
            for ent in blob.cons:
                if ent[0] == 'rid':
                    entries += 1
                    if ent[2] == 0:
                        zero += 1
                        zero_in.add((e.cls, blob.utf8(ent[1])))
                    over += ent[2] > 0xFFFF
    by_source = dict((e.source, lits) for e, lits in sources)
    for e in harness.oracle_pairs() + harness.older_pairs():
        lits = by_source[e.source]
        on_line = {}
        k = 0
        for line, pack, lid in lits:
            a = ans[(e.source, k)]
            on_line.setdefault(line, []).append((a[0], a[1]))
            k += 1
        for blob in oracle.load_class(e):
            for tree in blob.trees:
                for nd in tree:
                    if nd.op != emit.OP_RID:
                        continue
                    nodes += 1
                    have = tuple(blob.rid(nd.imm))
                    if have in on_line.get(nd.slot, ()):
                        hit += 1
                    else:
                        wrong.append('%s line %d: %r' % (e.cls, nd.slot, have))
    g.check('every resource id node of the consistent pairs has the pack name and the id '
            'of the rule', nodes == hit == 9058, '%d of %d; %s' % (hit, nodes, wrong[:2]))
    no_file = [a for a in ans.values() if a[2] == packs.R_NO_FILE]
    g.check('the pack without a file compiles to system.rpk id 0: the one rid entry with '
            'local id 0 in the shipped pools (TimeAttackTest), and no shipped local id '
            'exceeds 16 bits',
            entries == 7242 and zero == 1 and over == 0 and zero_in == set([(
                'sl/Scripts/game/CareerEvents/TimeAttackTest.class', 'system.rpk')]) and
            len(no_file) > 0 and set(no_file) == set([('system.rpk', 0, packs.R_NO_FILE)]),
            '%d entries, %d with id 0 %r, %d above 16 bits' % (entries, zero, sorted(zero_in),
                                                              over))
    plain = [a for a in ans.values() if a[2] not in (packs.R_OK, packs.R_NO_FILE)]
    g.check('every literal of the shipped sources has a low word and no high word',
            not plain and n_lit == len(ans) == 9952, '%d literals, %r' % (n_lit, plain[:2]))

    # ---- the order of compilation
    rev = answers(harness.packs(), list(reversed(sources)))
    alone = {}
    for item in sources:
        if item[1]:
            alone.update(answers(harness.packs(), [item]))
    differ = [k for k in ans if rev.get(k) != ans[k] or alone.get(k) != ans[k]]
    order_same = len(ans) - len(differ)
    g.check('the answers are the same in reverse order and for each source alone',
            not differ and len(rev) == len(alone) == len(ans), repr(differ[:3]))
    # why no order can give another answer: whatever could register a pack
    # behind the scans (a literal of any source, a name of any external
    # reference table) spells it in one way
    start = harness.packs()
    spelled = {}
    for e, lits in sources:
        for line, pack, lid in lits:
            p = packs.pack_path(pack)
            if start.find(p) == packs.NONE:
                spelled.setdefault(packs.lower(p), set()).add(p)
    for p in listing:
        for x in harness.pack_disk().externals(p) or []:
            if start.find(x) == packs.NONE:
                spelled.setdefault(packs.lower(x), set()).add(x)
    several = sorted(k for k, v in spelled.items() if len(v) > 1)
    longest = max(len(p) for v in spelled.values() for p in v)
    g.check('every pack the start of the game and the scans do not register has one '
            'spelling in all literals and all external reference tables (%d packs), and no '
            'name is longer than a record keeps: no order of compilation gives another '
            'answer' % len(spelled),
            not several and len(spelled) > 30 and longest <= packs.NAME_MAX, repr(several[:3]))
    stateless = pack_oracle.TablePackNames(pack_oracle.TABLE_940, listing)
    differ = []
    for e, lits in sources:
        k = 0
        for line, pack, lid in lits:
            name = stateless.canonical(packs.pack_path(pack))
            want = (name, lid) if name is not None else (packs.NO_FILE_PACK, packs.NO_FILE_ID)
            if ans[(e.source, k)][:2] != want:
                differ.append((e.source, pack))
            k += 1
    g.check('... and they are the answers of the 126-entry table over the pack files',
            not differ, repr(differ[:3]))

    # ---- controls: without the scans
    t1 = harness.packs(False)
    answers(t1, sources)
    t2 = harness.packs(False)
    answers(t2, list(reversed(sources)))
    c1 = spellings_reproduced(t1, pool)
    c2 = spellings_reproduced(t2, pool)
    g.check('control: without the scans fewer spellings are reproduced, and how many '
            'depends on the order of compilation', (c1, c2) == (119, 123), '%d / %d' % (c1, c2))

    # ---- the interface form against the functions of the exe
    a_tab = harness.packs()
    b_tab = harness.packs()
    differ = []
    n_forms = 0
    kinds = {}

    def both(path, number):
        a = packs.resolve(a_tab, path, number)
        gid = b_tab.global_id(path, number)
        entry = b_tab.pool_entry(gid)
        kinds[a[2]] = kinds.get(a[2], 0) + 1
        if entry is None:
            return a[2] == packs.R_FAULT
        return a[2] != packs.R_FAULT and (a[0], a[1]) == entry

    for e, lits in sources:
        for line, pack, lid in lits:
            n_forms += 1
            if not both(packs.pack_path(pack), lid):
                differ.append((pack, lid))
    for p in listing:
        ext = harness.pack_disk().externals(p) or []
        for hi in (0, 1, 2, len(ext), len(ext) + 1, 0x7FFF, 0xFFFF):
            for lo in (0, 1, 0xFFFF):
                n_forms += 1
                if not both(p.lower(), (hi << 16) | lo):
                    differ.append((p, hi, lo))
    for path, number in (('no\\such.rpk', 5), ('no\\such.rpk', 0x10005), ('NO\\SUCH.rpk', 0)):
        n_forms += 1
        if not both(path, number):
            differ.append((path, number))
    g.check('packs.resolve() over the interface gives what the functions of the exe give, '
            'followed literally (%d questions)' % n_forms,
            not differ and len(a_tab.records) == len(b_tab.records) and
            [r.name for r in a_tab.records] == [r.name for r in b_tab.records] and
            all(kinds.get(w, 0) > 0 for w in (packs.R_OK, packs.R_NO_FILE, packs.R_ZERO,
                                             packs.R_EXTERNAL, packs.R_NO_EXTERNAL)),
            '%r %r' % (differ[:3], kinds))

    # ---- the reader of the external reference tables
    disk = harness.pack_disk()
    bad = []
    names = {}
    n_ref = 0
    for p in listing:
        mine = disk.externals(p)
        theirs = second_reader(os.path.join(root, p.replace('\\', os.sep)))
        if mine != (theirs or []) or theirs is None:
            bad.append(p)
        for x in mine or []:
            n_ref += 1
            names.setdefault(packs.lower(x), set()).add(x)
    several = [k for k, v in names.items() if len(v) > 1]
    g.check('the external reference tables: the reader of the package and a second one '
            'agree on all %d pack files; %d references, %d names, one spelling each'
            % (len(listing), n_ref, len(names)),
            not bad and len(listing) == 147 and len(names) == 60 and not several and
            disk.externals('no\\such.rpk') is None and
            disk.externals('CARS/racers/duhen.RPK') == disk.externals('cars\\racers\\Duhen.rpk'),
            repr(bad[:3] + several[:3]))

    # ---- find-or-add as read
    files = packs.ListedPackFiles(['a\\B.rpk', 'x/y.rpk', 'system.rpk'],
                                  {'a\\B.rpk': ['Ext\\One.rpk', 'system.rpk']})
    t = packs.PackTable(files)
    first = t.find_or_add('A\\b.RPK', packs.O_OPEN)
    again = t.find_or_add('a\\B.rpk', packs.O_LITERAL)
    slash = t.find_or_add('a/b.rpk', packs.O_LITERAL)
    long_name = 'd\\' + 'n' * 70 + '.rpk'
    cut1 = t.find_or_add(long_name, packs.O_LITERAL)
    cut2 = t.find_or_add(long_name, packs.O_LITERAL)
    accent = t.find_or_add('caf\xc9.rpk', packs.O_LITERAL)
    accent2 = t.find_or_add('caf\xe9.rpk', packs.O_LITERAL)
    g.check('find-or-add: record 0 is system.rpk; A to Z are folded and nothing else; the '
            'first spelling stays; "/" and "\\" are different names; a name is cut to 63 '
            'characters and then never found again',
            t.records[0].name == 'system.rpk' and first == again == 1 and
            t.records[1].name == 'A\\b.RPK' and slash == 2 and cut1 != cut2 and
            t.records[cut1].name == long_name[:63] == t.records[cut2].name and
            accent != accent2 and t.find('SYSTEM.RPK') == 0 and
            t.find('nowhere.rpk') == packs.NONE and t.name_of('a\\b.rpk') == 'A\\b.RPK')
    n_before = len(t.records)
    name = t.canonical('a\\b.rpk')
    g.check('loading a pack registers the names of its external reference table as the '
            'file spells them, and does not load them',
            name == 'A\\b.RPK' and len(t.records) == n_before + 1 and
            t.records[n_before].name == 'Ext\\One.rpk' and
            t.records[n_before].origin == packs.O_EXTERNAL and
            not t.records[n_before].loaded and t.records[1].ext == [1, n_before, 0])
    n_before = len(t.records)
    gone = t.canonical('Gone\\Pack.rpk')
    g.check('a pack without a file: no name, the record with the written spelling stays, '
            'and the next question is answered the same way',
            gone is None and len(t.records) == n_before + 1 and
            t.records[n_before].name == 'Gone\\Pack.rpk' and
            t.canonical('gone\\pack.RPK') is None and len(t.records) == n_before + 1 and
            packs.resolve(t, 'gone\\pack.rpk', 7) == ('system.rpk', 0, packs.R_NO_FILE))
    listing2 = ['parts\\One.rpk', 'parts\\engines\\Two.RPK', 'parts\\deep\\er\\Three.rpk',
                'Multibot\\Maps\\Ring\\t_data.rpk', 'multibot\\maps\\loose.rpk',
                'elsewhere\\Four.rpk', 'parts\\notapack.txt']
    t = packs.session_940(packs.ListedPackFiles(listing2), listing2)
    g.check('a scan registers the directory as the script spells it and the file as the '
            'listing does, for the files directly in the directory',
            [r.name for r in t.records] == ['system.rpk', 'parts\\engines\\Two.RPK',
                                            'parts\\One.rpk',
                                            'multibot\\maps\\Ring\\t_data.rpk'] and
            packs.listed_directories(listing2, 'multibot\\maps\\') == ['Ring'] and
            packs.resolve(t, 'PARTS\\one.rpk', 3) == ('parts\\One.rpk', 3, packs.R_OK) and
            packs.resolve(t, 'ELSEWHERE\\four.rpk', 3) == ('ELSEWHERE\\four.rpk', 3,
                                                           packs.R_OK),
            repr([r.name for r in t.records]))
    return same, len(pool), hit, nodes, order_same, n_lit, c1, c2, n_boot, passed


def run(opts):
    oc = harness.Outcome('M9 exe rules')
    g = Checks()
    n, same_b, same_a, n_texts, exact, passed = float_part(g, opts)
    (same, n_pool, hit, nodes, order_same, n_lit, c1, c2, n_boot,
     passed_packs) = pack_part(g, opts)
    good = [what for what, why in g.results if why is None]
    for what, why in g.results:
        if why is not None:
            oc.failures.append('%s: %s' % (what, why[:400]))
    oc.text = ('floats: %d/%d class files the exe wrote byte-identical under %s (%d under A), '
               '%s equals the exact model on %d/%d literal texts; packs: %d/%d spellings of '
               'the shipped pools, %d/%d rid nodes, %d/%d literals the same in any order of '
               'compilation (%d and %d spellings without the scans); checks %d/%d' % (
                   same_b, n, EXE, same_a, EXE, exact, n_texts, same, n_pool, hit, nodes,
                   order_same, n_lit, c1, c2,
                   len(good), len(g.results)))
    oc.ok = (len(good) == len(g.results) and n == same_b == 136 and same == n_pool == 126 and
             hit == nodes == 9058 and exact == n_texts and order_same == n_lit == 9952)
    oc.passed['m9.pass'] = (['check: ' + w for w in good] + ['exe wrote ' + c for c in passed] +
                            passed_packs)
    oc.notes.append('the pack table after the start of the game and the scans: %d records'
                    % n_boot)
    for what, why in g.results:
        oc.notes.append('%s: %s' % (what, 'ok' if why is None else 'FAILED'))
    return oc


if __name__ == '__main__':
    sys.exit(harness.main_single(run, sys.argv[1:]))
