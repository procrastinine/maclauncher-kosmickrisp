// SPDX-License-Identifier: MIT
import fs from 'node:fs';
import path from 'node:path';

export const id = 'metal-display-timing';
export function apply({base, edits}) {
  const edit = (file, change) => {
    let text = edits.get(file) ?? fs.readFileSync(path.join(base, file), 'utf8');
    change((from, to) => {
      if (!text.includes(from)) throw Error(`${id}: missing ${file} anchor ${from.slice(0, 70)}`);
      text = text.replace(from, to);
    });
    edits.set(file, text);
  };
  const wsi = (file, change) => edit('src/vulkan/wsi/' + file, change);
  edit('src/kosmickrisp/vulkan/kk_physical_device.c', r => {
    r('      .KHR_swapchain = true,', '      .KHR_swapchain = true,\n      .GOOGLE_display_timing = true,');
  });
  wsi('meson.build', r => r("if with_platform_macos\n", `if with_platform_macos
  platform_deps += dependency('appleframeworks', modules : ['CoreVideo'])
`));
  wsi('wsi_common_metal_layer.h', r => {
    r('void\nwsi_metal_layer_add_presented_handler', `uint64_t
wsi_metal_layer_refresh_duration(const CAMetalLayer *metal_layer);

void
wsi_metal_layer_add_presented_handler`);
    r('void (*presented_handler)(void *, uint64_t),\n                                      void *present_info_ptr, uint64_t present_id);',
      `void (*presented_handler)(void *, uint64_t, uint64_t, uint32_t, uint64_t),
                                      void *present_info_ptr, uint64_t present_id,
                                      uint64_t timing_serial, uint32_t image_index);`);
    r('uint32_t width, uint32_t height, uint32_t row_pitch);', 'uint32_t width, uint32_t height, uint32_t row_pitch, uint64_t target_time);');
    r('wsi_metal_layer_present(CAMetalDrawable **drawable_ptr);', 'wsi_metal_layer_present(CAMetalDrawable **drawable_ptr, uint64_t target_time);');
  });
  wsi('wsi_common_metal_layer.m', r => {
    r('#import <Metal/Metal.h>', '#import <Metal/Metal.h>\n#import <AppKit/AppKit.h>\n#import <CoreVideo/CoreVideo.h>');
    r('void\nwsi_metal_layer_add_presented_handler', `/* An unattached layer has no known display. Do not invent a refresh rate. */
uint64_t
wsi_metal_layer_refresh_duration(const CAMetalLayer *metal_layer)
{
   @autoreleasepool {
      NSScreen *screen = nil;
      for (CALayer *layer = (CALayer *)metal_layer; layer; layer = layer.superlayer) {
         id delegate = layer.delegate;
         if ([delegate respondsToSelector:@selector(window)]) {
            screen = ((NSView *)delegate).window.screen;
            if (screen)
               break;
         }
      }
      if (!screen)
         return 0;
      CGDirectDisplayID display = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
      CVDisplayLinkRef link = NULL;
      if (CVDisplayLinkCreateWithCGDisplay(display, &link) != kCVReturnSuccess)
         return 0;
      CVTime period = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);
      CVDisplayLinkRelease(link);
      if ((period.flags & kCVTimeIsIndefinite) || period.timeValue <= 0 || period.timeScale <= 0)
         return 0;
      return (uint64_t)((double)period.timeValue * 1e9 / period.timeScale);
   }
}

void
wsi_metal_layer_add_presented_handler`);
    r('void (*presented_handler)(void *, uint64_t),\n                                      void *present_info_ptr, uint64_t present_id)',
      `void (*presented_handler)(void *, uint64_t, uint64_t, uint32_t, uint64_t),
                                      void *present_info_ptr, uint64_t present_id,
                                      uint64_t timing_serial, uint32_t image_index)`);
    r('         presented_handler(present_info_ptr, present_id);', `         /* A callback may also describe a skipped drawable, whose time is zero. */
         uint64_t actual_time = timing_serial ? (uint64_t)(mtlDrwbl.presentedTime * 1e9) : 0;
         presented_handler(present_info_ptr, present_id, timing_serial, image_index, actual_time);`);
    r('uint32_t width, uint32_t height, uint32_t row_pitch)', 'uint32_t width, uint32_t height, uint32_t row_pitch, uint64_t target_time)');
    r('      [commandBuffer presentDrawable:drawable];', `      if (target_time)
         [commandBuffer presentDrawable:drawable atTime:(double)target_time * 1e-9];
      else
         [commandBuffer presentDrawable:drawable];`);
    r('wsi_metal_layer_present(CAMetalDrawable **drawable_ptr)', 'wsi_metal_layer_present(CAMetalDrawable **drawable_ptr, uint64_t target_time)');
    r('      [drawable present];', `      if (target_time)
         [drawable presentAtTime:(double)target_time * 1e-9];
      else
         [drawable present];`);
  });
  wsi('wsi_common_metal.c', r => {
    r('   uint64_t present_id;\n};', `   uint64_t present_id;
   /* Protected by mutex; callbacks can outlive the swapchain. */
   struct wsi_metal_swapchain *chain;
};`);
    r('   uint32_t current_image_index;', '   uint32_t current_image_index;\n   struct wsi_image_timing_request timing_request;');
    r('static void\nwsi_metal_present_complete(void *present_info_ptr, uint64_t present_id)', `static uint64_t
wsi_metal_poll_refresh_duration(struct wsi_swapchain *wsi_chain, uint64_t *interval)
{
   struct wsi_metal_swapchain *chain = (struct wsi_metal_swapchain *)wsi_chain;
   *interval = 0; /* CoreVideo nominal duration does not assert fixed-rate pacing. */
   return wsi_metal_layer_refresh_duration(chain->surface->pLayer);
}

static void
wsi_metal_set_timing_request(struct wsi_swapchain *wsi_chain,
                            const struct wsi_image_timing_request *request)
{
   struct wsi_metal_swapchain *chain = (struct wsi_metal_swapchain *)wsi_chain;
   chain->timing_request = *request;
   if (request->feedback) {
      uint64_t interval;
      uint64_t duration = wsi_metal_poll_refresh_duration(wsi_chain, &interval);
      if (duration)
         wsi_swapchain_present_timing_update_refresh_rate(wsi_chain, duration, interval, 0);
   }
}

static void
wsi_metal_present_complete(void *present_info_ptr, uint64_t present_id,
                           uint64_t timing_serial, uint32_t image_index, uint64_t actual_time)`);
    r('   mtx_lock(&present_info->mutex);\n   if (present_id > present_info->present_id)', `   mtx_lock(&present_info->mutex);
   struct wsi_metal_swapchain *chain = present_info->chain;
   if (chain && timing_serial) {
      if (actual_time) {
         wsi_swapchain_present_timing_notify_completion(&chain->base, timing_serial,
                                                        actual_time, &chain->images[image_index].base);
      } else {
         /* Only displayed images have past presentation timings. Remove skipped
          * drawables so they cannot block delivery of later real feedback. */
         mtx_lock(&chain->base.present_timing.lock);
         for (size_t i = 0; i < chain->base.present_timing.timings_count; i++) {
            if (chain->base.present_timing.timings[i].serial != timing_serial)
               continue;
            memmove(&chain->base.present_timing.timings[i],
                    &chain->base.present_timing.timings[i + 1],
                    (--chain->base.present_timing.timings_count - i) *
                       sizeof(chain->base.present_timing.timings[0]));
            break;
         }
         mtx_unlock(&chain->base.present_timing.lock);
      }
   }
   if (present_id > present_info->present_id)`);
    r('   if (present_id && wsi_chain->present_wait_enabled) {', `   uint64_t timing_serial = chain->timing_request.feedback ? chain->timing_request.serial : 0;
   if ((present_id && wsi_chain->present_wait_enabled) || timing_serial) {`);
    r('         present_id);', '         present_id, timing_serial, image_index);');
    r('         image->base.row_pitches[0]);', '         image->base.row_pitches[0], chain->timing_request.time);');
    r('      wsi_metal_layer_present(&image->drawable);', '      wsi_metal_layer_present(&image->drawable, chain->timing_request.time);');
    r('   for (uint32_t i = 0; i < chain->base.image_count; i++) {\n      struct wsi_metal_image *image', `   /* Exclude callbacks before destroying images, timing history or locks. */
   mtx_lock(&chain->present_info->mutex);
   chain->present_info->chain = NULL;
   mtx_unlock(&chain->present_info->mutex);

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      struct wsi_metal_image *image`);
    r('   if (wsi_device->metal.get_mtl4_command_queue) {', `   chain->present_info->chain = chain;
   chain->base.set_timing_request = wsi_metal_set_timing_request;
   chain->base.poll_early_refresh = wsi_metal_poll_refresh_duration;
   /* CAMetalDrawable times use the Mach host clock in seconds, expressed here
    * as nanoseconds. No scheduled callback time is used as presentation time. */
   chain->base.present_timing.time_domain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR;

   if (wsi_device->metal.get_mtl4_command_queue) {`);
  });
}
