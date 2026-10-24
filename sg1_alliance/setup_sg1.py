#!/usr/bin/env python3
"""One-step setup for the SG-1 recomp: generates the recompiled code from YOUR default.xbe,
applies our hooks, and (optionally) builds.

    python sg1_alliance\\setup_sg1.py "C:\\path\\to\\game folder"          (folder that contains default.xbe)
    python sg1_alliance\\setup_sg1.py "C:\\path\\to\\game folder" --build   (also run cmake + build)
    add --redo to regenerate even if sg1_alliance\\src\\recomp\\gen already exists
    add --no-multiplayer to skip the co-op/multiplayer script fixes, --mp-no-extras to leave out the joiner character / squadmate take-over / objectives extras

Run it from anywhere; needs Python 3.9+ (pip installs 'capstone' for you). The pipeline takes several minutes.
"""
import os, sys, subprocess, shutil
here = os.path.dirname(os.path.abspath(__file__))          # .../sg1_alliance
root = os.path.dirname(here)                                # repo root (the toolkit)
args = [a for a in sys.argv[1:] if not a.startswith('--')]
flags = [a for a in sys.argv[1:] if a.startswith('--')]
if not args:
    print(__doc__); sys.exit(1)
game = args[0].strip('"')
xbe = game if game.lower().endswith('.xbe') else os.path.join(game, 'default.xbe')
if not os.path.isfile(xbe):
    print('ERROR: default.xbe not found at', xbe); sys.exit(1)
gen = os.path.join(here, 'src', 'recomp', 'gen')
py = sys.executable

def run(title, cmd, cwd=root):
    print('\n=== %s\n    %s' % (title, ' '.join(cmd)), flush=True)
    r = subprocess.run(cmd, cwd=cwd)
    if r.returncode != 0:
        print('\nERROR: step failed (%s). Fix the message above and run this script again.' % title); sys.exit(r.returncode)

try:
    import capstone  # noqa
except ImportError:
    run('Installing capstone (disassembler library)', [py, '-m', 'pip', 'install', 'capstone'])

done = os.path.exists(os.path.join(gen, 'recomp_dispatch.c')) and '--redo' not in flags
if done:
    print('Generated code already exists in', gen, '(use --redo to regenerate).')
else:
    os.makedirs(gen, exist_ok=True)
    os.makedirs(os.path.join(root, 'tools', 'xbe_parser', 'output'), exist_ok=True)
    aj = os.path.join(root, 'tools', 'xbe_parser', 'output', 'default_analysis.json')
    run('1/5 Parse the XBE', [py, '-m', 'tools.xbe_parser', xbe, '--json', aj])
    run('2/5 Disassemble (slow: 30-60+ minutes, one CPU core)', [py, '-m', 'tools.disasm', xbe, '--analysis-json', aj])
    run('3/5 Identify library functions', [py, '-m', 'tools.func_id', xbe, '-v'])
    run('4/5 ABI analysis', [py, '-m', 'tools.abi_analysis', xbe, '-v'])
    run('5/5 Recompile to C (several minutes)', [py, '-m', 'tools.recomp', xbe, '--all', '--split', '250', '--gen-dir', gen])

run('Applying our hooks to the generated code', [py, os.path.join(here, 'game_hooks', 'apply_hooks.py')], cwd=here)
try:   # the dump lacks the Tutorial / M01A maps; New Game freezes without stand-ins
    run('Creating stand-in tutorial maps in the game folder', [py, os.path.join(here, 'tools', 'make_standin_maps.py'), os.path.dirname(xbe)], cwd=here)
except SystemExit:
    print('(stand-in maps not created; run tools\\make_standin_maps.py by hand)')
try:   # key layout of this port (B = third person, Ctrl = duck, ...): the dump's own layout is the PC prototype one
    run('Applying the keyboard layout (User.ini)', [py, os.path.join(here, 'tools', 'controls_preset.py'), os.path.dirname(xbe)], cwd=here)
except SystemExit:
    print('(keyboard layout not applied; run tools\\controls_preset.py by hand)')
try:   # plain stargates ignore their second trigger in the retail scripts, so the menu gate never closes
    run('Fixing the stargate that never closes (StargateContent.u)', [py, os.path.join(here, 'tools', 'patch_gate_close.py'), os.path.join(os.path.dirname(xbe), 'System', 'StargateContent.u')], cwd=here)
except SystemExit:
    print('(gate fix not applied; run tools\\patch_gate_close.py by hand)')
try:   # script fixes: non-team chat, Last Man Standing menu entry, and the missing Replicator.ini (each keeps a .orig_sg1 backup; run sg1patch.py --revert to undo)
    run('Applying the game script fixes (teamsay, Last Man Standing, Replicator.ini)', [py, os.path.join(here, 'tools', 'patches', 'sg1patch.py'), os.path.dirname(xbe)], cwd=here)
except SystemExit:
    print('(some script fixes were not applied; run tools\\patches\\sg1patch.py by hand, it says which)')
try:   # the health/shield bar never moves in story mode in the retail script (same on the Xbox); keeps a .orig_hb backup, patch_health_bar.py --revert undoes it
    run('Making the health bar work in story mode (StargateGame.u)', [py, os.path.join(here, 'tools', 'patch_health_bar.py'), os.path.dirname(xbe)], cwd=here)
except SystemExit:
    print('(health bar fix not applied; run tools\\patch_health_bar.py by hand)')
try:   # the selected profile's name was never put into the join URL (the build typed "SetName" into the console instead); Engine.u PlayerProfile.Initialize now does it. Backup Engine.u.orig_profname, patch_profile_name.py --revert undoes it
    run('Making the selected profile set the player name (Engine.u)', [py, os.path.join(here, 'tools', 'patch_profile_name.py'), os.path.dirname(xbe)], cwd=here)
except SystemExit:
    print('(profile name patch not applied; run tools\\patch_profile_name.py by hand)')
try:
    run('Raising texture detail, weather and high detail actors (UnrealEngine2.ini)', [py, os.path.join(here, 'tools', 'patch_gfx_ini.py'), os.path.dirname(xbe)], cwd=here)
except SystemExit:
    print('(graphics settings not applied; run tools\\patch_gfx_ini.py by hand)')
if '--no-multiplayer' not in flags:
    try:   # co-op campaign / multiplayer fixes: network-only or menu-only changes, single player is not touched. --mp-no-extras leaves out the extras. Undo: tools\\multiplayer\\setup_multiplayer.py <game> --revert
        run('Applying the multiplayer / co-op script fixes', [py, os.path.join(here, 'tools', 'multiplayer', 'setup_multiplayer.py'), os.path.dirname(xbe)] + (['--no-extras'] if '--mp-no-extras' in flags else []), cwd=here)
    except SystemExit:
        print('(multiplayer fixes not applied; run tools\\multiplayer\\setup_multiplayer.py by hand, it says which step failed)')
if '--build' in flags:
    if not shutil.which('cmake'):
        print('cmake not found: open a "x64 Native Tools Command Prompt for VS 2022" and run this script again, or build by hand (below).')
    else:
        run('Configure', ['cmake', '-S', '.', '-B', 'build', '-DXBOXRECOMP_DIR=..'], cwd=here)
        run('Build (Release)', ['cmake', '--build', 'build', '--config', 'Release'], cwd=here)
print('''
Setup finished.''' + ('' if '--build' in flags else '''
To build, in a "x64 Native Tools Command Prompt for VS 2022":
    cd sg1_alliance
    cmake -S . -B build -DXBOXRECOMP_DIR=..
    cmake --build build --config Release'''))
print('''Then run  sg1_alliance\\build\\Release\\dbglaunch.exe sg1_alliance_recomp.exe  (it finds default.xbe in its own or a parent folder; or set RECOMP_GAME_DIR).
Optional next steps: HD pack (tools\\make_hdpack.py) - see README.''')
