"""Co-op fix 19: the crosshair of a joining player (client) never turned green on allies or red on enemies.
CommandInterface.TestTraceActors (StargateGame.u) only colours the crosshair when the aimed pawn has a Controller, and asks the
Controller (SameTeamAs, which needs Level.Game). On a client other pawns have no Controller (not replicated), so nothing happened.
Now (bytecode only, join-safe; single player / host unchanged because their pawns always have a Controller):
  if (Pawn(closestObject) != None && (Pawn(closestObject).Controller != None || Pawn(closestObject).GetTeamNum() != 255))
      ...
      if ((Pawn(closestObject).Controller != None && Pawn(closestObject).Controller.SameTeamAs(ViewportOwner.Actor.Pawn.Controller))
          || (Pawn(closestObject).Controller == None && Pawn(closestObject).GetTeamNum() == ViewportOwner.Actor.Pawn.GetTeamNum()))
(GetTeamNum of a pawn without a Controller comes from its replicated PlayerReplicationInfo.Team; bots and players all have one.)
 v2: the range test  distToHitPosFromPawn <= SGWP.GetMaxRange()  always failed on a client: GetMaxRange is not a simulated function and
     the weapon is only a simulated proxy there, so it returned 0. It now reads the property SGWP.MaxRange (same value).
Needs patch_coop_pkgext.py. Usage: python patch_coop_crosshair.py "<game folder>" [--revert]   (backup <file>.orig_coopcross)
"""
import sys, os, shutil, struct

# ---------------------------------------------------------------- package helpers
def read_ci(d, o):
    b = d[o]; o += 1
    neg = b & 0x80; v = b & 0x3F
    if b & 0x40:
        sh = 6
        while True:
            b = d[o]; o += 1
            v |= (b & 0x7F) << sh; sh += 7
            if not b & 0x80: break
    return (-v if neg else v), o

def write_ci(v):
    neg = v < 0; v = abs(v)
    b = (0x80 if neg else 0) | (v & 0x3F); v >>= 6
    out = bytearray()
    if v:
        b |= 0x40; out.append(b)
        while True:
            c = v & 0x7F; v >>= 7
            if v: c |= 0x80
            out.append(c)
            if not v: break
    else:
        out.append(b)
    return bytes(out)

class Pkg:
    def __init__(self, d):
        self.d = d
        tag, ver, lic, flags, nn, no, ne, eo, ni, io = struct.unpack_from('<IHHIIIIIII', d, 0)
        if tag != 0x9E2A83C1: raise SystemExit('not an Unreal package')
        self.eo = eo
        self.names = []; o = no
        for _ in range(nn):
            l, o = read_ci(d, o); self.names.append(d[o:o + l - 1].decode('latin1')); o += l + 4
        self.imports = []; o = io
        for _ in range(ni):
            cp, o = read_ci(d, o); cn, o = read_ci(d, o); pk = struct.unpack_from('<i', d, o)[0]; o += 4; on, o = read_ci(d, o)
            self.imports.append((self.names[cn], pk, self.names[on]))
        self.ex = []; o = eo
        for _ in range(ne):
            s = o
            cls, o = read_ci(d, o); sup, o = read_ci(d, o); outer = struct.unpack_from('<i', d, o)[0]; o += 4
            nm, o = read_ci(d, o); o += 4; szp = o; sz, o = read_ci(d, o); offp = o; off = 0
            if sz > 0: off, o = read_ci(d, o)
            self.ex.append(dict(cls=cls, outer=outer, name=self.names[nm], size=sz, off=off, szp=szp, offp=offp, end=o))
    def clsname(self, e):
        c = e['cls']
        return self.imports[-c - 1][2] if c < 0 else (self.ex[c - 1]['name'] if c > 0 else 'Class')

# ---------------------------------------------------------------- bytecode walker (validated against 3259 of 3273 functions of StargateGame.u)
class Bad(Exception): pass

def tok(d, p, fix):
    """Walk one expression/statement. Returns (memory size, next position). fix collects (position of a jump-target word)."""
    op = d[p]; p += 1; m = 1
    def ci():
        nonlocal p
        _, p = read_ci(d, p)
    def ex():
        nonlocal p, m
        a, p = tok(d, p, fix); m += a
    def params():
        nonlocal p, m
        while True:
            if d[p] == 0x16: p += 1; m += 1; return
            ex()
    if op in (0, 1, 2, 3, 0x20, 0x29): ci(); m += 4
    elif op == 4: ex()
    elif op == 5: p += 1; m += 1; ex()
    elif op == 6: fix.append(p); p += 2; m += 2
    elif op == 7: fix.append(p); p += 2; m += 2; ex()
    elif op in (8, 0x0b, 0x17, 0x25, 0x26, 0x27, 0x28, 0x2a, 0x30, 0x31, 0x15, 0x16, 0x47): pass
    elif op == 9: p += 3; m += 3; ex()
    elif op == 0x0a:
        w = struct.unpack_from('<H', d, p)[0]; fix.append(p); p += 2; m += 2
        if w != 0xffff: ex()
    elif op == 0x0c: raise Bad('label table')
    elif op in (0x0d, 0x0e, 0x2d, 0x37): ex()
    elif op in (0x0f, 0x10, 0x14, 0x1a, 0x46): ex(); ex()
    elif op == 0x11: ex(); ex(); ex(); ex()
    elif op in (0x12, 0x19): ex(); p += 3; m += 3; ex()
    elif op == 0x13: ci(); m += 4; ex()
    elif op == 0x18: p += 2; m += 2; ex()
    elif op in (0x1b, 0x38): ci(); m += 4; params()
    elif op == 0x1c: ci(); m += 4; params()
    elif op in (0x1d, 0x1e): p += 4; m += 4
    elif op == 0x1f:
        while d[p] != 0: p += 1; m += 1
        p += 1; m += 1
    elif op == 0x21: ci(); m += 4
    elif op in (0x22, 0x23): p += 12; m += 12
    elif op in (0x24, 0x2c): p += 1; m += 1
    elif op == 0x2e: ci(); m += 4; ex()
    elif op == 0x2f: ex(); fix.append(p); p += 2; m += 2
    elif op in (0x32, 0x33): ci(); m += 4; ex(); ex()
    elif op == 0x34:
        while d[p:p + 2] != b'\0\0': p += 2; m += 2
        p += 2; m += 2
    elif op == 0x36: ci(); m += 4; ex()
    elif op == 0x39: p += 1; m += 1; ex()
    elif op in (0x40, 0x41): ex(); ex(); ex()
    elif op >= 0x60:
        if op < 0x70: p += 1; m += 1
        params()
    else: raise Bad('unknown opcode %02x' % op)
    return m, p

def memlen(b):
    p = 0; m = 0
    while p < len(b):
        a, p = tok(b, p, []); m += a
    return m

def locate_script(d, off, size):
    """Returns (position of the ScriptSize int, script start, script size (memory), script end)."""
    blob = d[off:off + size]
    for q in range(4, min(60, len(blob) - 8)):
        sz = struct.unpack_from('<i', blob, q)[0]
        if not 2 < sz < 40000: continue
        p = off + q + 4; m = 0
        try:
            while m < sz:
                a, p = tok(d, p, []); m += a
        except Exception:
            continue
        if m == sz and d[p - 1] == 0x47: return off + q, off + q + 4, sz, p
    raise SystemExit('script not found')


# ---------------------------------------------------------------- the patch
def fit(v, n):
    b = bytearray(write_ci(v))
    if len(b) > n: sys.exit('table entry would change length')
    while len(b) < n:                               # redundant continuation byte: same value, longer encoding
        b[-1] |= (0x40 if len(b) == 1 else 0x80); b.append(0)
    return bytes(b)

def statements(script, fix):
    out = []; p = 0
    while True:
        s = p; a, p = tok(script, p, fix); out.append((s, a))
        if script[s] == 0x47: return out




# ---- token walker that records (byte position, memory position, opcode) of every token and checks skip / context words
def walk(d, p, base, rec, bad):
    op = d[p]; start = p; p += 1; m = 1
    rec.append((start, base, op))
    def ci():
        nonlocal p
        _, p = read_ci(d, p)
    def ex():
        nonlocal p, m
        a, p = walk(d, p, base + m, rec, bad); m += a
        return a
    def params():
        nonlocal p, m
        while True:
            if d[p] == 0x16: p += 1; m += 1; return
            ex()
    if op in (0, 1, 2, 3, 0x20, 0x29): ci(); m += 4
    elif op == 4: ex()
    elif op == 5: p += 1; m += 1; ex()
    elif op == 6: p += 2; m += 2
    elif op == 7: p += 2; m += 2; ex()
    elif op in (8, 0x0b, 0x17, 0x25, 0x26, 0x27, 0x28, 0x2a, 0x30, 0x31, 0x15, 0x16, 0x47): pass
    elif op == 9: p += 3; m += 3; ex()
    elif op == 0x0a:
        w = struct.unpack_from('<H', d, p)[0]; p += 2; m += 2
        if w != 0xffff: ex()
    elif op in (0x0d, 0x0e, 0x2d, 0x37): ex()
    elif op in (0x0f, 0x10, 0x14, 0x1a, 0x46): ex(); ex()
    elif op == 0x11: ex(); ex(); ex(); ex()
    elif op in (0x12, 0x19):
        ex(); w = struct.unpack_from('<H', d, p)[0]; p += 3; m += 3; a = ex()
        if w != a: bad.append(('ctx', start, w, a))
    elif op == 0x13: ci(); m += 4; ex()
    elif op == 0x18:
        w = struct.unpack_from('<H', d, p)[0]; p += 2; m += 2; a = ex()
        if w != a + 1: bad.append(('skip', start, w, a))
    elif op in (0x1b, 0x38): ci(); m += 4; params()
    elif op == 0x1c: ci(); m += 4; params()
    elif op in (0x1d, 0x1e): p += 4; m += 4
    elif op == 0x1f:
        while d[p] != 0: p += 1; m += 1
        p += 1; m += 1
    elif op == 0x21: ci(); m += 4
    elif op in (0x22, 0x23): p += 12; m += 12
    elif op in (0x24, 0x2c): p += 1; m += 1
    elif op == 0x2e: ci(); m += 4; ex()
    elif op == 0x2f: ex(); p += 2; m += 2
    elif op in (0x32, 0x33): ci(); m += 4; ex(); ex()
    elif op == 0x36: ci(); m += 4; ex()
    elif op == 0x39: p += 1; m += 1; ex()
    elif op in (0x40, 0x41): ex(); ex(); ex()
    elif op >= 0x60:
        if op < 0x70: p += 1; m += 1
        params()
    else: raise Bad('unknown opcode %02x' % op)
    return m, p




# ---------------------------------------------------------------- the patch
I = lambda r: b'\x01' + write_ci(r)
H = lambda v: struct.pack('<H', v)

def AND(a, b): return b'\x82' + a + b'\x18' + H(memlen(b) + 1) + b + b'\x16'

class Asm:
    """Tiny assembler: items are bytes or ('j', opcode, label, cond-bytes) or ('L', label). Jump words are absolute memory positions."""
    def __init__(self, items): self.items = items
    def size(self):
        m = 0
        for it in self.items:
            if isinstance(it, bytes): m += memlen(it)
            elif it[0] == 'j': m += 3 + (memlen(it[3]) if it[3] else 0)
            elif it[0] == 'it': m += 3 + memlen(it[1])
        return m
    def build(self, base, ext):
        lab = dict(ext); m = base
        for it in self.items:
            if isinstance(it, bytes): m += memlen(it)
            elif it[0] == 'j': m += 3 + (memlen(it[3]) if it[3] else 0)
            elif it[0] == 'it': m += 3 + memlen(it[1])
            else: lab[it[1]] = m
        out = b''
        for it in self.items:
            if isinstance(it, bytes): out += it
            elif it[0] == 'j': out += bytes([it[1]]) + H(lab[it[2]]) + (it[3] or b'')
            elif it[0] == 'it': out += b'\x2f' + it[1] + H(lab[it[2]])
        assert memlen(out) == self.size()
        return out

def rebuild(script, prepend=None, repl={}, extlab={}):
    """prepend: Asm placed at memory 0 (label 'next' = new position of the old first statement).
    repl: {old statement byte position: Asm}; its label 'next' = new position of the following old statement.
    Every jump word of the kept statements is moved to the new statement positions."""
    fix = []; stm = statements(script, fix)
    old_m = []; m = 0
    for s, a in stm: old_m.append(m); m += a
    old_end = m
    pre = prepend.size() if prepend else 0
    new_m = []; m = pre
    for (s, a) in stm:
        new_m.append(m); m += repl[s].size() if s in repl else a
    mp = dict(zip(old_m, new_m)); mp[old_end] = m
    out = b''
    bpos = {st_: new_m[i] for i, (st_, a_) in enumerate(stm)}
    xl = {k: bpos[v] for k, v in extlab.items()}
    if prepend: out += prepend.build(0, dict(xl, next=new_m[0]))
    for i, (s, a) in enumerate(stm):
        e = stm[i + 1][0] if i + 1 < len(stm) else len(script)
        nxt = new_m[i + 1] if i + 1 < len(stm) else m
        if s in repl:
            out += repl[s].build(new_m[i], dict(xl, next=nxt)); continue
        seg = bytearray(script[s:e])
        for fp in fix:
            if s <= fp < e:
                t = struct.unpack_from('<H', seg, fp - s)[0]
                if t == 0xffff: continue
                if t not in mp: sys.exit('jump target %d is not a statement start' % t)
                struct.pack_into('<H', seg, fp - s, mp[t])
        out += bytes(seg)
    assert memlen(out) == m, (memlen(out), m)
    return out

def relocate(d, P, fn, newscr):
    e = P.ex[fn]
    szp, st0, ssz, en = locate_script(bytes(d), e['off'], e['size'])
    blob = bytes(d[e['off']:e['off'] + e['size']])
    q = szp - e['off']; s0_ = st0 - e['off']; e0_ = en - e['off']
    nblob = blob[:q] + struct.pack('<i', memlen(newscr)) + blob[q + 4:s0_] + newscr + blob[e0_:]
    nszsz = e['offp'] - e['szp']; noffsz = e['end'] - e['offp']
    if len(nblob) > (1 << (6 + 7 * (nszsz - 1))) - 1: sys.exit('size field too small (run patch_coop_pkgext.py first)')
    cap = (1 << (6 + 7 * (noffsz - 1))) - 1
    cur = None
    for x in P.ex:
        if x['name'] == 'ScriptText' and x['size'] > 200:
            live = max([y['off'] + y['size'] for y in P.ex if y['size'] and x['off'] <= y['off'] < x['off'] + x['size'] and y is not x] + [x['off'] + 64])
            a = (live + 15) & ~15
            if a + len(nblob) <= x['off'] + x['size'] and a + len(nblob) <= cap: cur = a; break
    if cur is None: sys.exit('no free space')
    d[cur:cur + len(nblob)] = nblob
    d[e['szp']:e['offp']] = fit(len(nblob), nszsz); d[e['offp']:e['end']] = fit(cur, noffsz)
    P2 = Pkg(bytes(d)); e2 = P2.ex[fn]
    a_, b_, c_, e_ = locate_script(bytes(d), e2['off'], e2['size'])
    scr = bytes(d[b_:e_]); rec = []; bad = []; pp = 0; mp = 0
    while pp < len(scr):
        aa, pp = walk(scr, pp, mp, rec, bad); mp += aa
    if bad or mp != c_ or scr != newscr: sys.exit('verification failed: %r' % (bad[:2],))
    return P2

def get_script(d, P, fn):
    e = P.ex[fn]; szp, st0, ssz, en = locate_script(bytes(d), e['off'], e['size'])
    return bytes(d[st0:en])

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args: sys.exit(__doc__)
    f = os.path.join(args[0], 'System', 'StargateGame.u'); bak = f + '.orig_coopcross'
    if '--revert' in sys.argv:
        if not os.path.exists(bak): sys.exit('no backup found, nothing to revert')
        shutil.copyfile(bak, f); print('reverted'); return
    d = bytearray(open(f, 'rb').read()); P = Pkg(bytes(d))
    c = [i + 1 for i, e in enumerate(P.ex) if e['name'] == 'CommandInterface' and e['outer'] == 0 and P.clsname(e) == 'Class'][0]
    fn = [i for i, e in enumerate(P.ex) if e['name'] == 'TestTraceActors' and e['outer'] == c and e['size']][0]
    sc = get_script(d, P, fn)
    nm = P.names.index('GetTeamNum')
    pawncls = [-(i + 1) for i, im in enumerate(P.imports) if im[0] == 'Class' and im[2] == 'Pawn']
    ctrl = [-(i + 1) for i, im in enumerate(P.imports) if im[0] == 'ObjectProperty' and im[2] == 'Controller' and P.imports[-im[1] - 1][2] == 'Pawn']
    if len(pawncls) != 1 or len(ctrl) != 1: sys.exit('imports not found')
    cl = [i + 1 for i, x in enumerate(P.ex) if x['name'] == 'closestObject' and x['outer'] == fn + 1]
    if len(cl) != 1: sys.exit('local closestObject not found')
    def ctx(e1, e2, size): return b'\x19' + e1 + H(memlen(e2)) + bytes([size]) + e2
    def OR(a, b): return b'\x84' + a + b'\x18' + H(memlen(b) + 1) + b + b'\x16'
    PC = b'\x2e' + write_ci(pawncls[0]) + b'\x00' + write_ci(cl[0])           # Pawn(closestObject)
    CT = ctx(PC, b'\x01' + write_ci(ctrl[0]), 4)                               # Pawn(closestObject).Controller
    TN = lambda obj: ctx(obj, b'\x1b' + write_ci(nm) + b'\x16', 4)           # obj.GetTeamNum()
    oldB = b'\x77' + CT + b'\x2a\x16'                                         # ...Controller != None
    sameteam_call = b'\x1b' + write_ci(P.names.index('SameTeamAs'))
    # v2: SGWP.GetMaxRange() -> SGWP.MaxRange (works on a client)
    mr = [-(i + 1) for i, im in enumerate(P.imports) if im[0] == 'FloatProperty' and im[2] == 'MaxRange' and P.imports[-im[1] - 1][2] == 'Weapon']
    if len(mr) != 1: sys.exit('import Weapon.MaxRange not found')
    gmr = b'\x1b' + write_ci(P.names.index('GetMaxRange')) + b'\x16'
    def fix_range(sc):
        stm = statements(sc, []); ends = [stm[i + 1][0] if i + 1 < len(stm) else len(sc) for i in range(len(stm))]
        mpos = {}; m = 0
        for (s_, a_) in stm: mpos[m] = s_; m += a_
        repl = {}; ext = {}; n = 0
        for i, (s_, a_) in enumerate(stm):
            seg = sc[s_:ends[i]]
            if gmr not in seg: continue
            if seg[0] != 0x07: sys.exit('GetMaxRange outside an if')
            cond = seg[3:]; out = b''; p = 0
            while True:
                q = cond.find(gmr, p)
                if q < 0: out += cond[p:]; break
                # cond[..q] ends with  19 <obj> <skip word> <size>  -> rewrite the skip word for the shorter member
                if cond[q - 3:q] != H(memlen(gmr)) + b'\x04': sys.exit('unexpected GetMaxRange context')
                out += cond[p:q - 3] + H(memlen(b'\x01' + write_ci(mr[0]))) + b'\x04' + b'\x01' + write_ci(mr[0]); p = q + len(gmr)
            tgt = struct.unpack_from('<H', sc, s_ + 1)[0]; lab = 'r%d' % n; n += 1
            ext[lab] = mpos[tgt]; repl[s_] = Asm([('j', 0x07, lab, out)])
        return rebuild(sc, None, repl, ext) if repl else sc
    if TN(PC) in sc:
        if gmr not in sc: print('already patched'); return
        newscr = fix_range(sc)
        rec = []; bad = []; pp = 0; mp_ = 0
        while pp < len(newscr):
            aa, pp = walk(newscr, pp, mp_, rec, bad); mp_ += aa
        if bad: sys.exit('bad body %r' % (bad[:2],))
        if not os.path.exists(bak): shutil.copyfile(f, bak)
        relocate(d, P, fn, newscr); open(f, 'wb').write(bytes(d))
        print('patched (v2 range fix): client crosshair colours ->', f); return
    stm = statements(sc, []); ends = [stm[i + 1][0] if i + 1 < len(stm) else len(sc) for i in range(len(stm))]
    s1 = [i for i in range(len(stm)) if sc[stm[i][0]] == 0x07 and sc[stm[i][0] + 3] == 0x82 and oldB in sc[stm[i][0]:ends[i]]]
    s2 = [i for i in range(len(stm)) if sc[stm[i][0]] == 0x07 and sc[stm[i][0] + 3:stm[i][0] + 4 + len(CT)] == b'\x19' + CT and sameteam_call in sc[stm[i][0]:ends[i]]]
    if len(s1) != 1 or len(s2) != 1: sys.exit('statements not found (%d, %d)' % (len(s1), len(s2)))
    # memory position -> statement byte position (for the two if targets)
    mpos = {}; m = 0
    for (s, a) in stm: mpos[m] = s; m += a
    mpos[m] = len(sc)
    repl = {}; ext = {}
    for k, (i, mk) in enumerate(((s1[0], 'a'), (s2[0], 'b'))):
        s = stm[i][0]; e = ends[i]; tgt = struct.unpack_from('<H', sc, s + 1)[0]; cond = sc[s + 3:e]
        if tgt not in mpos: sys.exit('if target is not a statement')
        if mk == 'a':
            Aop = cond[1:cond.index(b'\x18')]
            if cond != b'\x82' + Aop + b'\x18' + H(memlen(oldB) + 1) + oldB + b'\x16': sys.exit('unexpected first condition layout')
            ncond = AND(Aop, OR(oldB, b'\x9b' + TN(PC) + b'\x1d' + struct.pack('<i', 255) + b'\x16'))
        else:
            vp = cond[len(b'\x19' + CT) + 3 + len(sameteam_call):-1]          # call argument: ViewportOwner.Actor.Pawn.Controller
            if not vp.startswith(b'\x19') or not cond.endswith(b'\x16'): sys.exit('unexpected SameTeamAs layout')
            vpawn = vp[1:]; vpawn = vpawn[:len(vpawn) - (len(b'\x05\x00\x04') + len(b'\x01' + write_ci(ctrl[0])))]   # strip ".Controller"
            if vp != ctx(vpawn, b'\x01' + write_ci(ctrl[0]), 4): sys.exit('unexpected SameTeamAs argument')
            A = b'\x82' + oldB + b'\x18' + H(memlen(cond) + 1) + cond + b'\x16'
            B2 = b'\x9a' + TN(PC) + TN(vpawn) + b'\x16'
            Bc = b'\x82' + b'\x72' + CT + b'\x2a\x16' + b'\x18' + H(memlen(B2) + 1) + B2 + b'\x16'
            ncond = OR(A, Bc)
        lab = 't' + mk; ext[lab] = mpos[tgt]
        repl[s] = Asm([('j', 0x07, lab, ncond)])
    newscr = fix_range(rebuild(sc, None, repl, ext))
    rec = []; bad = []; pp = 0; mp_ = 0
    while pp < len(newscr):
        aa, pp = walk(newscr, pp, mp_, rec, bad); mp_ += aa
    if bad: sys.exit('bad body %r' % (bad[:2],))
    if not os.path.exists(bak): shutil.copyfile(f, bak)
    relocate(d, P, fn, newscr)
    open(f, 'wb').write(bytes(d))
    print('patched: client crosshair turns green on allies / red on enemies (CommandInterface.TestTraceActors) ->', f)

main()
