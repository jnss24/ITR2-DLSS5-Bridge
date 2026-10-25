# ITR2-DLSS5-Bridge

ITR2-DLSS5-Bridge is a small, self-contained Windows DLL (`itr2_dlss5_bridge.dll`) used by the Into the Radius 2 DLSS mod. When it's in the game folder, the mod loads it and can run DLSS 5 (NGX Neural Rendering, feature 18) on the foveated centre window, either before upscaling (Order A) or after it (Order B), with one or two passes per eye.

The bridge loads `nvngx_dlssnr.dll` (version 310.8.x only) itself and calls that DLL's own NGX functions instead of going through the driver. During each call it changes the answer to `GetModuleFileNameW`, so the DLL accepts it as its caller. It exposes a plain C interface (`ITR2_Dlss5Bridge_GetApi`, ABI v1) and has no build dependency on the main mod or on NVIDIA's SDK headers.

The code is adapted from `Runtime.cpp` in Maeyanie/OpenShadersNRForked (commit e9c5f967), licensed GPL-3.0-or-later with that project's modding exceptions. `nvngx_dlssnr.dll` is not included.
