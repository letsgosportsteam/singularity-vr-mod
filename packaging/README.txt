================================================================================
 SINGULARITY VR  -  ALPHA
 A VR mod for Singularity (2010, Raven Software)
 https://github.com/letsgosportsteam/singularity-vr-mod
================================================================================

This is a d3d9.dll proxy. It forwards every Direct3D 9 call to the real system
library and, along the way, renders the game in stereo to an OpenXR headset with
6-DOF head tracking, motion-controller input, and the gun following your hand.

No game files are modified. Uninstall by deleting one file.

THIS IS AN ALPHA. It has run on very few machines so far.


--------------------------------------------------------------------------------
1. WHAT YOU NEED
--------------------------------------------------------------------------------

 * Singularity (2010) for PC - Steam or GOG.

 * VIRTUAL DESKTOP, with VDXR selected as the OpenXR runtime. This build only
   supports VDXR. All testing has been done on a Quest 3S.

 * Microsoft Visual C++ 2015-2022 Redistributable (x86).
   The x86 one. The x64 one you already have will not do. This is needed to
   RUN the game with the mod installed, not just to build it.
   https://aka.ms/vs/17/release/vc_redist.x86.exe

 * Windows 10 or 11.

 * HIGHLY RECOMMENDED: a DEDICATED ROUTER, with the PC on ethernet to it - OR
   Virtual Desktop's wired USB connection, currently in beta.


--------------------------------------------------------------------------------
2. INSTALL
--------------------------------------------------------------------------------

Download the release zip and unzip everything from it into the game's
Binaries folder, beside Singularity.exe:

    d3d9.dll
    openxr_loader.dll
    SingularityVR.ini

Typical locations:

    Steam:  ...\steamapps\common\Singularity\Binaries\
    GOG:    ...\GOG Games\Singularity\Binaries\

That is the whole install. SingularityVR.ini ships pre-configured - the VR
settings menu (section 4) changes it live and saves automatically, so you
rarely need to touch it by hand.

IN THE GAME'S VIDEO SETTINGS, UNCHECK EVERY OPTION. High Quality Decals in
particular enables dynamic shadows, which are currently broken.

SingularityVR.ini.example, in the repository, is a fully-documented reference
of every setting the mod reads, in case you want to go further than the menu.

To uninstall: delete d3d9.dll. Nothing else was touched.


--------------------------------------------------------------------------------
3. FIRST LAUNCH
--------------------------------------------------------------------------------

Connect Virtual Desktop FIRST, then launch Singularity.exe with no arguments.
It starts in VR. There is no keypress and no command line.

THE FIRST RUN WILL BE THE WRONG RESOLUTION. That is expected and it is not a
bug. The mod has to inject -ResX/-ResY before the engine reads the command
line, which happens before it can ask the headset anything - so it asks the
headset during the run and uses the answer on the NEXT launch.

    Launch once. Quit. Launch again. The second run is at full resolution.

The same applies after you change your headset's render scale: one stale run,
then correct.


--------------------------------------------------------------------------------
4. CONTROLS
--------------------------------------------------------------------------------

Touch controllers act as a full gamepad, with haptics. Every physical input on
both controllers, and what it does:

  LEFT CONTROLLER

    Thumbstick ............... move / strafe
    Thumbstick click ......... dash
    Trigger ................... aim
    Grip ....................... impulse / melee
    X ........................... age / renovate (TMD)
    Y ........................... flashlight
                                  HOLD ~1 sec: open the VR settings menu instead
    Menu ..................... pause menu
                                  HOLD ~1 sec: recentre (re-zeroes your view
                                  and your seating)

  RIGHT CONTROLLER

    Thumbstick ............... turn (snap or smooth, set in the settings menu)
    Thumbstick - tilt up ..... cycle weapons
    Thumbstick - tilt down ... use a health item
    Thumbstick click ......... anti-gravity
    Trigger ................... fire
    Grip ....................... use / reload
    A ............................ jump
                                  HOLD ~1 sec: also toggles the laser sight
                                  (you will jump once on the way - jump has to
                                  stay instant)
    B ............................ crouch

Everything beyond the two long presses is Raven's own binding, passed straight
through. The settings menu (HOLD Y) is where everything else - turn mode,
weapon alignment, occlusion, and the rest - is configured, in the headset,
while the game runs. Changes apply live and are saved when you close it.

Note: the F-key debug bindings mentioned in the repo's development notes are
inert in this build. They only exist when Debug=1.


--------------------------------------------------------------------------------
5. CONFIGURATION
--------------------------------------------------------------------------------

Settings live in SingularityVR.ini in the same Binaries folder, and the VR
settings menu is the intended way to change them.

Two warnings if you hand-edit:

  * The ini takes the FIRST key of a duplicated name, silently. A second copy
    of a key further down the file does nothing.

  * AutoResX / AutoResY are the mod's own cache of YOUR headset's size. Never
    copy them from someone else's ini. Delete them to force a re-cache.

Anything you set in the ini overrides the compiled-in default, including
defaults that were tuned by testing. If something behaves oddly after editing,
delete the ini and let the code defaults take over.


--------------------------------------------------------------------------------
6. KNOWN ISSUES
--------------------------------------------------------------------------------

  * This has only been tested on one machine, so your experience may vary.

  * Missing or culled geometry is mostly resolved outside of in-engine
    cutscenes, where it isn't fixable yet. If you notice geometry not
    rendering elsewhere, set OCCLUSION to DRAW ALL on the ADVANCED page of the
    settings menu.

  * A menu rotation bug: entering a game, pausing, then exiting to the main
    menu can leave the menu rotated. Restart the game to clear it.

  * PhysX. If the game dies instantly with no message, that is usually a
    missing 32-bit PhysXLoader.dll, and the UNMODDED game does it too - other
    games' PhysX installers can delete the copy Singularity needs. The repo
    ships setup_physx.ps1, which makes the game self-contained using a copy
    already on your own machine.

The repo's STATUS.md is the live, honest state of every one of these. It is
more current than this file will ever be.


--------------------------------------------------------------------------------
7. PLANNED
--------------------------------------------------------------------------------

Next up: more testing, general bug fixes, and performance improvements. Also
on deck: comfort settings (height adjustment, vignettes), shrinking flat
cutscenes so they don't fill your whole view in the headset, testing the
late-game weapons, fixing ammo/health/radio HUD positioning for better
headset visibility, and an improved settings menu.

Further out: SteamVR/OpenVR support, and Xbox controller support for the
head-aiming mode.

Longer shots, and both will be tough: two-hand tracking, and possibly manual
reloading.


--------------------------------------------------------------------------------
8. REPORTING A BUG
--------------------------------------------------------------------------------

Open an issue at:

    https://github.com/letsgosportsteam/singularity-vr-mod/issues

Attach the log. It is at:

    %LOCALAPPDATA%\SingularityVR\view_matrix.log

Paste that path into Explorer's address bar. view_matrix.log is always the run
that just happened; older runs sit beside it stamped with their start time.

Please say which headset, which OpenXR runtime, Steam or GOG, and whether you
had already relaunched once (section 3).

A CONTROL THAT COSTS ONE MINUTE AND SAVES EVERYONE TIME: rename d3d9.dll to
d3d9.dll.off and run the game again at the same resolution. If the problem is
still there, it is not this mod. Please say in the report whether you tried it.


--------------------------------------------------------------------------------
9. LICENCE
--------------------------------------------------------------------------------

MIT. See LICENSE in the repository.

Third-party components and their licences are listed in
THIRD-PARTY-NOTICES.txt, also in the repository.

Not affiliated with or endorsed by Activision or Raven Software. Singularity is
their trademark. This mod contains no game code or assets, redistributes no
game content, and modifies no game files.
