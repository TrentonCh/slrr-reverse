#!/usr/bin/env python3
"""gen_grammar_md.py - write notes/grammar.md from lib/grammar.md.in.

The prose and the EBNF of grammar.md are written by hand in grammar.md.in;
every number in it is a placeholder that this script fills from the outputs
of the proofs, so the document cannot drift from the scripts:

    {{c KEY}}        count of census key KEY (notes/grammar_census.json)
    {{z KEY}}        the same, but the count must be 0 and KEY must be a key
                     the census can produce (guards against a mistyped key)
    {{f KEY}}        number of source files in which KEY occurs
    {{ex KEY}}       the examples of KEY, as a code block
    {{p ID}}         "N of M" of rule ID of check_parse.py
    {{m ID}}         "N of M" of rule ID of check_members.py
    {{sh PATH}}      a number of check_shape.py: stats.X, lit.X, head.X,
                     type.X, ev.X, cat.CATEGORY.X
    {{se KEY.FIELD}} a mutation of check_sensitivity.py (no_parse, trees, pairs)
    {{sc KEY}}       its census of places where a decision matters
    {{adj A B}}      operators of level A applied last, level B applied first
    {{d KEY}}        a count of study_class_dirs.py ({{dx}}, {{pk}}, {{dd}}: its
                     package counts, classes per package, directories per class)
    {{ms KEY}} {{mx KEY}} {{sm KEY}}   statistics of check_members.py
    {{ps KEY}} {{tr KEY}} {{cat KEY}}  statistics of check_parse.py / census.py
    {{table NAME}}   a generated table (TABLES below)

An unknown placeholder or key stops the script and lists all of them.

    python -I gen_grammar_md.py
"""
import io
import json
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import lexer   # noqa: E402
import parser  # noqa: E402

NOTES = os.path.normpath(os.path.join(_HERE, '..', 'notes'))
TEMPLATE = os.path.join(_HERE, 'grammar.md.in')
OUT = os.path.join(NOTES, 'grammar.md')


def load(name):
    with io.open(os.path.join(NOTES, name), encoding='utf-8') as f:
        return json.load(f)


def num(n):
    return '{:,}'.format(n)


class Gen(object):
    def __init__(self):
        self.census = load('grammar_census.json')
        self.members = load('members_check.json')
        self.parse = load('parse_check.json')
        self.shape = load('shape_check.json')
        self.sens = load('sensitivity.json')
        self.dirs = load('class_dirs.json')
        self.c = self.census['counts']
        self.errors = []
        with io.open(os.path.join(_HERE, 'census.py'), encoding='utf-8') as f:
            self.census_src = f.read()
        self.sh = {}
        for k, v in self.shape['stats'].items():
            self.sh['stats.' + k] = v
        for k, v in self.shape['lit_stats'].items():
            self.sh['lit.' + k] = v
        for k, v in self.shape['head_stats'].items():
            self.sh['head.' + k] = v
        for k, v in self.shape['type_steps'].items():
            self.sh['type.' + k] = v
        for k, v in self.shape['events'].items():
            self.sh['ev.' + k] = v
        for cat, d in self.shape['per_category'].items():
            for k, v in d.items():
                self.sh['cat.%s.%s' % (cat, k)] = v
        self.mut = dict((r['key'], r) for r in self.sens['mutations'])
        self.adj = dict(((r['outer'], r['inner']), r['count'])
                        for r in self.sens['adjacent_levels'])

    def err(self, text):
        self.errors.append(text)
        return '??'

    # -- scalar placeholders ---------------------------------------------------
    def do_c(self, key):
        if key not in self.c:
            return self.err('census key %r' % key)
        return num(self.c[key])

    def possible(self, key):
        """Can the census produce this key at all?"""
        head, _, rest = key.partition(' ')
        if key.startswith('note.'):
            return key[5:] in parser.NOTE_CODES
        if key.startswith('node.'):
            return ("'%s'" % key[5:]) in io.open(
                os.path.join(_HERE, 'parser.py'), encoding='utf-8').read()
        if head == 'operator':
            return rest in lexer.OPERATORS
        if head == 'keyword':
            return rest in lexer.KEYWORDS
        return ("'%s'" % key) in self.census_src

    def do_z(self, key):
        if not self.possible(key):
            return self.err('zero key %r is not a key the census can produce' % key)
        if self.c.get(key, 0) != 0:
            return self.err('zero key %r has count %d' % (key, self.c[key]))
        return '0'

    def do_f(self, key):
        if key not in self.census['files']:
            return self.err('census files key %r' % key)
        return num(self.census['files'][key])

    def do_ex(self, key):
        ex = self.census['examples'].get(key)
        if not ex:
            return self.err('no examples for %r' % key)
        return '```\n' + '\n'.join(ex) + '\n```'

    def rule(self, doc, rid, what):
        for r in doc['rules']:
            if r['id'] == rid:
                return '%s of %s' % (num(r['holds']), num(r['of']))
        return self.err('%s rule %r' % (what, rid))

    def do_p(self, rid):
        return self.rule(self.parse, rid, 'parse')

    def do_m(self, rid):
        return self.rule(self.members, rid, 'members')

    def do_ms(self, key):
        if key not in self.members['stats']:
            return self.err('members stat %r' % key)
        return num(self.members['stats'][key])

    def do_ps(self, key):
        if key not in self.parse['stats']:
            return self.err('parse stat %r' % key)
        return num(self.parse['stats'][key])

    def do_sh(self, key):
        if key not in self.sh:
            return self.err('shape key %r' % key)
        return num(self.sh[key])

    def do_se(self, arg):
        key, _, field = arg.rpartition('.')
        if key not in self.mut or field not in self.mut[key]:
            return self.err('sensitivity %r' % arg)
        return num(self.mut[key][field])

    def do_sc(self, key):
        if key not in self.sens['census']:
            return self.err('sensitivity census %r' % key)
        return num(self.sens['census'][key])

    def do_adj(self, arg):
        a, b = [int(x) for x in arg.split()]
        return num(self.adj.get((a, b), 0))

    def do_d(self, key):
        if key not in self.dirs['counts']:
            return self.err('class_dirs key %r' % key)
        return num(self.dirs['counts'][key])

    def do_dc(self, arg):
        cat, _, key = arg.partition('.')
        try:
            return num(self.dirs['per_category'][cat].get(key, 0))
        except KeyError:
            return self.err('class_dirs category %r' % arg)

    def do_sm(self, key):
        if key not in self.members['scope_models']:
            return self.err('scope model %r' % key)
        return num(self.members['scope_models'][key])

    def do_mx(self, key):
        if key == 'rules':
            return num(len(self.members['rules']))
        if key in self.members['descriptors']:
            return num(self.members['descriptors'][key])
        return self.err('members extra %r' % key)

    def do_dx(self, key):
        d = self.dirs
        vals = {'packages': d['package_count'],
                'on_classpath': len(d['packages_on_classpath']),
                'off_classpath': len(d['packages_off_classpath']),
                'targets': len(d['targets'])}
        if key not in vals:
            return self.err('class_dirs extra %r' % key)
        return num(vals[key])

    def do_pk(self, key):
        if key not in self.dirs['package_sizes']:
            return self.err('package %r' % key)
        return num(self.dirs['package_sizes'][key])

    def do_dd(self, key):
        if key not in self.dirs['classes_in_several_directories']:
            return self.err('class in several directories %r' % key)
        return num(self.dirs['classes_in_several_directories'][key])

    def do_os(self, key):
        if key != 'count':
            return self.err('only_source %r' % key)
        return num(len(self.census['only_source']))

    def do_tr(self, key):
        if key not in self.census['type_res']:
            return self.err('type_res key %r' % key)
        return num(self.census['type_res'][key])

    def do_cat(self, key):
        if key not in self.census['categories']:
            return self.err('category %r' % key)
        return num(self.census['categories'][key])

    # -- tables ------------------------------------------------------------------
    def table_keywords(self):
        rows = ['| Keyword | Uses | Files |', '| --- | ---: | ---: |']
        for kw in sorted(lexer.KEYWORDS):
            n = self.c.get('keyword ' + kw, 0)
            f = self.census['files'].get('keyword ' + kw, 0)
            rows.append('| `%s` | %s | %s |' % (kw, num(n), num(f)))
        return '\n'.join(rows)

    def table_operators(self):
        rows = ['| Token | Uses | | Token | Uses | | Token | Uses |',
                '| --- | ---: | --- | --- | ---: | --- | --- | ---: |']
        ops = sorted(lexer.OPERATORS, key=lambda o: (-self.c.get('operator ' + o, 0), o))
        cells = ['`%s` | %s' % (o.replace('|', '\\|'), num(self.c.get('operator ' + o, 0)))
                 for o in ops]
        while len(cells) % 3:
            cells.append(' | ')
        n = len(cells) // 3
        for i in range(n):
            rows.append('| %s | | %s | | %s |' % (cells[i], cells[i + n], cells[i + 2 * n]))
        return '\n'.join(rows)

    def table_precedence(self):
        levels = [
            (1, ['||']), (2, ['&&']), (3, ['|']), (4, ['^']), (5, ['&']),
            (6, ['==', '!=']), (7, ['<', '>', '<=', '>=', 'instanceof']),
            (8, ['<<', '>>', '>>>']), (9, ['+', '-']), (10, ['*', '/', '%']),
        ]
        rows = ['| Level | Operators (uses) | Chains of the level | Trees that differ when the level groups to the right |',
                '| ---: | --- | ---: | ---: |']
        for lv, ops in levels:
            uses = []
            for o in ops:
                if o == 'instanceof':
                    n = self.c.get('node.InstanceOf', 0)
                else:
                    n = self.c.get('binary.' + o, 0)
                uses.append('`%s` %s' % (o.replace('|', '\\|'), num(n)))
            chains = self.sens['census'].get('chain of level %d' % lv, 0)
            m = self.mut['right %d' % lv]['trees']
            rows.append('| %d | %s | %s | %s |' % (lv, ', '.join(uses), num(chains), num(m)))
        return '\n'.join(rows)

    def table_adjacency(self):
        names = self.sens['levels']
        lv = sorted(int(k) for k in names)
        rows = ['| applied last \\ applied first | ' + ' | '.join(str(b) for b in lv) + ' |',
                '| --- | ' + ' | '.join('---:' for _ in lv) + ' |']
        for a in lv:
            cells = []
            for b in lv:
                n = self.adj.get((a, b), 0)
                cells.append('-' if a == b else (num(n) if n else '.'))
            rows.append('| %d `%s` | %s |' % (a, names[str(a)].replace('|', '\\|'),
                                             ' | '.join(cells)))
        return '\n'.join(rows)

    def table_mutations(self):
        rows = ['| Wrong grammar | Sources that no longer parse | Trees that differ | Source files affected |',
                '| --- | ---: | ---: | ---: |']
        for r in self.sens['mutations']:
            if r['key'].startswith('right ') or r['key'].startswith('swap '):
                continue
            rows.append('| %s | %s | %s | %s |' % (
                r['text'].replace('|', '\\|'), num(r['no_parse']), num(r['trees']),
                num(r['pairs'])))
        return '\n'.join(rows)

    def table_swaps(self):
        rows = ['| Neighbouring levels exchanged | Trees that differ |', '| --- | ---: |']
        for r in self.sens['mutations']:
            if r['key'].startswith('swap '):
                rows.append('| %s | %s |' % (
                    r['text'].replace('neighbouring levels ', '').replace(' exchanged', '')
                    .replace('|', '\\|'), num(r['trees'])))
        return '\n'.join(rows)

    def table_imports(self):
        rows = ['| Import | Sources | Package has classes |', '| --- | ---: | --- |']
        items = [(k[7:], v) for k, v in self.c.items() if k.startswith('import ')]
        uni = set(self.dirs['packages_on_classpath']) | set(self.dirs['packages_off_classpath'])
        for name, n in sorted(items, key=lambda kv: (-kv[1], kv[0])):
            pkg = name[:-2] if name.endswith('.*') else name
            pkg = pkg.split(':')[-1]
            rows.append('| `%s` | %s | %s |' % (name, num(n), 'yes' if pkg in uni else 'no'))
        return '\n'.join(rows)

    def table_rules_parse(self):
        rows = ['| Rule | Holds for | Statement |', '| --- | ---: | --- |']
        for r in self.parse['rules']:
            rows.append('| %s | %s of %s | %s |' % (r['id'], num(r['holds']), num(r['of']), r['text']))
        return '\n'.join(rows)

    def table_rules_members(self):
        rows = ['| Rule | Holds for | Statement |', '| --- | ---: | --- |']
        for r in self.members['rules']:
            rows.append('| %s | %s of %s | %s |' % (r['id'], num(r['holds']), num(r['of']),
                                                    r['text'].replace('|', '\\|')))
        return '\n'.join(rows)

    def table_body_forms(self):
        rows = ['| Declaration | Count | Tree in the class file |', '| --- | ---: | --- |']
        for k in sorted(self.members['body_forms']):
            what, _, tree = k.partition(' -> ')
            rows.append('| %s | %s | %s |' % (what.replace('"', '`'), num(self.members['body_forms'][k]), tree))
        return '\n'.join(rows)

    def table_nodes(self):
        items = sorted((k[5:], v) for k, v in self.c.items() if k.startswith('node.'))
        rows = ['| Node | Count | | Node | Count | | Node | Count |',
                '| --- | ---: | --- | --- | ---: | --- | --- | ---: |']
        cells = ['%s | %s' % (k, num(v)) for k, v in items]
        while len(cells) % 3:
            cells.append(' | ')
        n = len(cells) // 3
        for i in range(n):
            rows.append('| %s | | %s | | %s |' % (cells[i], cells[i + n], cells[i + 2 * n]))
        return '\n'.join(rows)

    def table_conditions(self):
        kinds = sorted(set(k.split(': ', 1)[1] for k in self.c if k.startswith('cond.all: ')),
                       key=lambda x: -self.c['cond.all: ' + x])
        rows = ['| Condition is | `if` | `while` | `do` | `for` | All |',
                '| --- | ---: | ---: | ---: | ---: | ---: |']
        for kd in kinds:
            def g(w):
                return num(self.c.get('cond.%s: %s' % (w, kd), 0))
            rows.append('| %s | %s | %s | %s | %s | %s |' % (
                kd.replace('|', '\\|'), g('if'), g('while'), g('do-while'), g('for'),
                num(self.c['cond.all: ' + kd])))
        return '\n'.join(rows)

    def prefix_table(self, prefix, head, strip=None):
        items = [(k[len(prefix):], v) for k, v in self.c.items() if k.startswith(prefix)]
        rows = ['| %s | Count |' % head, '| --- | ---: |']
        for k, v in sorted(items, key=lambda kv: (-kv[1], kv[0])):
            rows.append('| %s | %s |' % (k.replace('|', '\\|'), num(v)))
        return '\n'.join(rows)

    def table_field_modifiers(self):
        return self.prefix_table('field.modifiers ', 'Field modifiers as written')

    def table_method_modifiers(self):
        return self.prefix_table('method.modifiers ', 'Method modifiers as written')

    def table_class_modifiers(self):
        return self.prefix_table('class.modifiers ', 'Class modifiers as written')

    def table_call_targets(self):
        return self.prefix_table('call.on ', 'Receiver of a call')

    def table_field_targets(self):
        return self.prefix_table('fieldaccess.on ', 'Receiver of `.name`')

    def table_assign_ops(self):
        rows = ['| Operator | Uses | As a statement | As a value |', '| --- | ---: | ---: | ---: |']
        for op in ('=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=', '>>>='):
            n = self.c.get('assign.' + op, 0)
            v = self.c.get('assign.%s used as a value' % op, 0)
            rows.append('| `%s` | %s | %s | %s |' % (op.replace('|', '\\|'), num(n), num(n - v), num(v)))
        return '\n'.join(rows)

    def table_packs(self):
        rows = ['| Pack as written | Literals | File on disk |', '| --- | ---: | --- |']
        packs = self.census['packs']
        for p in sorted(packs, key=lambda x: (-packs[x]['literals'], x)):
            v = packs[p]
            if v['same_case']:
                continue
            rows.append('| `%s` | %s | %s |' % (p, num(v['literals']),
                                               ('`%s`' % v['file']) if v['file'] else 'none'))
        return '\n'.join(rows)

    def table_events(self):
        ev = self.shape['events']
        rows = ['| Event | Count | | Event | Count | | Event | Count |',
                '| --- | ---: | --- | --- | ---: | --- | --- | ---: |']
        cells = ['%s | %s' % (k, num(v)) for k, v in sorted(ev.items())]
        while len(cells) % 3:
            cells.append(' | ')
        n = len(cells) // 3
        for i in range(n):
            rows.append('| %s | | %s | | %s |' % (cells[i], cells[i + n], cells[i + 2 * n]))
        return '\n'.join(rows)

    def table_shape_categories(self):
        rows = ['| Category | Pairs | Trees | Same shape | Differ | Of these: a literal value only |',
                '| --- | ---: | ---: | ---: | ---: | ---: |']
        pc = self.shape['per_category']
        for cat in sorted(pc):
            c = pc[cat]
            rows.append('| %s | %s | %s | %s | %s | %s |' % (
                cat, num(c.get('pairs', 0)), num(c.get('trees', 0)), num(c.get('equal', 0)),
                num(c.get('different', 0)), num(c.get('different in a literal only', 0))))
        return '\n'.join(rows)

    def table_note_codes(self):
        rows = ['| Note | Meaning | Count |', '| --- | --- | ---: |']
        for code in sorted(parser.NOTE_CODES):
            rows.append('| `%s` | %s | %s |' % (code, parser.NOTE_CODES[code].replace('|', '\\|'),
                                                num(self.c.get('note.' + code, 0))))
        return '\n'.join(rows)

    def do_table(self, name):
        fn = getattr(self, 'table_' + name, None)
        if fn is None:
            return self.err('table %r' % name)
        return fn()

    # -- driver --------------------------------------------------------------------
    def fill(self, text):
        def sub(m):
            kind, arg = m.group(1), m.group(2).strip()
            fn = getattr(self, 'do_' + kind, None)
            if fn is None:
                return self.err('placeholder kind %r' % kind)
            return fn(arg)
        return re.sub(r'\{\{(\w+)\s+(.*?)\}\}', sub, text)


def main(argv):
    g = Gen()
    with io.open(TEMPLATE, encoding='utf-8') as f:
        text = f.read()
    out = g.fill(text)
    if g.errors:
        for e in g.errors:
            print('unresolved: %s' % e)
        print('%d placeholders could not be filled; %s not written' % (len(g.errors), OUT))
        return 1
    with io.open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(out)
    print('wrote %s (%d lines)' % (OUT, out.count('\n')))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
