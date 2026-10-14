#ifndef EMULATOR_PUBLIC_H
#define EMULATOR_PUBLIC_H

#define CONTROLLER_MAP_FLAG_AXIS		0x4000
#define CONTROLLER_MAP_FLAG_INVERSE		0x8000

typedef struct
{
	int id;

	int kc_square, kc_circle, kc_triangle, kc_cross;

	int kc_l1, kc_l2, kc_l3;
	int kc_r1, kc_r2, kc_r3;

	int kc_start, kc_select;

	int kc_dpad_left, kc_dpad_right, kc_dpad_up, kc_dpad_down;
} PsyXKeyboardMapping;

typedef struct
{
	int id;

	// you can bind axis by adding CONTROLLER_MAP_AXIS_FLAG
	int gc_square, gc_circle, gc_triangle, gc_cross;

	int gc_l1, gc_l2, gc_l3;
	int gc_r1, gc_r2, gc_r3;

	int gc_start, gc_select;

	int gc_dpad_left, gc_dpad_right, gc_dpad_up, gc_dpad_down;

	int gc_axis_left_x, gc_axis_left_y;
	int gc_axis_right_x, gc_axis_right_y;
} PsyXControllerMapping;

typedef void(*GameDebugKeysHandlerFunc)(int nKey, char down);
typedef void(*GameDebugMouseHandlerFunc)(int x, int y, int dx, int dy);
typedef void(*GameOnTextInputHandler)(const char* buf);

//------------------------------------------------------------------------

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

/* Mapped inputs */
extern PsyXControllerMapping		g_cfg_controllerMapping;
extern PsyXKeyboardMapping			g_cfg_keyboardMapping;
extern int							g_cfg_controllerToSlotMapping[2];

/* Game inputs */
extern GameOnTextInputHandler		g_cfg_gameOnTextInput;

/* Graphics configuration */
extern int							g_cfg_swapInterval;
extern int							g_cfg_pgxpZBuffer;
extern int							g_cfg_bilinearFiltering;
extern int							g_cfg_pgxpTextureCorrection;

/* Debug inputs */
extern GameDebugKeysHandlerFunc		g_dbg_gameDebugKeys;
extern GameDebugMouseHandlerFunc	g_dbg_gameDebugMouse;

/* Usually called at the beginning of main function */
extern void PsyX_Initialise(char* windowName, int screenWidth, int screenHeight, int fullscreen);

/* Cleans all resources and closes open instances */
extern void PsyX_Shutdown(void);

/* Returns the screen size dimensions */
extern void PsyX_GetScreenSize(int* screenWidth, int* screenHeight);

/* Sets mouse cursor position */
extern void PsyX_SetCursorPosition(int x, int y);

/* Sets mouse relative movement */
extern void PsyX_SetCursorRelative(int enable);

/* Returns the mouse movement collected since the last call (only counted while relative mode is on) and clears it.
   The return value is 1 when relative mode is on, else 0. */
extern int PsyX_GetMouseDelta(int* dx, int* dy);

/* Snow layer on the road: up-facing 3D surfaces near the road level. amount 0..1, groundPos = a point on the road level and
   groundUp = unit vector pointing up, both in view space (GTE units) */
extern void PsyX_SetSnow(float amount, const float* groundPos, const float* groundUp);

/* Distance fog on 3D geometry: amount 0..1 (1 = about 8 cells of visibility, falls off with the square of the distance),
   extinction = weather fog per GTE unit (exponential), color = rgb 0..1 */
extern void PsyX_SetFog(float amount, float extinction, const float* color);

/* Headlight cone of the player car, position and direction in view space (GTE units), amount 0..1 */
extern void PsyX_SetBeam(float amount, const float* pos, const float* dir);

/* Lamps that light the air (halos, also in front of walls): pos = count * 3 floats in view space like PsyX_SetBeam,
   color = count * 4 floats (rgb 0..1, radius), dir = count * 3 floats (unit headlight direction that also lights
   surfaces in front of it, 0 for a lamp), density 0 = off. amount = in-scattering of the headlight cone. */
#define PSYX_MAX_VOLUME_LIGHTS 16
extern void PsyX_SetVolumeLights(int count, const float* pos, const float* color, const float* dir, float density);
extern void PsyX_SetVolumeBeam(float amount);

/* Height fog on top of the distance fog: optical depth heightDensity * exp(-heightFalloff * height) per unit, height above
   sea level (camHeight for the camera). worldUpView and sunDirView are unit vectors in view space (GTE units, y down);
   sunScatter 0..1 turns the fog towards sunColor when looking into the sun. heightDensity 0 = off. */
extern void PsyX_SetAtmosphere(float heightFalloff, float heightDensity, const float* worldUpView, float camHeight, const float* sunDirView, const float* sunColor, float sunScatter);

/* Usually called after ClearOTag/ClearOTagR */
extern char PsyX_BeginScene(void);

/* Usually called after DrawOTag/DrawOTagEnv */
extern void PsyX_EndScene(void);

/* Explicitly updates emulator input loop */
extern void PsyX_UpdateInput(void);

/* Returns keyboard mapping index */
extern int PsyX_LookupKeyboardMapping(const char* str, int default_value);

/* Returns controller mapping index */
extern int PsyX_LookupGameControllerMapping(const char* str, int default_value);

/* Screen size of emulated PSX viewport with widescreen offsets */
extern void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16* rect);

/* Waits for timer */
extern void PsyX_WaitForTimestep(int count);

/* Changes swap interval state */
extern void PsyX_EnableSwapInterval(int enable);

/* Changes swap interval interval interval */
extern void PsyX_SetSwapInterval(int interval);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif