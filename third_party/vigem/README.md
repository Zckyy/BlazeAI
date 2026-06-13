# ViGEmClient SDK (optional virtual-controller backend)

These headers/lib back the **"Virtual Controller (ViGEm)"** mouse-injection method.
`CMakeLists.txt` only links/compiles this backend when
`third_party/vigem/include/ViGEm/Client.h` exists, so on a fresh clone (this folder is
empty and gitignored) the app builds fine and `MOUSE_VIGEM` just stays unavailable in
the UI.

## Re-fetching it

Build the SDK from source (it produces a static `ViGEmClient.lib`, no DLL):

```powershell
cd $env:TEMP
git clone --depth 1 https://github.com/ViGEm/ViGEmClient.git
cd ViGEmClient
cmake -B build -A x64 -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreadedDLL"
cmake --build build --config Release
```

Then copy into this folder:

```powershell
$BLAZE = "C:\path\to\BlazeAI\third_party\vigem"
New-Item -ItemType Directory -Force "$BLAZE\include\ViGEm","$BLAZE\lib\x64" | Out-Null
Copy-Item "$env:TEMP\ViGEmClient\include\ViGEm\*.h" "$BLAZE\include\ViGEm"
Copy-Item "$env:TEMP\ViGEmClient\build\Release\ViGEmClient.lib" "$BLAZE\lib\x64"
```

## Runtime requirement

The **ViGEmBus driver** must also be installed on the machine (not just this SDK) for
`vigem_connect()` to succeed: https://github.com/ViGEm/ViGEmBus/releases

To make games actually pick up the virtual pad instead of your physical controller,
hide the physical controller's HID interface with **HidHide**:
https://github.com/nefarius/HidHide
