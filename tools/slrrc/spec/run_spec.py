#!/usr/bin/env python3
"""run_spec.py - regenerate every number notes/compiler_spec.md quotes from the
scripts in this directory, and compare the headline numbers with the ones the
document states.

    python -I run_spec.py            everything (about 2 minutes once the float
                                     results are cached; about 10 the first time)
    python -I run_spec.py --quick    skip float_models.py (uses its cache)

Exit code 1 when a step fails or a headline number differs.

Prerequisites: the corpus index and the analysis caches exist, i.e. these have
been run at least once:
    python -I ../lib/run_all.py
    python -I ../lib/run_language.py
    python -I ../analysis/decl/run_decl.py
    python -I ../analysis/expr/run_all.py
    python -I ../analysis/stmt/run_all.py
"""
import os
import re
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(_HERE, 'out')

STEPS = [
    ('float_models.py', ['--json', os.path.join(OUT, 'float_models.json')], 'float_models.txt', True),
    ('float_models.py', ['--set', 'other', '--json', os.path.join(OUT, 'float_models_other.json')],
     'float_models_other.txt', True),
    ('float_files.py', [], 'float_files.txt', False),
    ('float_closed_forms.py', [], 'float_closed_forms.txt', False),
    ('rid_packs.py', ['--json', os.path.join(OUT, 'rid_packs.json')], 'rid_packs.txt', False),
    ('blockers.py', ['--json', os.path.join(OUT, 'blockers.json')], 'blockers.txt', False),
    ('stale_detect.py', ['--json', os.path.join(OUT, 'stale_detect.json')], 'stale_detect.txt', False),
    ('decl_expected.py', [], 'decl_expected.txt', False),
    ('expected_totals.py', [], 'expected_totals.txt', False),
    ('spike_check.py', ['--json', os.path.join(OUT, 'spike_check.json')], None, False),
    ('spike_check.py', ['--packs', 'disk'], None, False),
    ('spike_check.py', ['--packs', 'written'], None, False),
    ('spike_source_only.py', [], 'spike_source_only.txt', False),
    ('spike_parsers.py', [], 'spike_parsers.txt', False),
    # the foundation's 30 layout rules on the 635 compiled class files
    (os.path.join('..', 'lib', 'check_layout.py'),
     ['--corpus', os.path.join(OUT, 'source_only_classes')], 'source_only_layout.txt', False),
]

# (file under out/, regular expression, expected groups, what it is)
HEADLINES = [
    ('decl_expected.txt', r'^M3 declarations (\d+)/(\d+)', ('2651', '2651'),
     'declarations and pool prefix from the source alone, oracle pairs'),
    ('spike_check.txt', r'^oracle\s+pairs\s+(\d+)\s+byte-identical\s+(\d+) \(float setting A alone:\s+(\d+)\)',
     ('2651', '2649', '2561'), 'spike: oracle pairs byte-identical (per-file float setting / A alone)'),
    ('spike_check.txt', r'^\s+trees\s+(\d+)\s+node for node\s+(\d+) \(float setting A alone:\s+(\d+)\)',
     ('7570', '7568', '7396'), 'spike: oracle trees node for node'),
    ('spike_check.txt', r'^older\s+pairs\s+(\d+)\s+byte-identical\s+(\d+)', ('153', '153'),
     'spike: older-build pairs byte-identical'),
    ('spike_check.txt', r'^stale\s+pairs\s+(\d+)\s+byte-identical\s+(\d+)', ('15', '0'),
     'spike: stale pairs (control)'),
    ('spike_check_disk.txt', r'^oracle\s+pairs\s+(\d+)\s+byte-identical\s+(\d+)', ('2651', '2644'),
     'spike: pack names as on disk'),
    ('spike_check_written.txt', r'^oracle\s+pairs\s+(\d+)\s+byte-identical\s+(\d+)', ('2651', '2581'),
     'spike: pack names as written'),
    ('spike_source_only.txt', r'^\s+compiled\s+(\d+) \((\d+) classes, (\d+) trees', ('635', '635', '771'),
     'spike: sources without a class compiled'),
    ('spike_source_only.txt', r'every tree splits into statement forms\s+(\d+) of (\d+) files',
     ('635', '635'), 'spike: source-only trees split into statement forms'),
    ('spike_source_only.txt', r'twins with every token on the same line: (\d+); .* equal: (\d+)',
     ('522', '522'), 'spike: source-only classes equal in shape to a shipped twin'),
    ('spike_source_only.txt', r'stack simulation .* X1 (\d+) of (\d+)', ('21523', '21523'),
     'spike: stack simulation of the source-only trees (X1)'),
    ('source_only_layout.txt', r'^(all MUST rules hold)', ('all MUST rules hold',),
     'spike: layout rules on the source-only class files'),
    ('spike_parsers.txt', r'^sources (\d+), identical output under both parser sets (\d+)',
     ('3454', '3454'), 'spike: same bytes from lib/parser.py and from the analysis parsers'),
    ('float_models.txt', r'^\s+div pc24 v24 s24 c24\s+(\d+) more files', ('2426',),
     'float: files reproduced by setting A'),
    ('float_models.txt', r'files no setting reproduces: (\d+)', ('0',),
     'float: files no setting reproduces'),
    ('float_closed_forms.txt', r'setting D: closed form equals the exact simulation for (\d+) of (\d+)',
     ('15775', '15775'), 'float: the four conversions in plain float / double arithmetic'),
    ('rid_packs.txt', r'rule disk\s+holds for (\d+) of (\d+) nodes', ('9046', '9058'),
     'pack names: disk rule'),
    ('stale_detect.txt', r'^members\s+(\d+)', ('0',), 'stale detection: member rows flag'),
]


def main(argv):
    quick = '--quick' in argv
    os.makedirs(OUT, exist_ok=True)
    bad = 0
    t00 = time.time()
    for script, args, outfile, slow in STEPS:
        if quick and slow:
            print('%-22s skipped (--quick)' % script)
            continue
        t0 = time.time()
        p = subprocess.run([sys.executable, '-I', os.path.join(_HERE, script)] + args,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=_HERE)
        text = p.stdout.decode('utf-8', 'replace')
        if outfile:
            with open(os.path.join(OUT, outfile), 'w', encoding='utf-8', newline='\n') as f:
                f.write(text.replace('\r\n', '\n'))
        last = [ln for ln in text.splitlines() if ln.strip()][-1:] or ['']
        print('%-22s %-28s exit %d  %5.1f s  %s' % (script, ' '.join(a for a in args if not a.startswith(OUT))[:28],
                                                  p.returncode, time.time() - t0, last[0][:70]))
        if p.returncode != 0:
            bad += 1
    print()
    for fname, rx, want, what in HEADLINES:
        path = os.path.join(OUT, fname)
        got = None
        if os.path.exists(path):
            with open(path, encoding='utf-8') as f:
                for ln in f:
                    m = re.search(rx, ln)
                    if m:
                        got = m.groups()
                        break
        ok = got == want
        bad += not ok
        print('%-76s %-22s %s' % (what, ' / '.join(got) if got else 'not found',
                                 'ok' if ok else 'EXPECTED ' + ' / '.join(want)))
    print('total %.0f s, %s' % (time.time() - t00, 'all as stated' if not bad else '%d problem(s)' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
