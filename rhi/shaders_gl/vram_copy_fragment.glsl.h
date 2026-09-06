#include "shaders_common.h"

/* VRAM-to-VRAM copy (GP0 80h) for the cases an image copy cannot express.
 *
 * glCopyImageSubData and a framebuffer blit move pixels but cannot look at
 * what they are overwriting, which is exactly what the PS1's mask handling
 * needs: with mask_test set, a destination pixel whose bit 15 is set must be
 * left alone.  Both rectangles are therefore staged side by side in one
 * scratch texture - the source at the origin, the destination copy_dst_offset
 * texels to its right - and this shader composites them.
 *
 * Sampling is by absolute window coordinate rather than by PS1 texel so an
 * upscaled copy carries every texel it had instead of being resampled down to
 * one texel per PS1 pixel.  On the native mirror the two are the same thing.
 *
 * The mask bit is read from alpha, which is where every write path in this
 * renderer puts it - primitives, fills and CPU uploads alike - and is the
 * same test the Vulkan backend's blit_vram shader makes. */
static const char *vram_copy_fragment = GLSL_FRAGMENT(
      uniform sampler2D copy_source;
      /* Added to the fragment's window coordinate to reach the staged
         source: the negated destination origin. */
      uniform ivec2 copy_offset;
      /* Distance from the staged source to the staged destination. */
      uniform int copy_dst_offset;
      uniform uint copy_mask_test;
      uniform uint copy_set_mask;
      out vec4 frag_color;

      void main() {
      ivec2 c = ivec2(gl_FragCoord.xy) + copy_offset;
      vec4 texel = texelFetch(copy_source, c, 0);

      if (copy_mask_test != 0u &&
          texelFetch(copy_source, c + ivec2(copy_dst_offset, 0), 0).a >= 0.5)
         discard;

      if (copy_set_mask != 0u)
         texel.a = 1.0;

      frag_color = texel;
      }
);
