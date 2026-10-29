// The parts of SCDA-XBERecomp's app layer the renderer sources (hle/tgp)
// call. Window, focus and handover are this runtime's own (fb_present*.c);
// Pace() is the HLE swap's frame pacing (hle_glue.cpp).
#pragma once

namespace app {
inline void WaitWhileUnfocused() {}
inline void SplashFirstFrame() {}
inline bool HandingOver() { return false; }
inline void FinishHandover() {}
void Pace();
}  // namespace app
