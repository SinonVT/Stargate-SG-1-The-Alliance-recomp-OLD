#!/usr/bin/env python3
"""Higher quality settings the Xbox build had turned down (System\\UnrealEngine2.ini, section [XboxDrv.XboxClient], the one the game uses):
    TextureDetailWorld / Terrain / PlayerSkin / WeaponSkin / Interface / RenderMap   Lower -> High   (the Xbox skipped the top mip level)
    WeatherEffects   False -> True                                                    (rain / snow emitters were switched off)
    NoFractalAnim    True -> False                                                    (animated fractal textures: water, energy, fire)
and in [D3DDrv.D3DRenderDevice]:
    HighDetailActors / SuperHighDetailActors   false -> True   (actors marked high detail were never drawn - the rain is one of them)
Only the Xbox default values are changed (Lower / False / false): a value the player set himself is left alone.
TextureDetailLightmap stays UltraHigh. Backup System\\UnrealEngine2.ini.orig_gfx. Re-running is harmless.
Note: the HD texture pack matches textures as the game uploads them; with full-size textures some HD replacements may no longer match.

Usage:  python patch_gfx_ini.py "<game folder>"            apply
        python patch_gfx_ini.py "<game folder>" --revert   restore the backup
"""
import sys, os, shutil
args = [a for a in sys.argv[1:] if not a.startswith('--')]
if not args: sys.exit(__doc__)
p = os.path.join(args[0].strip('"'), 'System', 'UnrealEngine2.ini'); bak = p + '.orig_gfx'
if '--revert' in sys.argv:
    if not os.path.exists(bak): sys.exit('no backup found, nothing to revert')
    shutil.copyfile(bak, p); print('reverted'); sys.exit(0)
if not os.path.isfile(p): sys.exit('not found: ' + p)
raw = open(p, 'rb').read(); nl = b'\r\n' if b'\r\n' in raw else b'\n'
lines = raw.split(nl); sec = b''; n = 0
WANT = {b'TextureDetailInterface': b'High', b'TextureDetailTerrain': b'High', b'TextureDetailWeaponSkin': b'High',
        b'TextureDetailPlayerSkin': b'High', b'TextureDetailWorld': b'High', b'TextureDetailRenderMap': b'High', b'WeatherEffects': b'True'}
for i, l in enumerate(lines):
    t = l.strip()
    if t.startswith(b'['): sec = t.lower(); continue
    if sec == b'[xboxdrv.xboxclient]' and b'=' in t:
        k, v = t.split(b'=', 1)
        if k in WANT and v.lower() in (b'lower', b'false'):          # Xbox default only
            lines[i] = k + b'=' + WANT[k]; n += 1
        if k == b'NoFractalAnim' and v.lower() == b'true':          # Xbox default only
            lines[i] = b'NoFractalAnim=False'; n += 1
    if sec == b'[d3ddrv.d3drenderdevice]' and b'=' in t:
        k, v = t.split(b'=', 1)
        if k in (b'HighDetailActors', b'SuperHighDetailActors') and v.lower() == b'false':
            lines[i] = k + b'=True'; n += 1
if n:
    if not os.path.exists(bak): shutil.copyfile(p, bak)
    open(p, 'wb').write(nl.join(lines))
print('graphics settings: %d changed (textures High, weather on, high detail actors on, fractal animation on)' % n if n else 'graphics settings: already applied')
