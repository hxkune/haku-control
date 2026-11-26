# Lists ui/i18n.js keys nothing uses: python tools/i18n-unused.py   (exit 1 when there are some)
# A key counts as used when its exact text is a literal somewhere in the UI, the core or pro\ (quoted, or in data-i18n),
# or when a literal that code goes on to join with more ('orgb.st' + n, `fix.${k}`) starts it. Tests and the mock are
# not counted: a test that only checks a key exists does not make it shown.
import pathlib, re, sys

root = pathlib.Path(__file__).resolve().parent.parent
src = (root / 'ui' / 'i18n.js').read_text(encoding='utf-8')
start = src.index('const STR = {')
blocks = re.split(r"\n  (en|ru|fr): \{", src[start:])
langs = {blocks[i]: set(re.findall(r"(?:^\s*|, )'([A-Za-z0-9_.-]+)':", blocks[i + 1], re.M)) for i in range(1, len(blocks), 2)}

files = [p for p in (root / 'ui').glob('*') if p.suffix in ('.js', '.html') and p.name != 'i18n.js' and not p.name.startswith('mock')]
files += [p for d in ('src', 'pro') for p in (root / d).rglob('*') if p.suffix in ('.c', '.cpp', '.h') and p.is_file()]
text = '\n'.join(p.read_text(encoding='utf-8', errors='replace') for p in files)
# the i18n.js code after the tables (t(), the data-i18n pass) may name keys too
text += src[src.index('\n};', start):]

lits = set(re.findall(r"""['"`]([A-Za-z0-9_.-]+)['"`]""", text)) | set(re.findall(r'[a-z0-9_]+(?:\.[A-Za-z0-9_-]+)+', text))   # also inside a longer string ('2 c 0 0 cmp.k.beat')
lits |= set(re.findall(r'data-i18n(?:-[a-z]+)?="([^"$]+)"', text))
# a literal followed by + or a template's ${: what comes after it is made at run time
prefixes = set(re.findall(r"""['"]([a-z0-9_.-]+)['"]\s*\+""", text)) | set(re.findall(r"`[^`]*?([a-z0-9_]+(?:\.[a-z0-9_-]*)*)\$\{", text))
prefixes = {p for p in prefixes if '.' in p or len(p) >= 3}

# and a literal added after (FX_NEEDS[id] + '.t'): a key that is a known one plus it
suffixes = set(re.findall(r"""\+\s*['"](\.[A-Za-z0-9_.-]+)['"]""", text))

def used(k):
    return k in lits or any(k.startswith(p) for p in prefixes) or any(k.endswith(x) and k[:-len(x)] in lits for x in suffixes)

every = set().union(*langs.values())
dead = sorted(k for k in every if not used(k))
for k in dead:
    print(k, '' if all(k in l for l in langs.values()) else '(only in ' + ', '.join(n for n, l in langs.items() if k in l) + ')')
miss = {n: sorted(langs['en'] - l) for n, l in langs.items() if n != 'en' and langs['en'] - l}
for n, ks in miss.items():
    print(f'missing in {n}:', ' '.join(ks))
print(f'{len(dead)} of {len(every)} keys unused', file=sys.stderr)
sys.exit(1 if dead else 0)
