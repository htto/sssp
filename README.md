# sssp
Steam ScreenShot Preload (Linux/Unix)

A small LD_PRELOAD library for steam games, that captures a game Screenshot.
Written because the steam overlay is killing performance for me and I didn't
use much of it besides the screenshot feature.

Just edit the game's launch options to something like:
- env LD_PRELOAD=/path/to/this/sssp_XY.so:$LD_PRELOAD %command%

Or even:
- env LD_PRELOAD=/path/to/this/sssp_32.so:/path/to/this/sssp_64.so:$LD_PRELOAD %command%

In the game, hitting the screenshot hotkey captures the current window and
issues the screenshot directly to steam. Steam's screenshot handler should
pop up after the game quit.

Buildable by issuing:
- gcc -m32 -o sssp_32.so sssp.c -shared -fPIC `pkg-config --cflags --libs x11`
- gcc -m64 -o sssp_64.so sssp.c -shared -fPIC `pkg-config --cflags --libs x11`

As always: Your mileage may vary. This library may even cause instabilty/crashes
to games or steam, and is NOT supported by Steam in any way. Don't blame Valve
or me.


TODO:
 - use some other hotkey than F12
 - performance improvements for the X event handling
 - some more validations
 - cleanup
