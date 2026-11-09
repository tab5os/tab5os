/* SPDX-License-Identifier: Apache-2.0
 * The composition root's media wiring: what the camera needs from the UI
 * (rotation, the UI task, the memory loan for the H.264 encoder).
 */
#pragma once

/* Install the camera's host; call before t5_camera_init(). Also takes the
 * UI's 100 KB internal block now, before USB and the rest fragment internal
 * RAM (t5_ui_init would otherwise be the first to ask). */
void board_media_wire(void);
