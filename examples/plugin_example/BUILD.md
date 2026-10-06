Build example plugin (MSYS2 UCRT64):

```powershell
$ucrt = "C:\msys64\ucrt64\bin"
& "$ucrt\cc.exe" -shared -O2 -Wall -Wextra -o example_rect_tracker.dll `
  examples\plugin_example\plugin_example.c
```

Copy the built DLL next to the app:

```powershell
Copy-Item -Force .\example_rect_tracker.dll .\dist\plugins\
```

Then run the app and press `F10` to enable it.

