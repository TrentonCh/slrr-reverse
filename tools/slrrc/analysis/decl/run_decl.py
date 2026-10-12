#!/usr/bin/env python3
"""run_decl.py - run every script of analysis/decl in order and write their
output under analysis/decl/out/.  About 110 seconds.

    python -I run_decl.py
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

STEPS = (
    ('universe.py', 'class universe (packages and classes of the corpus)'),
    ('check_parse.py', 'the analysis parser accepts every source'),
    ('check_decl.py', 'CONS, CLSS, FILD, MTHD byte for byte on every pair'),
    ('check_alternatives.py', 'each rule switched to an alternative'),
    ('check_tail.py', 'the pool rules without a source, on every blob'),
    ('check_source_only.py', 'the 635 sources without a class file'),
    ('check_bridge.py', 'the same model on lib/parser.py'),
    ('study_refclass.py', 'which class a member ref names'),
    ('stats_decl.py', 'census for notes/declarations.md'),
)


def main():
    out = os.path.join(HERE, 'out')
    os.makedirs(out, exist_ok=True)
    rc = 0
    for script, what in STEPS:
        t = time.time()
        p = subprocess.run([sys.executable, '-I', os.path.join(HERE, script)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        text = p.stdout.decode('utf-8', 'replace')
        with open(os.path.join(out, script[:-3] + '.log'), 'w', encoding='utf-8') as f:
            f.write(text)
        lines = text.rstrip().split('\n')
        print('%-24s %5.1fs  rc=%d  %s' % (script, time.time() - t, p.returncode, what))
        print('      ' + lines[0][:150])
        if p.returncode:
            rc = 1
            print('\n'.join('      ' + x for x in lines[-8:]))
    return rc


if __name__ == '__main__':
    sys.exit(main())
