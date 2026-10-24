# Multiplayer / co-op script fixes

`setup_multiplayer.py "<game folder>"` applies them all (setup_sg1.py does it for you; `--no-multiplayer` skips it, `--mp-no-extras` leaves out the extras).
Undo: `setup_multiplayer.py "<game folder>" --revert` (restores `*.orig_mp`). Host and joiners must use the same patched files.

Default set: only network-game code paths, multiplayer menus, or the "simulated" flag on animation functions, so single player behaves as before.
- start (no lobby wait in a network campaign), mode (StoryModeGame in the host list), browser + listinfo (campaign servers visible, real player count)
- restart + restart2 (restart / mission-failed restart keeps the listen server and the clients follow, with game_hooks/recomp_0023_coop_reconnect.patch)
- revive, revivefx, selfrevive, bleedout, downfix, downpose (network campaign: downed instead of dead, revive bar text, no self revive,
  bleed-out kills the player then the mission fails, downed pose the same on every screen)
- clientfx / clientfx2 / clientfx3 / clientfx5 --marker (other players lying / revived / look up-down, needs game_hooks/recomp_0027_remote_player_look.patch)
- animfix, simfuncs, melee, reload (weapon pose, crouch/walk legs, melee and reload animations of other players)
- pkgext (widens export size fields only, no new names/imports: the client rejects those)
- clientdown + downself (a downed joining player cannot walk or shoot, and lies flat on his own screen)
- clienthud (a joining player's HUD bar shows his health)
- sceneend + sceneend2 (a joining player gets his character and HUD back after a cutscene)
- crosshair (a joining player's crosshair turns green on allies / red on enemies)
- nopause (the Tab screen does not pause a network game; single player still pauses)
- UnrealEngine2.ini, net driver sections only: LanServerMaxTickRate/NetServerMaxTickRate 60, MaxClientRate 100000, ConnectionTimeout 60
  (smoother remote players; a joiner is not dropped while a big map loads)

Extras (also default; they run in single player too but leave it unchanged there; `--no-extras` skips them):
chars (joiner gets the chosen character), takeover + takeover2 (joiner takes over the AI squadmate's pawn, spectates during cutscenes),
objectives (info screen objective list rebuilt on open).

Native side (exe, applied by game_hooks/apply_hooks.py): recomp_0023 (restart reconnect), recomp_0027 (remote look), recomp_0043_drawtext_optcolor
(Tab OBJECTIVES text was invisible on the host and in single player), src/xnet_shim.c (UDP reset errors ignored, LAN / SG_NET_PEER discovery).

Notes on how it works: see the project notes sg1-networking-notes / sg1-coop-notes-night7*.
