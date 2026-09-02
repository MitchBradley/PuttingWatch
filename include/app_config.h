#pragma once

// Build-time app options.

// The post-calibration "Rest captured" screen briefly shows the averaged
// rest-gravity vector (x y z) before monitoring starts. It is an IMU /
// calibration debugging aid, not something end users need, so production
// builds skip it: the "Hold still..." screen hands straight off to the
// monitoring screen.
//
// Set to 1 (e.g. add -DSHOW_REST_CAPTURED_SCREEN=1 to build_flags in
// platformio.ini) to bring it back for debugging. Nothing is deleted when it
// is 0: the screen's rendering code still lives in src/ui.cpp (case
// State::Done), the rest-gravity vector is still plumbed into the UI snapshot,
// and the model's Done state is unchanged - main.cpp just sets
// SwingTracker::Config::monitoringDelayMs to 0 so Done is a single-tick
// pass-through to Monitoring instead of a 2 s dwell.
#ifndef SHOW_REST_CAPTURED_SCREEN
#    define SHOW_REST_CAPTURED_SCREEN 0
#endif
