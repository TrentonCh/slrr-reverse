#!/usr/bin/env python3
"""run_all.py - run every check of the statement study and rewrite the notes.

    python -I run_all.py

Order matters: check_stmt.py writes the JSON that line_risk.py reads, and
make_notes.py reads all of them.  Needs notes/pairs.json of the foundation
(lib/oracle.py builds it when it is missing).  Exit status 1 when a step
fails or a headline result is not what notes/statements.md states.
"""
import json
import os
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))

STEPS = [
    'parse_all.py',
    'check_parsers.py',
    'check_stmt.py',
    'check_stmt.py --lib',
    'check_jumps.py',
    'check_destruct.py',
    'tree_shape.py',
    'census_forms.py',
    'line_risk.py',
    'selftest_destruct.py',
    'make_notes.py',
]


def main():
    rc = 0
    t00 = time.time()
    for s in STEPS:
        t0 = time.time()
        parts = s.split()
        p = subprocess.run([sys.executable, '-I', os.path.join(_HERE, parts[0])] + parts[1:],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        dt = time.time() - t0
        tail = p.stdout.decode('utf-8', 'replace').strip().splitlines()
        print('%-20s exit %d  %5.1f s   %s' % (s, p.returncode, dt, tail[-1][:90] if tail else ''))
        if p.returncode != 0 and s != 'parse_all.py':
            rc = 1
        if p.returncode != 0 and s == 'parse_all.py':
            rc = 1
    out = os.path.join(_HERE, 'out')
    try:
        cs = json.load(open(os.path.join(out, 'check_stmt.json')))['totals']['940']
        cj = json.load(open(os.path.join(out, 'check_jumps.json')))['counts']
        cd = json.load(open(os.path.join(out, 'check_destruct.json')))
        lr = json.load(open(os.path.join(out, 'line_risk.json')))
        split = sum(g.get('trees split into statement forms', 0) for g in cd['totals'].values())
        cl = json.load(open(os.path.join(out, 'check_stmt_lib.json')))['totals']['940']
        heads = [
            ('oracle trees reproduced with every line', cs['trees reproduced with every line'], cs['trees']),
            ('the same on the trees of lib/parser.py', cl['trees reproduced with every line'], cs['trees']),
            ('oracle nodes with the predicted line', cs['nodes in trees reproduced with every line'], cs['nodes']),
            ('jumps inside the tree on a dispatched node', cj['J2 holds'], cj['jumps']),
            ('trees split into statement forms', split, cj['trees'] - 1),
            ('line-risk nodes in class-less sources', sum(r[2] for r in lr['at_risk']), 0),
        ]
        ra = open(os.path.join(out, 'check_stmt.txt')).read().split('\n')[1:]
        rb = open(os.path.join(out, 'check_stmt_lib.txt')).read().split('\n')[1:]
        heads.append(('check_stmt report equal under both parsers (1 = yes)', int(ra == rb), 1))
        for label, got, want in heads:
            ok = got == want
            print('%-46s %s of %s  %s' % (label, got, want, 'ok' if ok else 'DIFFERENT'))
            if not ok:
                rc = 1
    except (OSError, KeyError, ValueError) as x:
        print('cannot read the results: %s' % x)
        rc = 1
    print('total %.1f s, exit %d' % (time.time() - t00, rc))
    return rc


if __name__ == '__main__':
    sys.exit(main())
