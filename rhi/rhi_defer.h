#ifndef __RHI_DEFER_H__
#define __RHI_DEFER_H__

/*
 * rhi_defer
 * ---------
 * A small FIFO queue for RHI-backend operations that arrive before the
 * hardware renderer (GL or Vulkan) has finished initialising.
 *
 * Background. The libretro frontend registers a `context_reset` callback
 * via RETRO_ENVIRONMENT_SET_HW_RENDER and is allowed to invoke it at any
 * point after `retro_load_game` returns - in particular it may invoke the
 * core's GPU side (push primitives, upload VRAM, set state) *before* the
 * first context_reset fires. Without buffering, every `rhi_*_*` entry
 * point either had to no-op (silently dropping side-effects) or risk
 * crashing on a NULL renderer pointer.
 *
 * Both backends previously chose "silently drop". The Vulkan backend later
 * gained an inline std::vector<std::function<void()>> defer queue, but the
 * GL backend (a C TU) never got an equivalent and continued to drop. This
 * caused real symptoms on at least King's Field with the GL backend: the
 * HUD glyph VRAM uploads delivered between SET_HW_RENDER and the first
 * context_reset were lost, and the glyphs only appeared after a savestate
 * load (which replays the full 1MiB VRAM blob via GPU_RestoreStateP3()).
 *
 * This module provides a single C-callable defer mechanism that both
 * backends share. Each backend maintains its own queue instance (via
 * rhi_defer_queue_t), pushes the operations it wants to replay during the
 * pre-renderer window, and drains the queue at the end of its own
 * context_reset once the renderer is up.
 *
 * Policy. We defer the same set of operations the Vulkan backend already
 * deferred:
 *   - state setters (tex window, draw offset, draw area, display ranges,
 *     display mode, vram framebuffer coords)
 *   - load_image (VRAM upload from CPU)
 *   - toggle_display
 * We do NOT defer push_triangle/push_quad/push_line, fill_rect, copy_rect,
 * or read_vram. Pre-context geometry has nowhere to draw to and matches
 * the existing Vulkan policy of dropping it; read_vram needs a synchronous
 * answer that an empty renderer cannot give.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tag identifying which deferred operation a queue entry represents. */
typedef enum
{
   RHI_DEFER_SET_TEX_WINDOW = 0,
   RHI_DEFER_SET_DRAW_OFFSET,
   RHI_DEFER_SET_DRAW_AREA,
   RHI_DEFER_SET_VRAM_FRAMEBUFFER_COORDS,
   RHI_DEFER_SET_HORIZONTAL_DISPLAY_RANGE,
   RHI_DEFER_SET_VERTICAL_DISPLAY_RANGE,
   RHI_DEFER_SET_DISPLAY_MODE,
   RHI_DEFER_LOAD_IMAGE,
   RHI_DEFER_TOGGLE_DISPLAY,

   /* Drawing.  Originally excluded on purpose (see the note above): with no
    * renderer there is nowhere to draw to, so pre-context geometry was
    * dropped.  They exist now for a second user of this queue - the threaded
    * GPU under a backend whose API is thread-affine, where the worker records
    * drawing it may not issue itself and the emulation thread replays it at
    * end of frame.  The context-down case still drops them, by clearing the
    * queue rather than by refusing to record. */
   RHI_DEFER_PUSH_TRIANGLE,
   RHI_DEFER_PUSH_QUAD,
   RHI_DEFER_PUSH_LINE,
   RHI_DEFER_FILL_RECT,
   RHI_DEFER_COPY_RECT
} rhi_defer_kind_t;

/* Vertex attributes carried by a recorded triangle/quad.  precise_rgb is
 * 3 floats per vertex and fog 4 per vertex, matching how the backends index
 * the caller's arrays (precise_rgb[v*3+c], fog[v*4+c]); both are optional at
 * the call site, so presence is recorded explicitly rather than inferred. */
#define RHI_DEFER_MAX_VERTS 4

/*
 * A single deferred operation. The struct stores the raw arguments the
 * caller would have passed to the corresponding rhi_*_<op> function; the
 * drain routine replays them by calling the backend-supplied callback
 * with the same arguments. We capture the *raw* inputs (tww/twh/twx/twy
 * etc.) rather than any pre-computed values so a single struct shape
 * works across both backends and the per-backend computation logic stays
 * in the entry point.
 *
 * For RHI_DEFER_LOAD_IMAGE the `vram` pointer captured here aliases
 * GPU.vram (the long-lived host-side mirror in mednafen/psx/gpu.c).
 * That pointer is allocated once at startup and torn down only on
 * `Cleanup()`; both lifetimes are strictly outside the
 * SET_HW_RENDER -> context_reset window we're buffering across, so
 * capturing the pointer (rather than memcpying a 1024 * height slice)
 * is safe and matches what the prior C++ Vulkan defer did.
 */
typedef struct
{
   rhi_defer_kind_t kind;
   union
   {
      struct
      {
         uint8_t tww;
         uint8_t twh;
         uint8_t twx;
         uint8_t twy;
      } set_tex_window;

      struct
      {
         int16_t x;
         int16_t y;
      } set_draw_offset;

      struct
      {
         uint16_t x0;
         uint16_t y0;
         uint16_t x1;
         uint16_t y1;
      } set_draw_area;

      struct
      {
         uint32_t xstart;
         uint32_t ystart;
      } set_vram_framebuffer_coords;

      struct
      {
         uint16_t x1;
         uint16_t x2;
      } set_horizontal_display_range;

      struct
      {
         uint16_t y1;
         uint16_t y2;
      } set_vertical_display_range;

      struct
      {
         bool depth_24bpp;
         bool is_pal;
         bool is_480i;
         int  width_mode;
      } set_display_mode;

      struct
      {
         uint16_t  x;
         uint16_t  y;
         uint16_t  w;
         uint16_t  h;
         /* Live pointer into the caller's VRAM mirror.  Only meaningful when
          * has_pixels is false. */
         uint16_t *vram;
         /* Offset into the queue's pixel arena of a w*h tightly-packed copy
          * taken at record time.  Required whenever the queue may be replayed
          * after the source has moved on - i.e. always, for a per-frame
          * journal, where a later write to the same VRAM rect would otherwise
          * make this upload carry the wrong pixels. */
         size_t    pixel_offset;
         bool      has_pixels;
         bool      mask_test;
         bool      set_mask;
      } load_image;

      struct
      {
         bool status;
      } toggle_display;

      /* Shared by PUSH_TRIANGLE (nverts 3) and PUSH_QUAD (nverts 4). */
      struct
      {
         float    px[RHI_DEFER_MAX_VERTS];
         float    py[RHI_DEFER_MAX_VERTS];
         float    pw[RHI_DEFER_MAX_VERTS];
         uint32_t c[RHI_DEFER_MAX_VERTS];
         float    precise_rgb[RHI_DEFER_MAX_VERTS * 3];
         float    fog[RHI_DEFER_MAX_VERTS * 4];
         uint16_t tx[RHI_DEFER_MAX_VERTS];
         uint16_t ty[RHI_DEFER_MAX_VERTS];
         uint16_t min_u, min_v, max_u, max_v;
         uint16_t texpage_x, texpage_y;
         uint16_t clut_x, clut_y;
         int      blend_mode;
         uint8_t  nverts;
         uint8_t  texture_blend_mode;
         uint8_t  depth_shift;
         bool     has_precise_rgb;
         bool     has_fog;
         bool     dither;
         bool     mask_test;
         bool     set_mask;
         bool     is_sprite;   /* quad only */
         bool     may_be_2d;   /* quad only */
      } push_poly;

      struct
      {
         int16_t  p0x, p0y, p1x, p1y;
         uint32_t c0, c1;
         int      blend_mode;
         bool     dither;
         bool     mask_test;
         bool     set_mask;
      } push_line;

      struct
      {
         uint32_t color;
         uint16_t x, y, w, h;
      } fill_rect;

      struct
      {
         uint16_t src_x, src_y, dst_x, dst_y, w, h;
         bool     mask_test;
         bool     set_mask;
      } copy_rect;
   } u;
} rhi_defer_op_t;

/*
 * Queue handle. Opaque to callers; the fields below are exposed only
 * because both files need to embed an instance as a static. Callers must
 * not touch these fields directly - use the API.
 */
typedef struct
{
   rhi_defer_op_t *ops;       /* heap-allocated, grown on demand           */
   size_t          count;     /* number of valid entries                   */
   size_t          capacity;  /* allocated slots                           */
   /* Packed pixel payloads for recorded VRAM uploads.  Ops reference this by
    * offset rather than by pointer so growth can realloc it freely, and it is
    * reset (not freed) alongside the op array. */
   uint16_t       *pixels;
   size_t          pixel_count;
   size_t          pixel_capacity;
   /* Normally a drain that grew the queue well past its initial size gives
    * the memory back, since the context-down burst it was sized for happens
    * once.  A queue used as a per-frame journal refills to the same high-water
    * mark every frame, and handing it back would mean a free plus a run of
    * doubling reallocs every single frame - far more work than the recording
    * saves.  Set this while that is the usage. */
   bool            keep_storage;
} rhi_defer_queue_t;

/*
 * Backend-supplied dispatcher invoked once per queued op during a drain.
 * Receives an opaque user pointer (typically NULL; the backend already
 * has all renderer state in its own statics) plus a const pointer to the
 * op being replayed. Implementation should switch on op->kind and call
 * the appropriate rhi_<backend>_<op>(...) function with op->u.<op>.*
 * fields. Returning has no effect; errors are the dispatcher's problem.
 */
typedef void (*rhi_defer_dispatch_fn)(void *user, const rhi_defer_op_t *op);

/* Discard every queued op and free the backing storage. Safe on an
 * empty / never-used queue. After this the queue is left in a usable
 * empty state; subsequent rhi_defer_push_* calls will reallocate. */
void rhi_defer_clear(rhi_defer_queue_t *q);

/* Number of currently queued ops. Cheap. */
size_t rhi_defer_count(const rhi_defer_queue_t *q);

/* Keep the backing allocation across drains (see keep_storage above).
 * Clearing it does not free anything by itself; the next drain will. */
void rhi_defer_set_keep_storage(rhi_defer_queue_t *q, bool keep);

/*
 * Drain the queue in FIFO order, invoking `dispatch(user, op)` once per
 * entry, then clear. The dispatcher is allowed to call back into the
 * same rhi_<backend>_* entry point that originally queued the op - by
 * the time drain runs the renderer is up, so the entry point's
 * "renderer present" branch will execute and actually perform the work.
 * It is *not* safe for the dispatcher to push new ops onto the same
 * queue during drain (we snapshot count up-front but reuse storage).
 */
void rhi_defer_drain(rhi_defer_queue_t *q,
                     rhi_defer_dispatch_fn dispatch,
                     void *user);

/* Per-op push helpers. Each appends a single tagged entry. They allocate
 * on first use and grow geometrically; an OOM during growth drops the
 * push (logged once per process) rather than aborting, since losing a
 * deferred state-set is preferable to crashing the frontend. */
void rhi_defer_push_set_tex_window(rhi_defer_queue_t *q,
                                   uint8_t tww, uint8_t twh,
                                   uint8_t twx, uint8_t twy);

void rhi_defer_push_set_draw_offset(rhi_defer_queue_t *q,
                                    int16_t x, int16_t y);

void rhi_defer_push_set_draw_area(rhi_defer_queue_t *q,
                                  uint16_t x0, uint16_t y0,
                                  uint16_t x1, uint16_t y1);

void rhi_defer_push_set_vram_framebuffer_coords(rhi_defer_queue_t *q,
                                                uint32_t xstart,
                                                uint32_t ystart);

void rhi_defer_push_set_horizontal_display_range(rhi_defer_queue_t *q,
                                                 uint16_t x1, uint16_t x2);

void rhi_defer_push_set_vertical_display_range(rhi_defer_queue_t *q,
                                               uint16_t y1, uint16_t y2);

void rhi_defer_push_set_display_mode(rhi_defer_queue_t *q,
                                     bool depth_24bpp,
                                     bool is_pal,
                                     bool is_480i,
                                     int  width_mode);

void rhi_defer_push_load_image(rhi_defer_queue_t *q,
                               uint16_t x, uint16_t y,
                               uint16_t w, uint16_t h,
                               uint16_t *vram,
                               bool mask_test, bool set_mask);

/* As above, but copies the w*h rect out of `src` (read at `src_stride`) into
 * the queue's pixel arena, so the upload replays the pixels as they were when
 * it was recorded rather than whatever is in VRAM at drain time.  Returns
 * false if the copy could not be made, in which case nothing was queued. */
bool rhi_defer_push_load_image_snapshot(rhi_defer_queue_t *q,
                                        uint16_t x, uint16_t y,
                                        uint16_t w, uint16_t h,
                                        const uint16_t *src,
                                        size_t src_stride,
                                        size_t src_height,
                                        bool mask_test, bool set_mask);

/* Reproduce a recorded snapshot into `dst` (a full `stride` x `height` VRAM
 * image) at the rect's own coordinates, wrapping in x and y exactly as the
 * backends' upload paths do, so a subsequent read of that rect sees what was
 * recorded.  Only the rect is written. */
void rhi_defer_stage_load_image(const rhi_defer_op_t *op,
                                const uint16_t *packed,
                                uint16_t *dst,
                                size_t stride, size_t height);

/* Base of the pixel arena, for a dispatcher unpacking a snapshot. */
const uint16_t *rhi_defer_pixels(const rhi_defer_queue_t *q, size_t offset);

void rhi_defer_push_toggle_display(rhi_defer_queue_t *q, bool status);

/* Drawing.  `precise_rgb` and `fog` may be NULL; when present they are copied
 * (nverts*3 and nverts*4 floats respectively), never aliased - they point at
 * caller stack arrays that are gone by the time the queue drains. */
void rhi_defer_push_triangle(rhi_defer_queue_t *q,
      float p0x, float p0y, float p0w,
      float p1x, float p1y, float p1w,
      float p2x, float p2y, float p2w,
      uint32_t c0, uint32_t c1, uint32_t c2,
      const float *precise_rgb, const float *fog,
      uint16_t t0x, uint16_t t0y,
      uint16_t t1x, uint16_t t1y,
      uint16_t t2x, uint16_t t2y,
      uint16_t min_u, uint16_t min_v,
      uint16_t max_u, uint16_t max_v,
      uint16_t texpage_x, uint16_t texpage_y,
      uint16_t clut_x, uint16_t clut_y,
      uint8_t texture_blend_mode, uint8_t depth_shift,
      bool dither, int blend_mode, bool mask_test, bool set_mask);

void rhi_defer_push_quad(rhi_defer_queue_t *q,
      float p0x, float p0y, float p0w,
      float p1x, float p1y, float p1w,
      float p2x, float p2y, float p2w,
      float p3x, float p3y, float p3w,
      uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3,
      const float *precise_rgb, const float *fog,
      uint16_t t0x, uint16_t t0y,
      uint16_t t1x, uint16_t t1y,
      uint16_t t2x, uint16_t t2y,
      uint16_t t3x, uint16_t t3y,
      uint16_t min_u, uint16_t min_v,
      uint16_t max_u, uint16_t max_v,
      uint16_t texpage_x, uint16_t texpage_y,
      uint16_t clut_x, uint16_t clut_y,
      uint8_t texture_blend_mode, uint8_t depth_shift,
      bool dither, int blend_mode, bool mask_test, bool set_mask,
      bool is_sprite, bool may_be_2d);

void rhi_defer_push_line(rhi_defer_queue_t *q,
      int16_t p0x, int16_t p0y, int16_t p1x, int16_t p1y,
      uint32_t c0, uint32_t c1,
      bool dither, int blend_mode, bool mask_test, bool set_mask);

void rhi_defer_push_fill_rect(rhi_defer_queue_t *q,
      uint32_t color, uint16_t x, uint16_t y, uint16_t w, uint16_t h);

void rhi_defer_push_copy_rect(rhi_defer_queue_t *q,
      uint16_t src_x, uint16_t src_y,
      uint16_t dst_x, uint16_t dst_y,
      uint16_t w, uint16_t h,
      bool mask_test, bool set_mask);

#ifdef __cplusplus
}
#endif

#endif /* __RHI_DEFER_H__ */
