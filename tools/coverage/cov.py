# Line coverage of the TESTING kernel, from a QEMU `-d in_asm` trace.
#
# Usage (from the repo root, with build/exodoom built at -O2 -g):
#   python3 -I tools/coverage/cov.py <dir containing asm.log>
#
# asm.log is the `-D` output of a test boot run with `-d in_asm`: every
# translated code block is logged, so an instruction address present in it
# is treated as executed. Each instruction in build/exodoom's disassembly is
# mapped to file:line with addr2line; a line is covered if any of its
# instructions ran. Line coverage only, kernel binary only (ring-3 images
# are linked separately and not counted).
import re, subprocess, sys, collections
S = sys.argv[1]
ex = set()
pat = re.compile(r'^0x([0-9a-f]+):\s+\S')
for l in open(S + '/asm.log', errors='replace'):
    m = pat.match(l)
    if m: ex.add(int(m.group(1), 16))
dis = subprocess.run(['objdump', '-d', '-w', 'build/exodoom'], capture_output=True, text=True).stdout
ins = [int(m.group(1), 16) for m in re.finditer(r'^\s*([0-9a-f]+):\t', dis, re.M)]
a2l = subprocess.run(['addr2line', '-e', 'build/exodoom'], input='\n'.join(hex(a) for a in ins), capture_output=True, text=True).stdout.split('\n')
lines = collections.defaultdict(lambda: [set(), set()])  # file -> [all, hit]
for a, loc in zip(ins, a2l):
    f, _, n = loc.rpartition(':')
    n = n.split(' ')[0]
    if not n.isdigit() or int(n) == 0: continue
    f = f.replace('/work/', '')
    lines[f][0].add(int(n))
    if a in ex: lines[f][1].add(int(n))
def group(f):
    if f.startswith('src/doom/'): return 'vendored doom'
    if f.startswith('src/nuked_opl3/'): return 'vendored nuked_opl3'
    if f.startswith('src/'): return 'ours'
    return 'other'
tot = collections.defaultdict(lambda: [0, 0])
for f, (a, h) in lines.items():
    g = group(f); tot[g][0] += len(a); tot[g][1] += len(h)
for g, (a, h) in tot.items(): print(f'{g:22s} {h:6d}/{a:6d} = {100*h/a:5.1f}%')
o = tot['ours']; print(f'\nfirst-party total: {100*o[1]/o[0]:.1f}%')
print('\nlowest first-party files (>=20 lines):')
rows = sorted(((len(h)/len(a), f, len(h), len(a)) for f, (a, h) in lines.items() if group(f) == 'ours' and len(a) >= 20))
for r, f, h, a in rows[:15]: print(f'  {100*r:5.1f}%  {h:4d}/{a:4d}  {f}')
