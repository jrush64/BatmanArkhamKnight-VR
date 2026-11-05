================================================================================
  BATMAN: ARKHAM KNIGHT VR  (experimental, unfinished)
================================================================================

WHAT YOU NEED

  - Batman: Arkham Knight (Steam, PC).
  - A PC VR headset and an OpenXR runtime (SteamVR, Meta Quest Link,
    Virtual Desktop, etc.). Set the runtime you use as the active OpenXR runtime.
  - A strong GPU: the game is rendered twice per frame, once per eye.

INSTALLING

  1. Close the game.

  2. Find your Arkham Knight folder. In Steam, right click the game, then
     Manage, then Browse local files. From there open  Binaries\Win64.

  3. Copy these files into that folder, next to BatmanAK.exe:

         dxgi.dll            (from the builds folder)
         openxr_loader.dll   (from this folder)
         akvr.ini            (from the runtime folder)

  4. Start your VR runtime, then start the game normally from Steam.

USING IT

  Insert   opens the tuning menu (IPD, convergence, eye swap, head tracking).
           Settings save to akvr.ini when the menu closes.
  Home     recenters the view (can be changed in the menu).
  F5       turns headset output on or off. The desktop shows a
           side-by-side image either way.
  F2       turns the stereo double render on or off.

  If depth looks inside out, open the menu and toggle "Swap eyes".

UNINSTALLING

  Delete dxgi.dll, openxr_loader.dll and akvr.ini from Binaries\Win64
  (and the logs folder and AK_XR.log if they were created).

THIRD-PARTY

  openxr_loader.dll is the Khronos OpenXR loader, Apache License 2.0.
  See openxr_loader.LICENSE.txt.
