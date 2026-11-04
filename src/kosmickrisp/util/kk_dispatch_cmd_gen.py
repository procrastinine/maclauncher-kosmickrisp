# coding=utf-8
COPYRIGHT = """\
/*
 * Copyright 2020 Intel Corporation
 * Copyright 2025 LunarG, Inc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
"""

import argparse
import os
import sys

from mako.template import Template

# Mesa-local imports must be declared in meson variable
# '{file_without_suffix}_depend_files'.
from vk_entrypoints import get_entrypoints_from_xml

TEMPLATE_H = Template(COPYRIGHT + """\
/* This file generated from ${filename}, don't edit directly. */

#ifndef KK_DISPATCH_CMD_H
#define KK_DISPATCH_CMD_H

#include "vk_dispatch_table.h"

#ifdef __cplusplus
extern "C" {
#endif

extern struct vk_device_dispatch_table kk_device_cmd_trampolines;
extern const struct vk_device_dispatch_table kk_cmd_enqueue_dispatch;

#ifdef __cplusplus
}
#endif

#endif /* KK_DISPATCH_CMD_H */
""")

TEMPLATE_C = Template(COPYRIGHT + """\
/* This file generated from ${filename}, don't edit directly. */

#include "kk_device.h"
#include "kk_cmd_buffer.h"
#include "kk_perf.h"
#include "kk_query_pool.h"
#include "kk_dispatch_cmd.h"
#include "kk_temporal.h"
#define VK_ENTRY_USE_WEAK 1
#include "vk_cmd_enqueue_entrypoints.h"
#undef VK_ENTRY_USE_WEAK

% for e in entrypoints:
  % if not e.is_device_entrypoint() or e.alias or e.params[0].type != 'VkCommandBuffer' or not e.name.startswith('Cmd'):
    <% continue %>
  % endif
  % if e.guard is not None:
#ifdef ${e.guard}
  % endif
static VKAPI_ATTR ${e.return_type} VKAPI_CALL
${e.prefixed_name('kk_cmd_tramp')}(${e.decl_params()})
{
    VK_FROM_HANDLE(kk_cmd_buffer, cmd, ${e.params[0].name});
  % if e.params[0].type == 'VkCommandBuffer':
    struct kk_device *dev = kk_cmd_buffer_device(cmd);
    % if e.name == 'CmdBeginRendering':
    cmd->deferred_render_chain |= pRenderingInfo->flags &
        (VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT);
    kk_cmd_record_rendering_attachments(cmd, pRenderingInfo);
    % elif e.name in ('CmdBeginQuery', 'CmdBeginQueryIndexedEXT'):
    VK_FROM_HANDLE(kk_query_pool, pool, queryPool);
    if (pool->vk.query_type == VK_QUERY_TYPE_OCCLUSION &&
        kk_cmd_buffer_record_occlusion_query(cmd) != VK_SUCCESS)
        return;
    % elif e.name == 'CmdBindDescriptorBuffersEXT':
    if (bufferCount && kk_cmd_buffer_defer_native_recording(cmd) != VK_SUCCESS)
        return;
    % elif e.name == 'CmdSetDescriptorBufferOffsetsEXT':
    if (setCount && kk_cmd_buffer_defer_native_recording(cmd) != VK_SUCCESS)
        return;
    % elif e.name == 'CmdSetDescriptorBufferOffsets2EXT':
    if (pSetDescriptorBufferOffsetsInfo->stageFlags && pSetDescriptorBufferOffsetsInfo->setCount &&
        kk_cmd_buffer_defer_native_recording(cmd) != VK_SUCCESS)
        return;
    % elif e.name == 'CmdBindDescriptorBufferEmbeddedSamplersEXT':
    if (kk_cmd_buffer_defer_native_recording(cmd) != VK_SUCCESS)
        return;
    % elif e.name == 'CmdBindDescriptorBufferEmbeddedSamplers2EXT':
    if (pBindDescriptorBufferEmbeddedSamplersInfo->stageFlags &&
        kk_cmd_buffer_defer_native_recording(cmd) != VK_SUCCESS)
        return;
    % elif e.name == 'CmdExecuteCommands':
    for (uint32_t i = 0; i < commandBufferCount &&
         (!cmd->deferred_render_chain || !cmd->transient_candidate ||
          !cmd->uses_occlusion_queries); i++) {
        VK_FROM_HANDLE(kk_cmd_buffer, secondary, pCommandBuffers[i]);
        cmd->transient_candidate |= secondary->transient_candidate;
        if (secondary->uses_occlusion_queries &&
            kk_cmd_buffer_record_occlusion_query(cmd) != VK_SUCCESS)
            return;
        if (secondary->deferred_render_chain &&
            kk_cmd_buffer_defer_native_recording(cmd) != VK_SUCCESS)
            return;
    }
    % endif
    % if e.return_type == 'void':
    KK_PERF_VOID(dev, KK_PERF_RECORD,
        cmd->record_dispatch->${e.name}(${e.call_params()}));
      % if e.name == 'CmdPipelineBarrier2':
    if (unlikely(pDependencyInfo->pNext != NULL))
        kk_temporal_copy_enqueued(cmd, pDependencyInfo);
      % endif
    % else:
    return KK_PERF_CALL(dev, KK_PERF_RECORD,
        cmd->record_dispatch->${e.name}(${e.call_params()}));
    % endif
  % endif
}
  % if e.guard is not None:
#endif
  % endif
% endfor

/* The enqueue entrypoint table has a different layout from a dispatch table.
 * Use the actual enqueue symbols in the same typed, read-only dispatch shape. */
const struct vk_device_dispatch_table kk_cmd_enqueue_dispatch = {
% for e in entrypoints:
  % if not e.is_device_entrypoint() or e.alias or e.params[0].type != 'VkCommandBuffer' or not e.name.startswith('Cmd') or e.name in no_enqueue_commands:
    <% continue %>
  % endif
  % if e.guard is not None:
#ifdef ${e.guard}
  % endif
    .${e.name} = ${e.prefixed_name('vk_cmd_enqueue')},
  % if e.guard is not None:
#endif
  % endif
% endfor
};

struct vk_device_dispatch_table kk_device_cmd_trampolines = {
% for e in entrypoints:
  % if not e.is_device_entrypoint() or e.alias or e.params[0].type != 'VkCommandBuffer' or not e.name.startswith('Cmd'):
    <% continue %>
  % endif
  % if e.guard is not None:
#ifdef ${e.guard}
  % endif
    .${e.name} = ${e.prefixed_name('kk_cmd_tramp')},
  % if e.guard is not None:
#endif
  % endif
% endfor
};
""")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--import-path', required=True,
                        help='Path to the Vulkan utility generators.')
    parser.add_argument('--out-c', help='Output C file.')
    parser.add_argument('--out-h', help='Output H file.')
    parser.add_argument('--beta', required=True, help='Enable beta extensions.')
    parser.add_argument('--xml',
                        help='Vulkan API XML file.',
                        required=True,
                        action='append',
                        dest='xml_files')
    args = parser.parse_args()

    sys.path.insert(0, args.import_path)
    from vk_cmd_queue_gen import NO_ENQUEUE_COMMANDS

    entrypoints = get_entrypoints_from_xml(args.xml_files, args.beta)

    # For outputting entrypoints.h we generate a anv_EntryPoint() prototype
    # per entry point.
    try:
        if args.out_h:
            with open(args.out_h, 'w', encoding='utf-8') as f:
                f.write(TEMPLATE_H.render(entrypoints=entrypoints,
                                          filename=os.path.basename(__file__)))
        if args.out_c:
            with open(args.out_c, 'w', encoding='utf-8') as f:
                f.write(TEMPLATE_C.render(entrypoints=entrypoints,
                                          no_enqueue_commands=NO_ENQUEUE_COMMANDS,
                                          filename=os.path.basename(__file__)))
    except Exception:
        # In the event there's an error, this imports some helpers from mako
        # to print a useful stack trace and prints it, then exits with
        # status 1, if python is run with debug; otherwise it just raises
        # the exception
        if __debug__:
            from mako import exceptions
            sys.stderr.write(exceptions.text_error_template().render() + '\n')
            sys.exit(1)
        raise


if __name__ == '__main__':
    main()
