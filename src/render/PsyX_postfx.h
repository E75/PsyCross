#ifndef PSYX_POSTFX_H
#define PSYX_POSTFX_H

// Post passes over the frame drawn so far (PsyX_postfx.cpp).
// Sun shafts: screen space god rays ("Volumetric Light Scattering as a Post-Process", GPU Gems 3 chapter 13).
// sunX, sunY, horizonY are PSX screen positions mapped to -0.5..0.5 (the same space as the 2D vertices),
// amount 0..255, color = 3 floats 0..1. Pixels below horizonY never count as sky.
void PsyX_PostFx_SunShafts(int amount, float sunX, float sunY, float horizonY, const float* color);
void PsyX_PostFx_Shutdown(void);

#endif // PSYX_POSTFX_H
