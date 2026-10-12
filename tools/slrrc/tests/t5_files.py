#!/usr/bin/env python3
"""t5_files.py - milestone M5: whole class files, byte for byte.

    python -I tests/t5_files.py [--only TEXT] [-v] [--update]

slrrc.compile_source() is given the source text, the CorpusProvider (the other
classes of the corpus), the pack names of the corpus and the options; it
returns the bytes of the class file.  The class file of the source is opened
only to compare.

  oracle   the 2,651 Build 940 pairs: the bytes equal the shipped class file.
           Compared three times: with the float setting
           tests/baseline/m5_float.tsv names for the file (A for a file it
           does not list), with setting B for all, and with setting A for
           all.  Two files cannot be reproduced from the install
           (tests/baseline/expected_fail.tsv: compiled against another
           version of a class they call).
  exe      THE FLOAT RULE.  The Build 940 exe has one float conversion,
           setting B (notes/exe_reads.md 1).  The 136 class files of the
           corpus that this exe itself wrote (tests/baseline/m5_exe940.tsv)
           must all be byte-identical under B, with no per-file choice; the
           settings that reproduce each of them are computed and compared
           with the list (40 are identical under B only, 6 under B and D
           only).  The other 2,515 files are as Steam shipped them.  B alone
           reproduces 2,298 of them; 215 need a conversion the shipped exe
           does not contain, the same routine in x87 arithmetic (A, C or D),
           and 2 are the expected failures.  WHAT STAYS UNEXPLAINED: which of
           the three x87 forms a shipped file needs.  Nothing in the file or
           in the install says, the builds that wrote them are not at hand,
           so no rule is made up for it: the table m5_float.tsv stays, and
           -v prints the 215 by the settings that reproduce them.
  CONS     per class of the oracle pairs: the constant pool chunk equals the
           shipped one byte for byte (the float setting does not reach it)
  older    the pairs written by older builds, compiled with the build number
           of the shipped file (setting A)
  stale    the pairs whose source was edited after the compile: a control,
           none may come out equal
  spike    every source of the corpus, the 635 without a class included: the
           bytes equal the ones of spec/spike_compiler.py (the composition of
           the analysis models), setting A

Every source is compiled in strict mode: a diagnostic of any kind fails the
file, so a pass also says that the source holds no construct outside the
proven subset.  (The one exception is written down in harness.py: the two
W_PACK warnings of TimeAttackTest, which must come.)

Checks on every output, whatever it is compared with:
  reader   lib/tufa.py (the strict reader of the foundation) reads it back,
           validate() finds nothing, and it serialises to the same bytes
  names    one blob per class of the source, in order, with its name
  pool     the five steps only ever append: the prefix of M3 is the head of
           the pool, every type carrier names an entry of step 2, and writing
           the trees interned nothing new (the passes covered every operand)
  nodes    a node has an operand exactly when the opcode table of the
           foundation says so
  float    the bytes do not depend on the float setting when the source has
           no float literal

Controls printed with -v (not part of the headline):
  the float table is minimal: a file listed under B, C or D differs under
           every earlier setting
  the time stamps of this copy of the corpus: the listed 136 files, and no
           other pair, carry the stamp the exe gives a class it writes (said,
           not required: a copy can lose its time stamps)
  the two expected failures become byte-identical when the provider answers
           with the declaration their own class file records for the callee

Headline:
  M5 files         2649/2651 byte-identical (float setting per file; 2434/2651
  with setting B, the conversion of the exe, alone; 2561/2651 with setting A
  alone); written by the Build 940 exe 136/136 under B; older builds 153/153;
  stale 0/15; CONS exact 2703/2705 classes; equal to the spike 3454/3454
  sources
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness              # noqa: E402
import oracle               # noqa: E402  lib
import tufa                 # noqa: E402  lib
import spike_check          # noqa: E402  spec
import spike_compiler       # noqa: E402  spec

import slrrc                        # noqa: E402
from slrrc import classes           # noqa: E402
from slrrc import classfile         # noqa: E402
from slrrc import emit              # noqa: E402
from slrrc import floatlit          # noqa: E402
from slrrc import scanner as sc     # noqa: E402

MODES = 'ABCD'
EXE = floatlit.EXE_MODE     # 'B': the one conversion of the Build 940 exe
NS = 1000000000


def exe_written():
    """class file -> the float settings that reproduce it ('=': no float
    literal), for the class files the Build 940 exe itself wrote
    (tests/baseline/m5_exe940.tsv says how they were told apart)."""
    out = {}
    for line in harness.Baseline('m5_exe940.tsv').load() or []:
        cls, modes = line.split('\t')
        out[cls] = modes
    return out


def exe_stamp(ns):
    """The write time the exe gives a class whose source was written at ns
    (nanoseconds): the DOS time of the source, even seconds rounded up, of
    which it keeps the steps of 4 seconds."""
    sec = (ns + 2 * NS - 1) // (2 * NS) * 2
    return sec // 4 * 4


def time_stamps(entries, listed):
    """How the write times of this copy of the corpus agree with the list:
    (listed files that carry the stamp of the exe, listed files, other pair
    class files that carry it)."""
    root = oracle.corpus_root()
    with_stamp = others = n_listed = 0
    for e in entries:
        if not e.is_pair:
            continue
        try:
            cm = os.stat(os.path.join(root, e.cls.replace('/', os.sep))).st_mtime_ns
            sm = os.stat(os.path.join(root, e.source.replace('/', os.sep))).st_mtime_ns
        except OSError:
            continue
        stamp = cm % NS == 0 and cm // NS == exe_stamp(sm)
        if e.cls in listed:
            n_listed += 1
            with_stamp += stamp
        else:
            others += stamp
    return with_stamp, n_listed, others


def float_table():
    """class file -> float setting, from tests/baseline/m5_float.tsv ('-':
    the file has no float literal)."""
    out = {}
    for line in harness.Baseline('m5_float.tsv').load() or []:
        cls, mode = line.split('\t')
        out[cls] = mode
    return out


def expected_failures():
    """class file -> reason, for the oracle pairs that cannot be reproduced
    from the install (the stale pairs of that file are not oracle pairs)."""
    out = {}
    for line in harness.Baseline('expected_fail.tsv').load() or []:
        cols = line.split('\t')
        if cols[1] != 'stale':
            out[cols[0]] = cols[3]
    return out


def why_different(mine, theirs):
    """One line that says where two class files part."""
    if not mine:
        return 'no output'
    try:
        r, det = spike_check.classify(mine, theirs)
    except Exception as x:      # noqa: the classifier reads what it is given
        return 'not comparable: %s' % x
    return '%s: %s' % (r, det)


def check_output(res, text, prefix_lens):
    """What must hold for every output: None, or what is wrong."""
    data = res.class_file
    try:
        blobs = tufa.parse(data)
    except tufa.TufaError as x:
        return 'the strict reader rejects it: %s' % x
    for b in blobs:
        bad = tufa.validate(b)
        if bad:
            return 'validate: %s' % bad[0]
    if tufa.serialise(blobs) != data:
        return 'does not serialise to the same bytes'
    names = [b.name for b in blobs]
    if names != res.class_names:
        return 'blob names %r, class names %r' % (names[:3], res.class_names[:3])
    if len(res.classes) != len(blobs):
        return '%d blobs, %d classes' % (len(blobs), len(res.classes))
    for cb, b, plen in zip(res.classes, blobs, prefix_lens):
        steps = cb.pool_steps
        if len(steps) != 5 or steps != sorted(steps):
            return '%s: pool steps %r' % (cb.info.name, steps)
        if steps[2] != plen:
            return '%s: prefix of %d entries, the declarations alone give %d' % (
                cb.info.name, steps[2], plen)
        if steps[4] != len(cb.pool.entries):
            return '%s: %d entries interned while the trees were written' % (
                cb.info.name, len(cb.pool.entries) - steps[4])
        if len(b.cons) != steps[4]:
            return '%s: %d entries in the file, %d in the pool' % (
                cb.info.name, len(b.cons), steps[4])
        if len(cb.trees) != len(b.trees):
            return '%s: %d trees in the file, %d emitted' % (
                cb.info.name, len(b.trees), len(cb.trees))
        for ti, tree in enumerate(cb.trees):
            for n in tree.nodes:
                if (n.kind != emit.K_NONE) != tufa.HAS_IMM[n.op]:
                    return '%s tree %d: op 0x%02X with operand kind %d' % (
                        cb.info.name, ti, n.op, n.kind)
                if n.op == emit.OP_TYPE:
                    ix = cb.pool.find_utf8(tree.operands[n.value].text)
                    if not ix < steps[1]:
                        return '%s tree %d: type carrier %r is not an entry of step 2' % (
                            cb.info.name, ti, tree.operands[n.value].text)
    return None


def cons_equal(mine, theirs):
    """(classes whose CONS chunk is byte-identical, classes of the shipped
    file, their names)."""
    b = tufa.parse(theirs)
    try:
        a = tufa.parse(mine) if mine else []
    except tufa.TufaError:
        a = []
    same = []
    for k, y in enumerate(b):
        if k < len(a) and tufa.chunk_payload(a[k], 'CONS') == tufa.chunk_payload(y, 'CONS'):
            same.append(k)
    return same, len(b)


class PatchedProvider(classes.ClassProvider):
    """A provider that answers for one class with one method declared as a
    class file of the corpus records it (the control for the two expected
    failures)."""

    def __init__(self, inner, cls, name, desc, is_static):
        self.inner = inner
        self.cls = cls
        info = inner.find(cls)
        self.info = classes.ClassInfo(cls)
        self.info.super = info.super
        self.info.interfaces = list(info.interfaces)
        self.info.is_interface = info.is_interface
        self.info.fields = list(info.fields)
        found = False
        for m in info.methods:
            if m.name == name:
                self.info.methods.append(classes.MethodInfo(name, desc, m.is_static))
                found = True
            else:
                self.info.methods.append(m)
        if not found:
            self.info.methods.append(classes.MethodInfo(name, desc, is_static))

    def find(self, fqn):
        if fqn == self.cls:
            return self.info
        return self.inner.find(fqn)


def recorded_callee(theirs, mine_blobs):
    """The first method ref of a shipped class file that the compiled file
    does not hold: (class, name, descriptor), or None."""
    have = set()
    for b in mine_blobs:
        for i, ent in enumerate(b.cons):
            if ent[0] == 'ref':
                have.add(b.ref(i))
    for b in theirs:
        for i, ent in enumerate(b.cons):
            if ent[0] == 'ref' and b.ref(i)[2].startswith('(') and b.ref(i) not in have:
                return b.ref(i)
    return None


def run(opts):
    oc = harness.Outcome('M5 files')
    prov = harness.provider()
    pk = harness.packs()
    table = float_table()
    exe = exe_written()
    expected = expected_failures()
    env = spike_compiler.Env()
    oracle_keys = set(e.key for e in harness.oracle_pairs())
    oracle_keys_cls = set(e.cls for e in harness.oracle_pairs())
    older_keys = set(e.key for e in harness.older_pairs())
    c = {}
    passed = []             # oracle class files, float setting per file
    passed_older = []
    passed_spike = []       # sources equal to the spike
    passed_cons = []        # class file#blob with an exact CONS chunk (oracle)
    passed_exe = []         # class files the exe wrote, identical under its setting
    exe_sets = {}           # settings that reproduce a file the exe wrote -> files
    shipped_sets = {}       # ... a shipped file that B does not reproduce -> files
    list_bad = []
    n_sources = 0
    notes_expected = []
    table_bad = []
    by_mode = {}

    def count(key, n=1):
        c[key] = c.get(key, 0) + n

    def compile_(text, name, mode, build, provider=None):
        return slrrc.compile_source(text, name, provider or prov, pk,
                                    slrrc.Options(build=build, float_mode=mode, strict=True))

    for e in harness.selected(harness.all_sources(), opts):
        n_sources += 1
        text = harness.load_source(e)
        res = compile_(text, e.source, 'A', slrrc.BUILD_940)
        odd = harness.surprises(e.source, res.diagnostics)
        clean = res.ok and not odd
        if not clean:
            oc.failures.append('%s: %s' % (e.source, odd[0] if odd else 'not ok'))
            count('not clean')
        data_a = res.class_file if clean else b''
        has_float = False
        if clean:
            front = slrrc.front(text, e.source, prov, slrrc.Options(strict=True))
            why = check_output(res, text, [len(cb.pool.entries) for cb in front.classes])
            if why is not None:
                oc.failures.append('%s: %s' % (e.source, why))
                count('output checks failed')
            has_float = any(t.kind == sc.T_FLOAT for t in res.ast.toks)
            if not has_float:
                other = compile_(text, e.source, 'D', slrrc.BUILD_940)
                if other.class_file != data_a:
                    oc.failures.append('%s: no float literal, but the bytes depend on the '
                                       'float setting' % e.source)
                    count('output checks failed')
        # ---- the spike, every source
        try:
            spike = spike_compiler.compile_source(text, env, 'A', 'table', name=e.source)[0]
        except Exception as x:      # noqa: the spike raises for everything
            spike = None
            oc.failures.append('%s: the spike does not compile it: %s: %s' % (
                e.source, type(x).__name__, str(x)[:120]))
        if spike is not None:
            if clean and spike == data_a:
                passed_spike.append(e.source)
            else:
                oc.failures.append('%s: differs from the spike: %s' % (
                    e.source, why_different(data_a, spike)))
        if not e.is_pair:
            continue
        # ---- the shipped class file
        theirs = oracle.load_class_bytes(e)
        if e.key in oracle_keys:
            grp = 'oracle'
        elif e.key in older_keys:
            grp = 'older'
        else:
            grp = 'stale'
        count(grp + ' files')
        build = e.builds[0] if e.builds else slrrc.BUILD_940
        if grp != 'stale' and clean:
            # what the source declares is what the shipped class file declares
            # (read with the package's own reader): the text a cache hashes
            shipped = classfile.read(theirs)
            for cb, b in zip(res.classes, shipped.blobs):
                count('declarations')
                info = classfile.class_info(b)
                if info is not None and classes.declaration_text(info) == \
                        classes.declaration_text(cb.info):
                    count('declarations equal')
                else:
                    oc.failures.append('%s: %s: the declarations of the source and of the '
                                       'class file differ' % (e.cls, cb.info.name))
        if grp == 'oracle':
            mode = table.get(e.cls, 'A')
            if mode == '-':
                mode = 'A'
                if has_float:
                    table_bad.append('%s: listed without a float literal, the source has one'
                                     % e.cls)
            elif e.cls in table and not has_float and clean:
                table_bad.append('%s: listed under %s, the source has no float literal'
                                 % (e.cls, mode))
            by_mode[mode] = by_mode.get(mode, 0) + 1
            ok_a = clean and data_a == theirs
            data = data_a
            if mode != 'A' and clean:
                data = compile_(text, e.source, mode, build).class_file
                # the table names the FIRST setting that reproduces the file
                for earlier in MODES[1:MODES.index(mode)]:
                    if compile_(text, e.source, earlier, build).class_file == theirs:
                        table_bad.append('%s: listed under %s, identical under %s too'
                                         % (e.cls, mode, earlier))
                if ok_a:
                    table_bad.append('%s: listed under %s, identical under A too'
                                     % (e.cls, mode))
            ok = clean and data == theirs
            if ok_a:
                count('oracle identical under A')
            # ---- the rule of the exe alone: setting B for every file
            by_setting = {'A': data_a, mode: data}
            if clean and has_float and EXE not in by_setting:
                by_setting[EXE] = compile_(text, e.source, EXE, build).class_file
            ok_b = clean and by_setting.get(EXE, data_a) == theirs
            if ok_b:
                count('oracle identical under B')
            which = '='                 # the settings that reproduce the file
            if e.cls in exe or (ok and not ok_b):
                if clean and has_float:
                    which = ''
                    for m in MODES:
                        if m not in by_setting:
                            by_setting[m] = compile_(text, e.source, m, build).class_file
                        if by_setting[m] == theirs:
                            which += m
                elif not ok_a:
                    which = ''
            if e.cls in exe:
                count('exe files')
                exe_sets[which] = exe_sets.get(which, 0) + 1
                if ok_a:
                    count('exe identical under A')
                if ok_b:
                    count('exe identical under B')
                    passed_exe.append(e.cls)
                else:
                    oc.failures.append('%s: written by the Build 940 exe, and not '
                                       'byte-identical under its setting %s: %s' % (
                                           e.cls, EXE, why_different(
                                               by_setting.get(EXE, data_a), theirs)))
                if which != exe[e.cls]:
                    list_bad.append('%s: listed as reproduced under %s, it is under %s' % (
                        e.cls, exe[e.cls], which or 'none'))
            else:
                count('shipped files')
                if ok_b:
                    count('shipped identical under B')
                elif ok:
                    shipped_sets[which] = shipped_sets.get(which, 0) + 1
            same, total = cons_equal(data, theirs)
            count('oracle classes', total)
            count('oracle classes with exact CONS', len(same))
            for k in same:
                passed_cons.append('%s#%d' % (e.cls, k))
            if ok:
                count('oracle identical')
                passed.append(e.cls)
            elif e.cls in expected and clean:
                # control: the declaration the class file itself records
                line = '%s: %s' % (e.cls, why_different(data, theirs))
                callee = recorded_callee(tufa.parse(theirs), tufa.parse(data))
                if callee is not None:
                    alt = compile_(text, e.source, mode, build,
                                   PatchedProvider(prov, callee[0], callee[1], callee[2], True))
                    line += '; with %s.%s:%s declared as the class file records it: %s' % (
                        callee[0], callee[1], callee[2],
                        'byte-identical' if alt.class_file == theirs else
                        'still different (%s)' % why_different(alt.class_file, theirs))
                    if alt.class_file == theirs:
                        count('expected failures explained')
                notes_expected.append(line)
            else:
                oc.failures.append('%s: %s' % (e.cls, why_different(data, theirs)))
        elif grp == 'older':
            data = compile_(text, e.source, 'A', build).class_file if clean else b''
            if clean and data == theirs:
                count('older identical')
                passed_older.append(e.cls)
            else:
                oc.failures.append('%s (build 0x%X): %s' % (
                    e.cls, build, why_different(data, theirs)))
        else:
            identical = False
            for mode in MODES:
                if clean and compile_(text, e.source, mode, build).class_file == theirs:
                    identical = True
            if identical:
                count('stale identical')
                oc.failures.append('%s: a stale pair came out byte-identical' % e.cls)
    for src, diags in prov.failed:
        d = diags[0]
        oc.failures.append('%s (declared for the provider): line %d: %s %s' % (
            src, d.line, d.code, d.text))
    oc.failures.extend('float table: %s' % x for x in table_bad)
    oc.failures.extend('list of the files the exe wrote: %s' % x for x in list_bad)
    missing = sorted(set(exe) - oracle_keys_cls) if opts.only is None else []
    for x in missing:
        oc.failures.append('list of the files the exe wrote: %s is no oracle pair' % x)

    def g(key):
        return c.get(key, 0)

    oc.text = ('%d/%d byte-identical (float setting per file; %d/%d with setting %s, the '
               'conversion of the exe, alone; %d/%d with setting A alone); '
               'written by the Build 940 exe %d/%d under %s; '
               'older builds %d/%d; stale %d/%d; CONS exact %d/%d classes; '
               'equal to the spike %d/%d sources' % (
                   g('oracle identical'), g('oracle files'),
                   g('oracle identical under B'), g('oracle files'), EXE,
                   g('oracle identical under A'), g('oracle files'),
                   g('exe identical under B'), g('exe files'), EXE,
                   g('older identical'), g('older files'),
                   g('stale identical'), g('stale files'),
                   g('oracle classes with exact CONS'), g('oracle classes'),
                   len(passed_spike), n_sources))
    oc.ok = (g('oracle files') == 2651 and g('oracle identical') == 2649 and
             g('oracle identical under A') == 2561 and
             g('oracle identical under B') == 2434 and
             g('exe identical under B') == g('exe files') == len(exe) == 136 and
             g('exe identical under A') == 90 and
             g('shipped files') == 2515 and g('shipped identical under B') == 2298 and
             sum(shipped_sets.values()) == 215 and not list_bad and not missing and
             g('older identical') == g('older files') == 153 and
             g('stale files') == 15 and g('stale identical') == 0 and
             g('oracle classes') == 2705 and g('oracle classes with exact CONS') == 2703 and
             len(passed_spike) == n_sources == 3454 and
             g('not clean') == 0 and g('output checks failed') == 0 and
             g('declarations equal') == g('declarations') and
             not table_bad and not prov.failed)
    oc.passed['m5.pass'] = passed
    oc.passed['m5_older.pass'] = passed_older
    oc.passed['m5_spike.pass'] = passed_spike
    oc.passed['m5_cons.pass'] = passed_cons
    oc.passed['m5_exe940.pass'] = passed_exe
    for x in notes_expected:
        oc.notes.append('expected (compiled against another version of the class it calls): '
                        '%s' % x)
    oc.notes.append('expected failures that are byte-identical once the callee is declared '
                    'as their class file records it: %d/%d' % (
                        g('expected failures explained'), len(notes_expected)))
    oc.notes.append('oracle files by float setting of the table: %s; the table names the '
                    'first setting that reproduces each file: %s' % (
                        ', '.join('%s %d' % (m, by_mode.get(m, 0)) for m in MODES),
                        'yes' if not table_bad else 'NO'))
    def sets(d):
        return ', '.join('%s %d' % (k if k != '=' else 'no float literal', d[k])
                         for k in sorted(d)) or 'none'

    oc.notes.append('the float rule: the Build 940 exe converts with setting %s (%s).  Class '
                    'files this exe wrote (tests/baseline/m5_exe940.tsv): %d, byte-identical '
                    'under %s: %d, under A: %d; by the settings that reproduce them: %s' % (
                        EXE, floatlit.ORIGIN[EXE], g('exe files'), EXE,
                        g('exe identical under B'), g('exe identical under A'),
                        sets(exe_sets)))
    oc.notes.append('class files as Steam shipped them: %d, byte-identical under %s alone: %d.  '
                    'Not under %s: %d that only a conversion of an earlier build reproduces '
                    '(%s) and %d expected failures.  Not explained: which of A, C, D such a '
                    'file needs; it is listed per file in m5_float.tsv' % (
                        g('shipped files'), EXE, g('shipped identical under B'), EXE,
                        sum(shipped_sets.values()), sets(shipped_sets),
                        g('shipped files') - g('shipped identical under B') -
                        sum(shipped_sets.values())))
    if opts.only is None:
        st = time_stamps(harness.all_sources(), exe)
        oc.notes.append('time stamps of this copy of the corpus: %d of the %d listed class '
                        'files carry the write time the exe gives a class it compiles (a whole '
                        'second, the DOS time of the source); %d other pair class files do' % st)
    oc.notes.append('writer version %s; outputs checked with the strict reader: %d' % (
        slrrc.VERSION, n_sources - g('not clean')))
    oc.notes.append('classes of the consistent pairs whose declaration text '
                    '(classes.declaration_text) is the same from the source and from the '
                    'shipped class file: %d/%d' % (g('declarations equal'), g('declarations')))
    return oc


if __name__ == '__main__':
    sys.exit(harness.main_single(run, sys.argv[1:]))
