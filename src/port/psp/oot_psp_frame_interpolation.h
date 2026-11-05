#ifndef OOT_PSP_FRAME_INTERPOLATION_H
#define OOT_PSP_FRAME_INTERPOLATION_H

/* Render history owns copies of transforms, never pointers into an old graphics
 * arena. Simulation, collision, animation events and display lists stay intact. */
void OotPspFrameInterpolation_Reset(void);
void OotPspFrameInterpolation_Begin(const void* state, int scene, int room, int camera, int enabled);
void OotPspFrameInterpolation_SetContext(int scene, int room, int camera);
int OotPspFrameInterpolation_HasSample(void);
void OotPspFrameInterpolation_SetWorldScope(int enabled);
void OotPspFrameInterpolation_SetIdentity(const void* identity);
void OotPspFrameInterpolation_ForgetIdentity(const void* identity);
void OotPspFrameInterpolation_Record(const void* address, const float value[4][4], const void* site);
void OotPspFrameInterpolation_End(void);
void OotPspFrameInterpolation_SetFraction(float fraction);
void OotPspFrameInterpolation_Resolve(const void* address, float value[4][4]);

#endif
