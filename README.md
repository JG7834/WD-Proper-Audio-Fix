WDAudioFix - Watch Dogs sound effects fix for S/PDIF / digital audio outputs

Fixes missing audio in Watch Dogs where users utilize Realtek Digital Output or similar. The previous mod (WD-ForceStereo) that is on Nexus does not fix the audio per se, it only forces channels into 2 in where their volume is too low. This new plugin kind of does the same but also tricks the game into outputting audio by spoofing the hardware name to output to.  Fixes audio in special cases, where a user may have a soundbar connected through SPDIF and using a Windows hack to get Dolby Digital (I would know.)

=========================================================================

THE PROBLEM
-----------
On some digital audio outputs (S/PDIF optical/coaxial, especially with Dolby
Digital Live or DTS Interactive enabled), Watch Dogs plays the menu music but
all sound effects are missing: no clicks, footsteps, gunfire or dialogue.

The game reads the output's internal device format. On these outputs that's
the Dolby Digital carrier format, and the game's sound-effects engine stops
producing any audio. This plugin makes the device report a plain stereo PCM
format to the game instead. Windows still mixes the game's audio into your
real output, so Dolby Digital Live and your other settings keep working.


REQUIREMENTS
------------
- Watch Dogs (PC, 64-bit)
- Windows 8 or later
- An ASI loader (ie: Ultimate ASI Loader). Skip it if you already have
  one installed for another mod.


INSTALLATION
------------
1. Open the game's "bin" folder - the folder that contains Watch_Dogs.exe.
     Steam:    ...\steamapps\common\Watch_Dogs\bin
     Ubisoft:  ...\Ubisoft Game Launcher\games\Watch_Dogs\bin

2. Copy these files into it:
     WDAudioFix.asi
     WDAudioFix.ini

3. Launch the game. Sound effects should work.


CONFIGURATION (WDAudioFix.ini)
------------------------------
FakeDevice=Digital Output
    The fix applies to any playback device whose name contains this text
    (not case-sensitive). The default matches Realtek's "Realtek Digital
    Output". If your device has a different name, change this to part of its
    name exactly as it appears in Windows Sound settings, for example:
        FakeDevice=SPDIF
        FakeDevice=Digital Audio
    Leave it empty to turn the fix off.

FakeChannels=2 / FakeBits=24
    The format reported to the game. Stereo 24-bit is the tested setting.

ForceMixFormat=1, AutoConvert=1, StripRawMode=1
    Supporting fixes. Leave them on unless troubleshooting.

[Log] RenderStats / DeviceProperties
    Diagnostic logging to WDAudioFix.log. Both are capped, so the log stays
    small. Set to 0 to turn off.


TROUBLESHOOTING
---------------
No WDAudioFix.log appears in the bin folder
    The ASI loader isn't loading. Try renaming dinput8.dll to version.dll or
    winmm.dll. Make sure you used the 64-bit loader.

Still no sound effects
    - Open WDAudioFix.log and look for lines containing "-> faked". If there
      are none, FakeDevice doesn't match your device's name - check the
      "Default render device:" line near the top of the log for the exact
      name and update FakeDevice.
    - Make sure the device you listen on is set as the Windows default
      playback device before launching the game.

When reporting a problem, include WDAudioFix.log.


UNINSTALL
---------
Delete WDAudioFix.asi, WDAudioFix.ini and WDAudioFix.log from the bin folder.
Remove dinput8.dll too if no other mod uses it.


CREDITS / LICENSES
------------------
MinHook - Copyright (C) Tsuda Kageyu. BSD 2-Clause license, see
MinHook-LICENSE.txt.
Ultimate ASI Loader - by ThirteenAG. MIT license, see its included license
file.
