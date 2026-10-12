#!/usr/bin/env python3
"""run_tests.py - run milestone tests, print their headlines, compare with the
baseline.

    python -I tests/run_tests.py m1 m2 m3          the front half (about 25 s)
    python -I tests/run_tests.py m4                the trees (about 25 s)
    python -I tests/run_tests.py m5 m6             whole files; the sources without a
                                                   class through the command line (50 s)
    python -I tests/run_tests.py m9                the two rules read in the exe: the float
                                                   conversion, the pack table (15 s)
    python -I tests/run_tests.py                   every milestone that is built
    python -I tests/run_tests.py extra extra4 extra5
                                                   the checks outside the milestones: front
                                                   half, emitter, writer (about 1 min each)
    python -I tests/run_tests.py review            one check per defect the review found:
                                                   results, provider, parser, reader, floats,
                                                   diagnostics, driver, test gaps (1 min)
    python -I tests/run_tests.py m3 --only Trigger narrow a run (no baseline compare)
    python -I tests/run_tests.py m1 m2 m3 --update store new passes in tests/baseline
                                                   (refused when a pass was lost)
    python -I tests/run_tests.py m1 --impl cpp     the C++ compiler (milestone M7)

Each milestone prints one headline; the last line counts the baseline items
that no longer pass.  Exit code 0 when every headline shows what its milestone
asks for and nothing was lost, 1 otherwise, 2 for a milestone that is not
built.

What the front half must print:

    M1 tokenise      3454/3454 sources, 852211 tokens
    M2 parse         3454/3454 sources
    M3 declarations  2651/2651 oracle pairs (2705/2705 classes; CLSS 2705/2705, FILD 238/238, MTHD 2701/2701; 31141 prefix entries); other pairs 168/168; equal to poolmodel 3508/3508 classes
    baseline         0 lost

and the trees:

    M4 trees         7568/7570 node for node (float setting per file; 7251/7570 with setting B, the conversion of the exe, alone; 7396/7570 with setting A alone); older builds 154/154; stale 3/18; equal to the spike 3454/3454 sources; pack table replay 126/126 spellings of the shipped pools, 9058/9058 rid nodes, 2/2 for the pack without a file

the files, and the sources without a class file:

    M5 files         2649/2651 byte-identical (float setting per file; 2434/2651 with setting B, the conversion of the exe, alone; 2561/2651 with setting A alone); written by the Build 940 exe 136/136 under B; older builds 153/153; stale 0/15; CONS exact 2703/2705 classes; equal to the spike 3454/3454 sources
    M6 source-only   635/635 compiled through the cache (635 classes, 771 trees, 32644 nodes, 693847 bytes); hashes 635/635 equal to the baseline (float setting B; under setting A 10 files differ, 33 literals); strict reader 635/635; layout rules hold; stack simulation holds (21523 nodes); structure 767/767 trees; siblings 522/522; manifest 635/635 entries; driver checks 23/23

and the two rules that were read in the exe (notes/exe_reads.md):

    M9 exe rules     floats: 136/136 class files the exe wrote byte-identical under B (90 under A), B equals the exact model on 15954/15954 literal texts; packs: 126/126 spellings of the shipped pools, 9058/9058 rid nodes, 9952/9952 literals the same in any order of compilation (119 and 123 spellings without the scans); checks 25/25
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness  # noqa: E402

# milestone -> module that holds run(opts)
TESTS = [
    ('m1', 't1_lex'),
    ('m2', 't2_parse'),
    ('m3', 't3_decl'),
    ('m4', 't4_trees'),
    ('m5', 't5_files'),
    ('m6', 't6_source_only'),
    ('m9', 't9_exe'),
    ('extra', 't_extra'),
    ('extra4', 't_extra4'),
    ('extra5', 't_extra5'),
    ('review', 't_review'),
]
EXTRAS = ('extra', 'extra4', 'extra5', 'review')    # not run unless named


def main(argv):
    opts = harness.parse_args(argv)
    try:
        harness.require_impl(opts.impl)
    except harness.NotBuilt as x:
        print(x)
        return 2
    known = dict(TESTS)
    names = opts.names
    built = [n for n, mod in TESTS
             if os.path.exists(os.path.join(harness.HERE, mod + '.py'))]
    if not names:
        names = [n for n in built if n not in EXTRAS]
    for n in names:
        if n not in known:
            print('unknown test %s (one of: %s)' % (n, ' '.join(k for k, _ in TESTS)))
            return 2
        if n not in built:
            print('%s: not built yet (tests/%s.py)' % (n, known[n]))
            return 2
    why = harness.package_is_self_contained()
    if why:
        print('FAIL the package is not self-contained: %s' % why)
        return 1
    outcomes = []
    for n in names:
        mod = __import__(known[n])
        outcomes.append(mod.run(opts))
    return harness.settle(outcomes, opts)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
