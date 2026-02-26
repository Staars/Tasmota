/*********************************************************************************\
  xdrv_81_5_esp32_webcam_CSI_overlay.ino - PPA SRM Overlay for Tasmota ESP32-P4
 
   Copyright (C) 2025  Christian Baars and Theo Arends

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

 



 * ---- WHY SRM AND NOT BLEND -----------------------------------------------------
 *
 * We investigated all three PPA operations:
 *
 * 1. ppa_do_blend  — REJECTED (ESP-IDF bug)
 *    The natural choice: composite an ARGB8888 fg over the YUV420 frame in-place.
 *    Failed with ESP_ERR_INVALID_ARG (258) on EVERY frame.
 *    Root cause 1: frame buffers were only 64-byte aligned in SPIRAM; PPA requires
 *      128-byte alignment for L1+L2 cache lines on ESP32-P4. Fixed in core.ino.
 *    Root cause 2 (blend-specific): even after the alignment fix, blend kept
 *      failing. Confirmed via diagnostic logging that all field values were correct.
 *      The actual cause is a known ESP-IDF bug (fixed in v5.4-beta2):
 *      "PPA blend data process error caused by mismatching writeback/invalidate
 *       data length if in_bg and out point to the same buffer and L2 cache line
 *       size > L1 cache line size" — exactly our SPIRAM setup on ESP32-P4.
 *    Workaround attempt: use a separate small YUV420 output tile + CPU memcpy
 *      back into the frame. Also failed — the blend driver's INVALID_ARG
 *      validation rejects the configuration regardless of separate buffers,
 *      likely due to additional constraints in the version we are using.
 *    Conclusion: blend is broken on this ESP-IDF version for YUV420 output.
 *
 * 2. ppa_do_fill   — REJECTED (wrong tool)
 *    Can only fill a solid colour rectangle. Cannot blit a pre-built pattern.
 *    Could be used for solid-colour overlays but not checkerboard or Berry canvas.
 *
 * 3. ppa_do_scale_rotate_mirror (SRM) — CHOSEN ✓
 *    Blits an RGB565 source buffer onto the YUV420 frame with hardware colour
 *    conversion. One call per frame. No alpha blending (opaque blit), which is
 *    fine: for the Berry canvas, Berry draws whatever it wants into the RGB565
 *    buffer and SRM stamps it directly onto the frame.
 *    Works perfectly once the 128-byte alignment fix was applied to core.ino.
 *
 * ---- MODES ---------------------------------------------------------------------
 *
 * wcoverlay 0
 *   Disable and free all overlay resources.
 *   Response: {"WcOverlay":0}
 *
 * wcoverlay 1
 *   Enable built-in green/black 8x8 checkerboard at position (16,16), size 32x32.
 *   Useful for testing that the overlay pipeline works.
 *   Response: {"WcOverlay":1}
 *
 * wcoverlay <pos_x>,<pos_y>,<width>,<height>
 *   Allocate a Berry-accessible RGB565 canvas at the given position/size.
 *   All four parameters must be even numbers (YUV420 constraint).
 *   Berry can write any pixels into the returned buffer address (RGB565 format),
 *   then the SRM engine blits it onto every subsequent frame automatically.
 *   The buffer is NOT double-buffered and NOT mutex-protected: Berry should
 *   update it between frames, not during encoding. Safe because Berry runs on
 *   the same core as the Tasmota main loop, which is separate from the encoder task.
 *   After every write from Berry, call:
 *     import webcam
 *     webcam.overlay_flush()    # or manually: tasmota.cmd("WcOverlayFlush")
 *   to flush the CPU cache to SPIRAM so the PPA DMA sees the new pixels.
 *   Response: {"WcOverlay":2,"addr":"0x481a7d00","size":12800}
 *
 * ---- IMPORTANT: BUFFER ALIGNMENT -----------------------------------------------
 *
 *   All buffers passed to PPA in external SPIRAM MUST be 128-byte aligned.
 *   ESP32-P4 has L1 cache line = 64 bytes, L2 cache line = 64 bytes.
 *   PPA requires alignment to BOTH -> 128 bytes minimum.
 *   The frame buffers in core.ino are allocated with heap_caps_aligned_calloc(128).
 *   The fg_buf and berry_buf here are allocated with heap_caps_aligned_alloc(128).
 *   Using 64-byte alignment causes every second frame to fail (alternating buffers).
 *
\*********************************************************************************/
#ifdef ESP32
#ifdef USE_CSI_WEBCAM

// ---------------------------------------------------------------------------
// Checkerboard geometry (mode 1)
// ---------------------------------------------------------------------------
#define WC_CB_WIDTH    32           // Checkerboard width  (pixels, even)
#define WC_CB_HEIGHT   32           // Checkerboard height (pixels, even)
#define WC_CB_POS_X    16           // X offset in frame   (pixels, even)
#define WC_CB_POS_Y    16           // Y offset in frame   (pixels, even)
#define WC_CB_BLOCK     8           // Tile size           (pixels)
#define WC_RGB565_GREEN 0x07E0      // Pure green in RGB565
#define WC_RGB565_BLACK 0x0000      // Black      in RGB565

// Pre-built SRM config — static fields set in WcOverlayInit, dynamic per-call
static ppa_srm_oper_config_t s_srm_cfg;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Allocate a 128-byte aligned DMA-capable SPIRAM buffer
static void* wc_alloc(size_t size) {
  return heap_caps_aligned_alloc(128, size, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// Build the static parts of the SRM config for a given source buffer/geometry.
// out.buffer/pic_w/pic_h are set per-call in WcApplyOverlay.
static void wc_build_srm(void *src_buf, uint16_t src_w, uint16_t src_h,
                          uint16_t dst_x, uint16_t dst_y) {
  memset(&s_srm_cfg, 0, sizeof(s_srm_cfg));

  s_srm_cfg.in.buffer         = src_buf;
  s_srm_cfg.in.pic_w          = src_w;
  s_srm_cfg.in.pic_h          = src_h;
  s_srm_cfg.in.block_w        = src_w;
  s_srm_cfg.in.block_h        = src_h;
  s_srm_cfg.in.block_offset_x = 0;
  s_srm_cfg.in.block_offset_y = 0;
  s_srm_cfg.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;

  s_srm_cfg.out.block_offset_x = dst_x;
  s_srm_cfg.out.block_offset_y = dst_y;
  s_srm_cfg.out.srm_cm         = PPA_SRM_COLOR_MODE_YUV420;
  s_srm_cfg.out.yuv_range      = PPA_COLOR_RANGE_FULL;
  s_srm_cfg.out.yuv_std        = PPA_COLOR_CONV_STD_RGB_YUV_BT601;

  s_srm_cfg.rotation_angle    = PPA_SRM_ROTATION_ANGLE_0;
  s_srm_cfg.scale_x           = 1.0f;
  s_srm_cfg.scale_y           = 1.0f;
  s_srm_cfg.mirror_x          = false;
  s_srm_cfg.mirror_y          = false;
  s_srm_cfg.alpha_update_mode = PPA_ALPHA_NO_CHANGE;  // 0, but explicit for clarity
  s_srm_cfg.mode              = PPA_TRANS_MODE_BLOCKING;
}

// ---------------------------------------------------------------------------
// WcOverlayDeinit — free all resources, called by wcoverlay 0 or on re-init
// ---------------------------------------------------------------------------
void WcOverlayDeinit(void) {
  Wc.overlay.enabled        = false;
  if (Wc.overlay.fg_buf)     { heap_caps_free(Wc.overlay.fg_buf);     Wc.overlay.fg_buf     = nullptr; }
  if (Wc.overlay.berry_buf)  { heap_caps_free(Wc.overlay.berry_buf);  Wc.overlay.berry_buf  = nullptr; }
  if (Wc.overlay.client)     { ppa_unregister_client(Wc.overlay.client); Wc.overlay.client   = nullptr; }
  Wc.overlay.berry_buf_size = 0;
  Wc.overlay.mode           = 0;
}

// ---------------------------------------------------------------------------
// WcOverlayInitCheckerboard — mode 1: built-in green/black test pattern
// ---------------------------------------------------------------------------
static bool WcOverlayInitCheckerboard(void) {
  // Register SRM client
  ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
  if (ppa_register_client(&ppa_cfg, &Wc.overlay.client) != ESP_OK) return false;

  // Allocate RGB565 source buffer
  size_t buf_size = WC_CB_WIDTH * WC_CB_HEIGHT * sizeof(uint16_t);
  Wc.overlay.fg_buf = (uint32_t*)wc_alloc(buf_size);
  if (!Wc.overlay.fg_buf) { ppa_unregister_client(Wc.overlay.client); Wc.overlay.client = nullptr; return false; }

  // Draw checkerboard
  uint16_t *p = (uint16_t*)Wc.overlay.fg_buf;
  for (int y = 0; y < WC_CB_HEIGHT; y++)
    for (int x = 0; x < WC_CB_WIDTH; x++)
      p[y * WC_CB_WIDTH + x] = (((x / WC_CB_BLOCK) + (y / WC_CB_BLOCK)) % 2) == 0
                                ? WC_RGB565_GREEN : WC_RGB565_BLACK;

  // Flush CPU cache -> SPIRAM so PPA DMA sees the pixels
  esp_cache_msync(Wc.overlay.fg_buf, buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

  wc_build_srm(Wc.overlay.fg_buf, WC_CB_WIDTH, WC_CB_HEIGHT, WC_CB_POS_X, WC_CB_POS_Y);
  return true;
}

// ---------------------------------------------------------------------------
// WcOverlayInitBerry — mode 2: Berry-drawable RGB565 canvas
// ---------------------------------------------------------------------------
static bool WcOverlayInitBerry(uint16_t pos_x, uint16_t pos_y, uint16_t w, uint16_t h) {
  // All dimensions must be even for YUV420
  if ((pos_x | pos_y | w | h) & 1) {
    AddLog(LOG_LEVEL_ERROR, PSTR("CAM: Overlay pos/size must be even numbers"));
    return false;
  }

  // Register SRM client
  ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
  if (ppa_register_client(&ppa_cfg, &Wc.overlay.client) != ESP_OK) return false;

  // Allocate RGB565 Berry canvas — cleared to black
  size_t buf_size = (size_t)w * h * sizeof(uint16_t);
  Wc.overlay.berry_buf = (uint8_t*)wc_alloc(buf_size);
  if (!Wc.overlay.berry_buf) { ppa_unregister_client(Wc.overlay.client); Wc.overlay.client = nullptr; return false; }
  memset(Wc.overlay.berry_buf, 0, buf_size);
  esp_cache_msync(Wc.overlay.berry_buf, buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

  Wc.overlay.berry_buf_size = buf_size;
  Wc.overlay.berry_pos_x    = pos_x;
  Wc.overlay.berry_pos_y    = pos_y;
  Wc.overlay.berry_w        = w;
  Wc.overlay.berry_h        = h;

  wc_build_srm(Wc.overlay.berry_buf, w, h, pos_x, pos_y);
  return true;
}

// ---------------------------------------------------------------------------
// WcOverlayFlush — flush Berry canvas CPU cache -> SPIRAM after Berry writes
// Called by wcoverlayflush command (or Berry: tasmota.cmd("WcOverlayFlush"))
// ---------------------------------------------------------------------------
void WcOverlayFlush(void) {
  if (Wc.overlay.mode == 2 && Wc.overlay.berry_buf) {
    esp_cache_msync(Wc.overlay.berry_buf, Wc.overlay.berry_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  }
}

// ---------------------------------------------------------------------------
// WcApplyOverlay — called every frame from the H264 encode loop
// ---------------------------------------------------------------------------
void WcApplyOverlay(uint8_t *frame_buf, uint16_t width, uint16_t height) {
  if (!frame_buf || !Wc.overlay.enabled) return;

  // Select active source buffer
  void *src = (Wc.overlay.mode == 1) ? (void*)Wc.overlay.fg_buf : (void*)Wc.overlay.berry_buf;
  if (!src) return;

  uint16_t ow = (Wc.overlay.mode == 1) ? WC_CB_WIDTH  : Wc.overlay.berry_w;
  uint16_t oh = (Wc.overlay.mode == 1) ? WC_CB_HEIGHT : Wc.overlay.berry_h;
  uint16_t ox = (Wc.overlay.mode == 1) ? WC_CB_POS_X  : Wc.overlay.berry_pos_x;
  uint16_t oy = (Wc.overlay.mode == 1) ? WC_CB_POS_Y  : Wc.overlay.berry_pos_y;

  if (ox + ow > width || oy + oh > height) return;

  // Update per-frame output fields (frame buffer changes each frame)
  s_srm_cfg.out.buffer      = frame_buf;
  s_srm_cfg.out.buffer_size = (size_t)width * height * 3 / 2;
  s_srm_cfg.out.pic_w       = width;
  s_srm_cfg.out.pic_h       = height;

  esp_err_t err = ppa_do_scale_rotate_mirror(Wc.overlay.client, &s_srm_cfg);
  if (err != ESP_OK) {
    AddLog(LOG_LEVEL_ERROR, PSTR("CAM: PPA SRM overlay failed (%d)"), err);
    return;
  }

  // Invalidate CPU cache so it doesn't hold stale pre-overlay data
  esp_cache_msync(frame_buf, s_srm_cfg.out.buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
}

// ---------------------------------------------------------------------------
// CmndWcOverlay
//   wcoverlay          -> {"WcOverlay":<mode>}
//   wcoverlay 0        -> disable, free buffers
//   wcoverlay 1        -> checkerboard test pattern
//   wcoverlay x,y,w,h  -> Berry RGB565 canvas
// ---------------------------------------------------------------------------
void CmndWcOverlay(void) {
  // Query
  if (XdrvMailbox.payload < 0) {
    ResponseCmndNumber(Wc.overlay.mode);
    return;
  }

  // Disable
  if (XdrvMailbox.payload == 0) {
    WcOverlayDeinit();
    AddLog(LOG_LEVEL_INFO, PSTR("CAM: Overlay disabled"));
    ResponseCmndNumber(0);
    return;
  }

  // Require active YUV420 session for all enable modes
  if (Wc.core.session_type != SESSION_RTSP_AND_WS && Wc.core.session_type != SESSION_WEBRTC) {
    AddLog(LOG_LEVEL_INFO, PSTR("CAM: Overlay only available for YUV420 sessions"));
    ResponseCmndFailed();
    return;
  }

  // Mode 1: checkerboard
  if (XdrvMailbox.payload == 1) {
    WcOverlayDeinit();
    if (!WcOverlayInitCheckerboard()) { ResponseCmndFailed(); return; }
    Wc.overlay.mode    = 1;
    Wc.overlay.enabled = true;
    AddLog(LOG_LEVEL_INFO, PSTR("CAM: Overlay mode 1 (checkerboard) enabled"));
    ResponseCmndNumber(1);
    return;
  }

  // Mode 2: Berry canvas — parse "pos_x,pos_y,width,height"
  // XdrvMailbox.data contains the raw string when payload != a simple integer
  uint16_t px = 0, py = 0, pw = 0, ph = 0;
  if (sscanf(XdrvMailbox.data, "%hu,%hu,%hu,%hu", &px, &py, &pw, &ph) == 4) {
    if (pw == 0 || ph == 0) { ResponseCmndFailed(); return; }

    WcOverlayDeinit();
    if (!WcOverlayInitBerry(px, py, pw, ph)) { ResponseCmndFailed(); return; }

    Wc.overlay.mode    = 2;
    Wc.overlay.enabled = true;
    AddLog(LOG_LEVEL_INFO, PSTR("CAM: Overlay mode 2 (Berry canvas) %dx%d at (%d,%d), addr=0x%08X size=%d"),
      pw, ph, px, py, (uint32_t)Wc.overlay.berry_buf, Wc.overlay.berry_buf_size);

    // Return JSON with address and size for Berry
    Response_P(PSTR("{\"WcOverlay\":2,\"addr\":\"0x%08X\",\"size\":%d}"),
      (uint32_t)Wc.overlay.berry_buf, Wc.overlay.berry_buf_size);
    return;
  }

  ResponseCmndFailed();
}

// ---------------------------------------------------------------------------
// CmndWcOverlayFlush — Berry calls this after drawing to push cache to SPIRAM
// ---------------------------------------------------------------------------
void CmndWcOverlayFlush(void) {
  if (Wc.overlay.mode != 2 || !Wc.overlay.berry_buf) {
    ResponseCmndFailed();
    return;
  }
  WcOverlayFlush();
  ResponseCmndDone();
}

#endif  // USE_CSI_WEBCAM
#endif  // ESP32
