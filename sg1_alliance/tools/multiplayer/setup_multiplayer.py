#!/usr/bin/env python3
"""Co-op campaign / multiplayer script fixes for YOUR OWN copy of the game files (System\\*.u, *.ini).

    python setup_multiplayer.py "C:\\SG-1 The_Alliance"            apply
    python setup_multiplayer.py "C:\\SG-1 The_Alliance" --no-extras  skip: joiner character choice, take over the AI squadmate, objectives tab rebuild
    python setup_multiplayer.py "C:\\SG-1 The_Alliance" --revert   restore every file from the backups

Every change is either gated to network games (Role / NetMode checks in the script), only touches multiplayer menus and
server-list code, or sets the "simulated" flag on animation functions (no effect in single player). Backups: <file>.orig_mp.
The "extras" (joiner character choice, squadmate take-over, objectives tab rebuild) are applied by default; they also run in single
player (Login class choice, info screen) but leave it unchanged there. --no-extras leaves them out.

The order matters (each step edits the result of the previous one); setup_sg1.py runs this after sg1patch.py / patch_health_bar.py.
The native side of the same work (look up/down of other players) is game_hooks/recomp_0027_remote_player_look.patch (applied by apply_hooks.py).
"""
import os, sys, shutil, subprocess

here = os.path.dirname(os.path.abspath(__file__))
args = [a for a in sys.argv[1:] if not a.startswith('--')]
if not args: sys.exit(__doc__)
game = args[0].strip('"')
sysdir = os.path.join(game, 'System')
if not os.path.isdir(sysdir): sys.exit('no System folder in ' + game)
FILES = [os.path.join('System', n) for n in ('StargateGame.u', 'Engine.u', 'StargateMenus.u')] + [
    os.path.join('System', 'tweak', 'MP', 'GameModes.ini'), os.path.join('System', 'UnrealEngine2.ini')]
EXTRA_BAKS = ['.orig_coopstart', '.orig_coopchars', '.orig_coopnpc', '.orig_cooprestart', '.orig_cooprestart2', '.orig_cooprevive', '.orig_coopclientfx',
              '.orig_cooplistinfo', '.orig_coopclientfx2', '.orig_coopobj', '.orig_coopanimfix', '.orig_coopfx3', '.orig_coopext',
              '.orig_coopsim', '.orig_coopmelee', '.orig_coopreload', '.orig_coopreviveFX', '.orig_coopdownpose', '.orig_coopselfrevive', '.orig_coopbleedout', '.orig_coopdownfix', '.orig_coopjoincut', '.orig_cooptakeover2', '.orig_coopnopause', '.orig_coopcross', '.orig_coopclientdown', '.orig_coopdownself', '.orig_coopclienthud', '.orig_coopsceneend', '.orig_coopsceneend2', '.orig_coopfx5', '.orig_coopmode', '.orig_coop']

def paths():
    for f in FILES:
        p = os.path.join(game, f)
        if os.path.exists(p): yield p

if '--revert' in sys.argv:
    for p in paths():
        b = p + '.orig_mp'
        if os.path.exists(b):
            shutil.copyfile(b, p); print('restored', os.path.relpath(p, game))
            os.remove(b)
            for s in EXTRA_BAKS:
                if os.path.exists(p + s): os.remove(p + s)
    sys.exit(0)

if any(os.path.exists(p + '.orig_mp') for p in paths()):
    print('multiplayer fixes: already applied (backups *.orig_mp exist). Run with --revert first to apply again.'); sys.exit(0)

STEPS = [('patch_coop_start.py', []),                                   # campaign starts at once in a network game (standalone unchanged)
         ('patch_coop_chars.py', ['x']),                                # extras: joiner gets the character that was chosen
         ('patch_coop_takeover.py', ['x']),                             # extras: joiner replaces the AI squadmate
         ('patch_coop_mode.py', []),                                    # "StoryModeGame" in the host game-mode list
         ('patch_coop_restart.py', []),                                 # restart level keeps the listen server
         ('patch_coop_restart2.py', []),                                # host restart = Level.ServerTravel("?restart") (clients follow)
         ('patch_coop_revive.py', []),                                  # network only: downed instead of dead, revive, mission failed when nobody revives
         ('patch_coop_clientfx.py', []),                                # clients: downed pawn drawn lying, revive bar cleared
         ('patch_coop_listinfo.py', []),                                # server list shows the real player count
         ('patch_coop_clientfx2.py', []),
         ('patch_coop_objectives.py', ['x']),                           # extras: host objectives tab
         ('patch_coop_animfix.py', []),                                 # remote pawns use the right weapon pose / crouch
         ('patch_coop_clientfx3.py', []),                               # revived pawn plays idle again, bar hidden
         ('patch_coop_pkgext.py', []),                                  # widen export size/offset fields (needed before the next steps grow PlayerTick)
         ('patch_coop_simfuncs.py', []),                                # remote pawns run leg animation functions (crouch / walk)
         ('patch_coop_melee.py', []),                                   # host's melee (right click) animation visible to clients
         ('patch_coop_reload.py', []),                                  # reload animations of other players visible (host <-> client)
         ('patch_coop_revivefx.py', []),                                # clients: revive bar text, revived player gets up (not lying)
         ('patch_coop_selfrevive.py', []),                              # a downed player cannot revive himself
         ('patch_coop_bleedout.py', []),                                # bleed-out: player dies like the NPCs, then mission fails
         ('patch_coop_downfix.py', []),                                 # bleed-out timer fix (old revive tools) + same lie-down anim on all screens
         ('patch_coop_takeover2.py', ['x']),                            # extras: joiner gets the squadmate's pawn (spectates during cutscenes)
         ('patch_coop_clientfx5.py', ['--marker']),                     # look up/down of other players (with the native hook)
         ('patch_coop_downpose.py', []),                                # clients: downed player lies flat (no look bend)
         ('patch_coop_nopause.py', []),                                 # Tab screen does not pause a co-op game (host saw an empty panel)
         ('patch_coop_crosshair.py', []),                               # clients: crosshair green on allies / red on enemies
         ('patch_coop_clientdown.py', []),                              # a downed client cannot walk or shoot
         ('patch_coop_downself.py', []),                                # the downed client lies flat on his own screen too
         ('patch_coop_clienthud.py', []),                               # client HUD bar shows his health (shield values are stale there)
         ('patch_coop_sceneend.py', []),                                # Engine.u: client keeps his character after a cutscene
         ('patch_coop_sceneend2.py', []),                               # client HUD comes back after a cutscene
         ('patch_coop_browser.py', [])]                                 # campaign servers visible in the System Link list
extras = '--no-extras' not in sys.argv   # default: all fixes (--extras is still accepted)

for p in paths():
    shutil.copyfile(p, p + '.orig_mp')
for name, a in STEPS:
    is_extra = (a == ['x'])
    if is_extra and not extras: continue
    cmd = [sys.executable, os.path.join(here, name), game] + ([] if is_extra else a)
    print('\n=== %s' % name, flush=True)
    r = subprocess.run(cmd)
    if r.returncode != 0:
        print('step failed: %s (exit %d). Reverting all multiplayer changes.' % (name, r.returncode))
        subprocess.run([sys.executable, os.path.abspath(__file__), game, '--revert'])
        sys.exit(r.returncode)

# LAN replication rate: remote players are updated at the server's net tick rate (35 Hz by default). 60 makes their look/aim smoother.
ini = os.path.join(game, 'System', 'UnrealEngine2.ini')
if os.path.exists(ini):
    raw = open(ini, 'rb').read(); nl = b'\r\n' if b'\r\n' in raw else b'\n'
    lines = raw.split(nl); sec = b''; n = 0
    for i, l in enumerate(lines):
        t = l.strip()
        if t.startswith(b'['): sec = t
        elif t == b'LanServerMaxTickRate=35' and sec in (b'[IpDrv.TcpNetDriver]', b'[IpDrv.TcpNetDriverXBox]'):
            lines[i] = b'LanServerMaxTickRate=60'; n += 1
    # network rates / timeout (network games only): smoother remote players, and a joiner is not dropped while a big map loads
    ch = {b'ConnectionTimeout=15.0': b'ConnectionTimeout=60.0', b'MaxClientRate=20000': b'MaxClientRate=100000',
          b'NetServerMaxTickRate=20': b'NetServerMaxTickRate=60'}
    for i, l in enumerate(lines):
        t = l.strip()
        if t.startswith(b'['): sec = t
        elif t in ch and sec in (b'[IpDrv.TcpNetDriver]', b'[IpDrv.TcpNetDriverXBox]'):
            lines[i] = l.replace(t, ch[t]); n += 1
    if n: open(ini, 'wb').write(nl.join(lines)); print('\nUnrealEngine2.ini net settings: %d lines (LanServerMaxTickRate 60, NetServerMaxTickRate 60, MaxClientRate 100000, ConnectionTimeout 60)' % n)
print('\nMultiplayer fixes applied%s. Undo: python %s "%s" --revert' % (' (with extras)' if extras else '', os.path.basename(__file__), game))
