#pragma once
#include <cstdint>
struct Quat { float x, y, z, w; };
struct Vec3f { float x, y, z; };
struct Pose { Quat ori; Vec3f pos; };   // OpenXR axes: +X right, +Y up, -Z forward; metres

struct CameraInputs {
    alignas(16) float eye[4];
    alignas(16) float target[4];
    alignas(16) float up[4];
    float a5; uint8_t a6; void** handle; bool valid;
};

bool Camera_Install();
void Camera_NotePresent();
void Camera_LogStats();
const CameraInputs& Camera_LastInputs();

// vr.cpp: head pose relative to the recentred reference (or the fake debug motion)
bool VR_GetHeadPose(Pose* out);

struct EyeSetup { Pose pose; float tanL, tanR, tanU, tanD; };

// vr.cpp: stereo frame control (called from the render-frame hook on the main thread)
bool VR_StereoWanted();
bool VR_StereoBegin(EyeSetup eyes[4]);     // xrWaitFrame/xrBeginFrame + per-view poses/FOVs (4 with quad views)
void VR_StereoCaptureEye(int eye);         // copy the game's back buffer into the eye swapchain

void VR_ReturnLatencyToken();
void VR_GpuSync();
int Camera_LastStereoEye();
bool Camera_InEye0Present();
