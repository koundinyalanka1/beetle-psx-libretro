/*
 * rhi_defer - implementation. See rhi_defer.h for the rationale and the
 * full description of which operations are deferred (and which are
 * deliberately not).
 *
 * The queue is a flat array of tagged op structs. Growth is geometric
 * (capacity doubles on overflow, starting at 16). Realistically the
 * queue never holds more than a few dozen entries during the
 * SET_HW_RENDER -> context_reset window, so the doubling cap is plenty
 * and we don't bother with a free-list or any reuse logic.
 */

#include "rhi_defer.h"

#include <stdlib.h>
#include <string.h>

/* Initial capacity allocated on first push. Chosen to comfortably hold
 * the steady-state pre-context_reset traffic (a handful of state sets
 * plus the initial GPU_RestoreStateP3() VRAM upload at most) without an
 * immediate regrow. */
#define RHI_DEFER_INITIAL_CAP 16

/*
 * Ensure the queue has room for at least one more op.
 * Returns true on success; false on allocation failure (in which case
 * the caller must drop the push - we deliberately don't abort).
 */
static bool rhi_defer_reserve_one(rhi_defer_queue_t *q)
{
   size_t          new_cap;
   rhi_defer_op_t *new_ops;

   if (q->count < q->capacity)
      return true;

   new_cap = (q->capacity == 0) ? RHI_DEFER_INITIAL_CAP : (q->capacity * 2);
   new_ops = (rhi_defer_op_t *)realloc(q->ops, new_cap * sizeof(*new_ops));
   if (!new_ops)
      return false;

   q->ops      = new_ops;
   q->capacity = new_cap;
   return true;
}

/* Append-and-zero. Returns a pointer to the new (uninitialised) slot or
 * NULL on OOM. The slot is bumped into `count` only on success so the
 * queue stays consistent on failure. */
static rhi_defer_op_t *rhi_defer_alloc_slot(rhi_defer_queue_t *q,
                                            rhi_defer_kind_t kind)
{
   rhi_defer_op_t *slot;

   if (!q)
      return NULL;
   if (!rhi_defer_reserve_one(q))
      return NULL;

   slot = &q->ops[q->count++];
   /* Zero the union so any unused fields are deterministic - cheap and
    * keeps valgrind/MSan happy if the dispatcher ever reads through the
    * wrong arm of the union by accident. */
   memset(slot, 0, sizeof(*slot));
   slot->kind = kind;
   return slot;
}

void rhi_defer_clear(rhi_defer_queue_t *q)
{
   if (!q)
      return;
   free(q->ops);
   q->ops      = NULL;
   q->count    = 0;
   q->capacity = 0;
   free(q->pixels);
   q->pixels         = NULL;
   q->pixel_count    = 0;
   q->pixel_capacity = 0;
}

/* Reserve `n` uint16 in the pixel arena and return their offset, or SIZE_MAX
 * on failure.  The arena is only ever appended to and reset wholesale, so an
 * offset stays valid for the life of a recording pass. */
static size_t rhi_defer_pixels_alloc(rhi_defer_queue_t *q, size_t n)
{
   size_t offset = q->pixel_count;

   if (n == 0)
      return offset;

   if (q->pixel_count + n > q->pixel_capacity)
   {
      size_t    new_cap = q->pixel_capacity ? q->pixel_capacity : 4096;
      uint16_t *grown;

      while (new_cap < q->pixel_count + n)
         new_cap *= 2;

      grown = (uint16_t *)realloc(q->pixels, new_cap * sizeof(*grown));
      if (!grown)
         return (size_t)-1;

      q->pixels         = grown;
      q->pixel_capacity = new_cap;
   }

   q->pixel_count += n;
   return offset;
}

void rhi_defer_stage_load_image(const rhi_defer_op_t *op,
                                const uint16_t *packed,
                                uint16_t *dst,
                                size_t stride, size_t height)
{
   size_t row;
   size_t x, y, w, h, first;

   if (!op || !packed || !dst)
      return;

   x = op->u.load_image.x; y = op->u.load_image.y;
   w = op->u.load_image.w; h = op->u.load_image.h;
   if (w == 0 || h == 0 || w > stride || h > height)
      return;

   /* Exactly the inverse of the snapshot loop, so the rect reads back the way
    * it was recorded. */
   first = (x + w > stride) ? stride - x : w;
   for (row = 0; row < h; row++)
   {
      const uint16_t *src_row = packed + row * w;
      uint16_t       *dst_row = dst + ((y + row) % height) * stride;

      memcpy(dst_row + x, src_row, first * sizeof(uint16_t));
      if (first < w)
         memcpy(dst_row, src_row + first, (w - first) * sizeof(uint16_t));
   }
}

const uint16_t *rhi_defer_pixels(const rhi_defer_queue_t *q, size_t offset)
{
   if (!q || !q->pixels || offset >= q->pixel_capacity)
      return NULL;
   return q->pixels + offset;
}

bool rhi_defer_push_load_image_snapshot(rhi_defer_queue_t *q,
                                        uint16_t x, uint16_t y,
                                        uint16_t w, uint16_t h,
                                        const uint16_t *src,
                                        size_t src_stride,
                                        size_t src_height,
                                        bool mask_test, bool set_mask)
{
   rhi_defer_op_t *op;
   size_t          offset;
   size_t          row;
   size_t          first;

   if (!q || !src || w == 0 || h == 0)
      return false;
   if ((size_t)w > src_stride || (size_t)h > src_height)
      return false;

   /* Reserve the pixels first: a failure here must not leave a queued op
    * pointing at an arena slot that was never filled. */
   offset = rhi_defer_pixels_alloc(q, (size_t)w * (size_t)h);
   if (offset == (size_t)-1)
      return false;

   op = rhi_defer_alloc_slot(q, RHI_DEFER_LOAD_IMAGE);
   if (!op)
      return false;

   /* Wrap in x and y, matching what the backends' upload paths do when a rect
    * runs off the edge of VRAM. */
   first = ((size_t)x + w > src_stride) ? src_stride - (size_t)x : (size_t)w;
   for (row = 0; row < (size_t)h; row++)
   {
      const uint16_t *src_row = src + (((size_t)y + row) % src_height) * src_stride;
      uint16_t       *dst_row = q->pixels + offset + row * (size_t)w;

      memcpy(dst_row, src_row + x, first * sizeof(uint16_t));
      if (first < (size_t)w)
         memcpy(dst_row + first, src_row, ((size_t)w - first) * sizeof(uint16_t));
   }

   op->u.load_image.x            = x;
   op->u.load_image.y            = y;
   op->u.load_image.w            = w;
   op->u.load_image.h            = h;
   op->u.load_image.vram         = NULL;
   op->u.load_image.pixel_offset = offset;
   op->u.load_image.has_pixels   = true;
   op->u.load_image.mask_test    = mask_test;
   op->u.load_image.set_mask     = set_mask;
   return true;
}

size_t rhi_defer_count(const rhi_defer_queue_t *q)
{
   return q ? q->count : 0;
}

void rhi_defer_drain(rhi_defer_queue_t *q,
                     rhi_defer_dispatch_fn dispatch,
                     void *user)
{
   size_t i;
   size_t n;

   if (!q || !dispatch)
      return;

   /* Snapshot the count so we replay exactly what was queued at entry.
    * If a dispatcher implementation accidentally re-enters the entry
    * point that pushes onto this same queue (it shouldn't - the renderer
    * is up by drain time), any further pushes would land past `n` and
    * get silently leaked when we clear() below. That's preferable to
    * looping forever, and the comment in the header tells callers not
    * to do this. */
   n = q->count;
   for (i = 0; i < n; ++i)
      dispatch(user, &q->ops[i]);

   /* Reset to empty but keep the backing storage around if it's small;
    * if it's grown large from an unusual workload, free it so we don't
    * hold the high-water mark forever. */
   if (!q->keep_storage && q->capacity > RHI_DEFER_INITIAL_CAP * 4)
   {
      rhi_defer_clear(q);
   }
   else
   {
      q->count       = 0;
      q->pixel_count = 0;
   }
}

void rhi_defer_set_keep_storage(rhi_defer_queue_t *q, bool keep)
{
   if (q)
      q->keep_storage = keep;
}

/* ---- per-op push helpers --------------------------------------------- */

void rhi_defer_push_set_tex_window(rhi_defer_queue_t *q,
                                   uint8_t tww, uint8_t twh,
                                   uint8_t twx, uint8_t twy)
{
   rhi_defer_op_t *op = rhi_defer_alloc_slot(q, RHI_DEFER_SET_TEX_WINDOW);
   if (!op)
      return;
   op->u.set_tex_window.tww = tww;
   op->u.set_tex_window.twh = twh;
   op->u.set_tex_window.twx = twx;
   op->u.set_tex_window.twy = twy;
}

void rhi_defer_push_set_draw_offset(rhi_defer_queue_t *q,
                                    int16_t x, int16_t y)
{
   rhi_defer_op_t *op = rhi_defer_alloc_slot(q, RHI_DEFER_SET_DRAW_OFFSET);
   if (!op)
      return;
   op->u.set_draw_offset.x = x;
   op->u.set_draw_offset.y = y;
}

void rhi_defer_push_set_draw_area(rhi_defer_queue_t *q,
                                  uint16_t x0, uint16_t y0,
                                  uint16_t x1, uint16_t y1)
{
   rhi_defer_op_t *op = rhi_defer_alloc_slot(q, RHI_DEFER_SET_DRAW_AREA);
   if (!op)
      return;
   op->u.set_draw_area.x0 = x0;
   op->u.set_draw_area.y0 = y0;
   op->u.set_draw_area.x1 = x1;
   op->u.set_draw_area.y1 = y1;
}

void rhi_defer_push_set_vram_framebuffer_coords(rhi_defer_queue_t *q,
                                                uint32_t xstart,
                                                uint32_t ystart)
{
   rhi_defer_op_t *op = rhi_defer_alloc_slot(q,
         RHI_DEFER_SET_VRAM_FRAMEBUFFER_COORDS);
   if (!op)
      return;
   op->u.set_vram_framebuffer_coords.xstart = xstart;
   op->u.set_vram_framebuffer_coords.ystart = ystart;
}

void rhi_defer_push_set_horizontal_display_range(rhi_defer_queue_t *q,
                                                 uint16_t x1, uint16_t x2)
{
   rhi_defer_op_t *op = rhi_defer_alloc_slot(q,
         RHI_DEFER_SET_HORIZONTAL_DISPLAY_RANGE);
   if (!op)
      return;
   op->u.set_horizontal_display_range.x1 = x1;
   op->u.set_horizontal_display_range.x2 = x2;
}

void rhi_defer_push_set_vertical_display_range(rhi_defer_queue_t *q,
                                               uint16_t y1, uint16_t y2)
{
   rhi_defer_op_t *op = rhi_defer_alloc_slot(q,
         RHI_DEFER_SET_VERTICAL_DISPLAY_RANGE);
   if (!op)
      return;
   op->u.set_vertical_display_range.y1 = y1;
   op->u.set_vertical_display_range.y2 = y2;
}

void rhi_defer_push_set_display_mode(rhi_defer_queue_t *q,
                                     bool depth_24bpp,
                                     bool is_pal,
                                     bool is_480i,
                                     int  width_mode)
{
   rhi_defer_op_t *op = rhi_defer_alloc_slot(q,
         RHI_DEFER_SET_DISPLAY_MODE);
   if (!op)
      return;
   op->u.set_display_mode.depth_24bpp = depth_24bpp;
   op->u.set_display_mode.is_pal      = is_pal;
   op->u.set_display_mode.is_480i     = is_480i;
   op->u.set_display_mode.width_mode  = width_mode;
}

void rhi_defer_push_load_image(rhi_defer_queue_t *q,
                               uint16_t x, uint16_t y,
                               uint16_t w, uint16_t h,
                               uint16_t *vram,
                               bool mask_test, bool set_mask)
{
   rhi_defer_op_t *op = rhi_defer_alloc_slot(q, RHI_DEFER_LOAD_IMAGE);
   if (!op)
      return;
   op->u.load_image.x         = x;
   op->u.load_image.y         = y;
   op->u.load_image.w         = w;
   op->u.load_image.h         = h;
   op->u.load_image.vram      = vram;
   op->u.load_image.mask_test = mask_test;
   op->u.load_image.set_mask  = set_mask;
}

static void rhi_defer_copy_vertex_attrs(rhi_defer_op_t *op,
                                        unsigned nverts,
                                        const float *precise_rgb,
                                        const float *fog)
{
   unsigned i;

   op->u.push_poly.nverts          = (uint8_t)nverts;
   op->u.push_poly.has_precise_rgb = (precise_rgb != NULL);
   op->u.push_poly.has_fog         = (fog != NULL);

   /* Deep copy: both point at arrays local to the caller's frame. */
   if (precise_rgb)
      for (i = 0; i < nverts * 3; i++)
         op->u.push_poly.precise_rgb[i] = precise_rgb[i];

   if (fog)
      for (i = 0; i < nverts * 4; i++)
         op->u.push_poly.fog[i] = fog[i];
}

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
      bool dither, int blend_mode, bool mask_test, bool set_mask)
{
   rhi_defer_op_t *op = rhi_defer_alloc_slot(q, RHI_DEFER_PUSH_TRIANGLE);
   if (!op)
      return;

   op->u.push_poly.px[0] = p0x; op->u.push_poly.py[0] = p0y; op->u.push_poly.pw[0] = p0w;
   op->u.push_poly.px[1] = p1x; op->u.push_poly.py[1] = p1y; op->u.push_poly.pw[1] = p1w;
   op->u.push_poly.px[2] = p2x; op->u.push_poly.py[2] = p2y; op->u.push_poly.pw[2] = p2w;
   op->u.push_poly.c[0] = c0; op->u.push_poly.c[1] = c1; op->u.push_poly.c[2] = c2;
   op->u.push_poly.tx[0] = t0x; op->u.push_poly.ty[0] = t0y;
   op->u.push_poly.tx[1] = t1x; op->u.push_poly.ty[1] = t1y;
   op->u.push_poly.tx[2] = t2x; op->u.push_poly.ty[2] = t2y;
   op->u.push_poly.min_u = min_u; op->u.push_poly.min_v = min_v;
   op->u.push_poly.max_u = max_u; op->u.push_poly.max_v = max_v;
   op->u.push_poly.texpage_x = texpage_x; op->u.push_poly.texpage_y = texpage_y;
   op->u.push_poly.clut_x = clut_x; op->u.push_poly.clut_y = clut_y;
   op->u.push_poly.texture_blend_mode = texture_blend_mode;
   op->u.push_poly.depth_shift = depth_shift;
   op->u.push_poly.dither = dither;
   op->u.push_poly.blend_mode = blend_mode;
   op->u.push_poly.mask_test = mask_test;
   op->u.push_poly.set_mask = set_mask;

   rhi_defer_copy_vertex_attrs(op, 3, precise_rgb, fog);
}

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
      bool is_sprite, bool may_be_2d)
{
   rhi_defer_op_t *op = rhi_defer_alloc_slot(q, RHI_DEFER_PUSH_QUAD);
   if (!op)
      return;

   op->u.push_poly.px[0] = p0x; op->u.push_poly.py[0] = p0y; op->u.push_poly.pw[0] = p0w;
   op->u.push_poly.px[1] = p1x; op->u.push_poly.py[1] = p1y; op->u.push_poly.pw[1] = p1w;
   op->u.push_poly.px[2] = p2x; op->u.push_poly.py[2] = p2y; op->u.push_poly.pw[2] = p2w;
   op->u.push_poly.px[3] = p3x; op->u.push_poly.py[3] = p3y; op->u.push_poly.pw[3] = p3w;
   op->u.push_poly.c[0] = c0; op->u.push_poly.c[1] = c1;
   op->u.push_poly.c[2] = c2; op->u.push_poly.c[3] = c3;
   op->u.push_poly.tx[0] = t0x; op->u.push_poly.ty[0] = t0y;
   op->u.push_poly.tx[1] = t1x; op->u.push_poly.ty[1] = t1y;
   op->u.push_poly.tx[2] = t2x; op->u.push_poly.ty[2] = t2y;
   op->u.push_poly.tx[3] = t3x; op->u.push_poly.ty[3] = t3y;
   op->u.push_poly.min_u = min_u; op->u.push_poly.min_v = min_v;
   op->u.push_poly.max_u = max_u; op->u.push_poly.max_v = max_v;
   op->u.push_poly.texpage_x = texpage_x; op->u.push_poly.texpage_y = texpage_y;
   op->u.push_poly.clut_x = clut_x; op->u.push_poly.clut_y = clut_y;
   op->u.push_poly.texture_blend_mode = texture_blend_mode;
   op->u.push_poly.depth_shift = depth_shift;
   op->u.push_poly.dither = dither;
   op->u.push_poly.blend_mode = blend_mode;
   op->u.push_poly.mask_test = mask_test;
   op->u.push_poly.set_mask = set_mask;
   op->u.push_poly.is_sprite = is_sprite;
   op->u.push_poly.may_be_2d = may_be_2d;

   rhi_defer_copy_vertex_attrs(op, 4, precise_rgb, fog);
}

void rhi_defer_push_line(rhi_defer_queue_t *q,
      int16_t p0x, int16_t p0y, int16_t p1x, int16_t p1y,
      uint32_t c0, uint32_t c1,
      bool dither, int blend_mode, bool mask_test, bool set_mask)
{
   rhi_defer_op_t *op = rhi_defer_alloc_slot(q, RHI_DEFER_PUSH_LINE);
   if (!op)
      return;
   op->u.push_line.p0x = p0x; op->u.push_line.p0y = p0y;
   op->u.push_line.p1x = p1x; op->u.push_line.p1y = p1y;
   op->u.push_line.c0 = c0;   op->u.push_line.c1 = c1;
   op->u.push_line.dither = dither;
   op->u.push_line.blend_mode = blend_mode;
   op->u.push_line.mask_test = mask_test;
   op->u.push_line.set_mask = set_mask;
}

void rhi_defer_push_fill_rect(rhi_defer_queue_t *q,
      uint32_t color, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
   rhi_defer_op_t *op = rhi_defer_alloc_slot(q, RHI_DEFER_FILL_RECT);
   if (!op)
      return;
   op->u.fill_rect.color = color;
   op->u.fill_rect.x = x; op->u.fill_rect.y = y;
   op->u.fill_rect.w = w; op->u.fill_rect.h = h;
}

void rhi_defer_push_copy_rect(rhi_defer_queue_t *q,
      uint16_t src_x, uint16_t src_y,
      uint16_t dst_x, uint16_t dst_y,
      uint16_t w, uint16_t h,
      bool mask_test, bool set_mask)
{
   rhi_defer_op_t *op = rhi_defer_alloc_slot(q, RHI_DEFER_COPY_RECT);
   if (!op)
      return;
   op->u.copy_rect.src_x = src_x; op->u.copy_rect.src_y = src_y;
   op->u.copy_rect.dst_x = dst_x; op->u.copy_rect.dst_y = dst_y;
   op->u.copy_rect.w = w; op->u.copy_rect.h = h;
   op->u.copy_rect.mask_test = mask_test;
   op->u.copy_rect.set_mask = set_mask;
}

void rhi_defer_push_toggle_display(rhi_defer_queue_t *q, bool status)
{
   rhi_defer_op_t *op = rhi_defer_alloc_slot(q, RHI_DEFER_TOGGLE_DISPLAY);
   if (!op)
      return;
   op->u.toggle_display.status = status;
}
