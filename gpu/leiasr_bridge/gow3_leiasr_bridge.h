// SPDX-License-Identifier: MIT
//
// C interface between gow3 and gow3_leiasr.dll, which weaves the stereo pair for LeiaSR 3D
// displays (Samsung Odyssey 3D, Acer SpatialLabs) with the LeiaSR SDK's Direct3D 11 weaver.
// The port contains no LeiaSR code: it loads the bridge at run time when 3D is set to LeiaSR,
// and the bridge loads the SR Platform's DLLs (installed with the display's software).
//
// Each frame the port renders both eyes side by side into the shared "views" texture, then
// calls gow3_leiasr_weave(), which weaves them for the window's place on the screen and the
// viewer's tracked eyes into the shared "output" texture, the size of the window's client
// area, which the port presents as it is. Both textures are created here (B8G8R8A8_UNORM,
// sRGB encoded) and shared as NT handles; the port imports them into Vulkan
// (VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT). Synchronisation is on the CPU: the port
// finishes its work on the views before gow3_leiasr_weave(), which returns once the output is
// written.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GOW3_LEIASR_BRIDGE_ABI 1

typedef void (*Gow3LeiaLogFn)(int warning, const char* message);

typedef struct Gow3LeiaDisplay {
    uint32_t view_width;  ///< the display's recommended size of one view, or 0
    uint32_t view_height;
    int32_t left, top, right, bottom; ///< the SR display on the desktop, or zeros
} Gow3LeiaDisplay;

typedef struct Gow3LeiaTextures {
    void* views_handle;  ///< NT handle of the side-by-side views texture
    void* output_handle; ///< NT handle of the woven output texture
} Gow3LeiaTextures;

typedef int (*Gow3LeiaAbiFn)(void);
/// Connects to the SR service (waiting up to `wait_ms` for it), creates a Direct3D 11 device on
/// the adapter with this LUID and a weaver for `hwnd`, and asks for the 3D lens. 0 on success.
typedef int (*Gow3LeiaCreateFn)(const uint8_t luid[8], void* hwnd, uint32_t wait_ms,
                                Gow3LeiaLogFn log, Gow3LeiaDisplay* display);
/// (Re)creates the shared textures: views (both eyes side by side) and output. 0 on success.
typedef int (*Gow3LeiaResizeFn)(uint32_t views_width, uint32_t views_height,
                                uint32_t output_width, uint32_t output_height,
                                Gow3LeiaTextures* textures);
/// Weaves the views into the output and waits for the GPU. 0 on success.
typedef int (*Gow3LeiaWeaveFn)(void);
/// Asks for the 3D lens on or off (2D pictures such as menus look sharper with it off).
typedef void (*Gow3LeiaLensFn)(int on);
typedef void (*Gow3LeiaDestroyFn)(void);

#ifdef GOW3_LEIASR_BRIDGE_BUILD
__declspec(dllexport) int gow3_leiasr_abi(void);
__declspec(dllexport) int gow3_leiasr_create(const uint8_t luid[8], void* hwnd, uint32_t wait_ms,
                                             Gow3LeiaLogFn log, Gow3LeiaDisplay* display);
__declspec(dllexport) int gow3_leiasr_resize(uint32_t views_width, uint32_t views_height,
                                             uint32_t output_width, uint32_t output_height,
                                             Gow3LeiaTextures* textures);
__declspec(dllexport) int gow3_leiasr_weave(void);
__declspec(dllexport) void gow3_leiasr_lens(int on);
__declspec(dllexport) void gow3_leiasr_destroy(void);
#endif

#ifdef __cplusplus
}
#endif
