#!/usr/bin/env python3
"""run_all.py - regenerate everything of the expression study.

    python -I run_all.py [--quick]

Runs the scripts of this directory in order, writes their output to out/ (a
.txt with what they print and, where they have one, a .json) and finally
notes/expressions.md.  --quick skips the control runs of study_variants.py
(about 3.5 of the 5 minutes) and keeps the existing out/variants.json.

The corpus is read from D:\\SLRR-test (or SLRR_CORPUS); nothing is written
outside C:\\Users\\trent\\source\\repos\\TrentonCh\\slrr-ghidra\\compiler.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')

STEPS = [
    # (script, arguments, name of the .txt, name of the .json or None)
    ('classdb.py', [], 'classdb', None),
    ('jparse.py', [], 'parse', None),
    ('probe_names.py', [], 'names', 'names'),
    ('check_expr.py', ['--set', '940', '--rules'], 'check_940', 'check_940'),
    ('check_expr.py', ['--set', 'other', '--rules'], 'check_other', 'check_other'),
    ('check_expr.py', ['--set', 'consistent', '--rules'], 'check_consistent', 'check_consistent'),
    ('check_expr.py', ['--set', 'stale'], 'check_stale', 'check_stale'),
    ('check_decomp.py', ['--show', '12'], 'decomp', 'decomp'),
    ('study_calls.py', [], 'calls', 'calls'),
    ('study_lines.py', ['--set', 'consistent'], 'lines', 'lines'),
    ('study_variants.py', [], 'variants', 'variants'),
    ('study_duplicates.py', [], 'duplicates', None),
    ('probe_source_only.py', [], 'source_only', 'source_only'),
    ('probe_precedence.py', [], 'precedence', 'precedence'),
    ('probe_stmt_roots.py', [], 'stmt_roots', 'stmt_roots'),
    ('make_notes.py', [], 'make_notes', None),
    ('lint_notes.py', [], 'lint_notes', None),
]


def main(argv):
    quick = '--quick' in argv
    os.makedirs(OUT, exist_ok=True)
    failed = 0
    for script, args, txt, js in STEPS:
        if quick and script == 'study_variants.py' and \
                os.path.exists(os.path.join(OUT, 'variants.json')):
            print('%-24s skipped (--quick)' % script)
            continue
        cmd = [sys.executable, '-I', os.path.join(HERE, script)] + args
        if js:
            cmd += ['--json', os.path.join(OUT, js + '.json')]
        t0 = time.time()
        p = subprocess.run(cmd, cwd=HERE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        text = p.stdout.decode('utf-8', 'replace').replace('\r\n', '\n')
        with open(os.path.join(OUT, txt + '.txt'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        last = text.strip().split('\n')[-1] if text.strip() else ''
        print('%-24s %-22s %5.1f s  exit %d  %s' % (script, ' '.join(args)[:22],
                                                   time.time() - t0, p.returncode, last[:70]))
        sys.stdout.flush()
        if p.returncode:
            failed += 1
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
