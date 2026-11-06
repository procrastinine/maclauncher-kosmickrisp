/* SPDX-License-Identifier: MIT
 * GPU ICB construction follows MetalSharp's MoltenVK adaptation at
 * 4e89065e359f9c6c2a78170b045835d6382113d9. The root table and preprocess
 * records below are Kosmic's; no host readback of generated data occurs.
 */
#include <metal_stdlib>
#include <metal_command_buffer>
using namespace metal;
struct Patch { uint src, dst, size, sequence; };
struct Attribute { uint src, slot, binding, offset, size, dynamic_stride; };
struct Params {
   ulong stream, count, output, base_root, base_draw, patches, attributes;
   ulong samplers, index, condition;
   uint maximum, first, stream_stride, record_stride;
   uint root_size, draw_offset, args_offset, index_offset;
   uint patch_count, attribute_count, action_offset, index_token;
   uint root_push, root_strides, root_bases, root_clamps;
   uint draw_base_vertex, draw_base_instance;
   uint action, primitive, index_size, dx_index;
   packed_uint3 local;
   uint inverted;
   uint ordinary, index_range;
   ulong execution_range;
   uint root_vertex_input, vertex_input_size, vertex_input_offset;
   uint vertex_fetch_size, vertex_fetch_stride;
};
struct ICB { command_buffer commands; };
static ulong kk_dgc_address(const device uchar *p) {
   const device uint *w = (const device uint *)p;
   return ulong(w[0]) | (ulong(w[1]) << 32);
}
/* A sequence record contains a root, per-draw state, action args and an index
 * binding. Explicit preprocessing freezes all token values on the GPU. */
kernel void kk_dgc_expand(constant Params &p [[buffer(KK_ARGUMENT_ROOT)]], uint i [[thread_position_in_grid]]) {
   if (i >= p.maximum) return;
   uint sequence = p.first + i;
   uint count = p.count ? min(*(const device uint *)p.count, p.maximum + p.first) : p.maximum + p.first;
   if (sequence >= count || (p.condition && ((*(const device uint *)p.condition != 0) == bool(p.inverted)))) {
      device uint *args=(device uint *)((device uchar *)p.output+ulong(i)*p.record_stride+p.args_offset);
      for(uint j=0;j<5;++j) args[j]=0;
      return;
   }
   device uchar *record = (device uchar *)p.output + ulong(i) * p.record_stride;
   const device uchar *src = (const device uchar *)p.stream + ulong(sequence) * p.stream_stride;
   const device uchar *base = (const device uchar *)p.base_root;
   for (uint j = 0; j < p.root_size; ++j) record[j] = base[j];
   if(p.vertex_input_size) {
      const device uchar *input=(const device uchar *)*(device ulong *)(record+p.root_vertex_input);
      device uchar *output=record+p.vertex_input_offset;
      for(uint j=0;j<p.vertex_input_size;++j) output[j]=input[j];
      *(device ulong *)(record+p.root_vertex_input)=ulong(output);
   }
   const device Patch *patches = (const device Patch *)p.patches;
   for (uint t = 0; t < p.patch_count; ++t) {
      Patch patch = patches[t];
      for (uint j = 0; j < patch.size; j += 4)
         *(device uint *)(record + p.root_push + patch.dst + j) = patch.sequence ? sequence : *(const device uint *)(src + patch.src + j);
   }
   const device Attribute *attributes = (const device Attribute *)p.attributes;
   for (uint t = 0; t < p.attribute_count; ++t) {
      Attribute a = attributes[t];
      const device uchar *token = src + a.src;
      ulong vb = kk_dgc_address(token);
      uint size = *(const device uint *)(token + 8);
      uint stride = a.dynamic_stride ? *(const device uint *)(token + 12) : *(device uint *)(record + p.root_strides + a.binding * 4);
      *(device uint *)(record + p.root_strides + a.binding * 4) = stride;
      if(p.vertex_input_size) *(device uint *)(record+p.vertex_input_offset+a.slot*p.vertex_fetch_size+p.vertex_fetch_stride)=stride;
      bool valid = ulong(size) >= ulong(a.offset) + a.size;
      *(device ulong *)(record + p.root_bases + a.slot * 8) = valid ? vb + a.offset : 0;
      *(device uint *)(record + p.root_clamps + a.slot * 4) = valid ? (stride ? (size - a.offset - a.size) / stride : ~0u) : 0;
   }
   device uchar *draw = record + p.draw_offset;
   const device uchar *draw_base = (const device uchar *)p.base_draw;
   for (uint j=0; j<p.args_offset-p.draw_offset; ++j) draw[j] = draw_base[j];
   *(device uint *)draw = p.ordinary ? sequence : 0;
   device uint *args = (device uint *)(record + p.args_offset);
   bool enabled = sequence < count;
   if (p.condition) enabled &= ((*(const device uint *)p.condition != 0) != bool(p.inverted));
   uint words = p.action == 2 ? 3 : (p.action == 1 ? 5 : 4);
   for(uint j=0;j<words;++j) args[j] = enabled ? *(const device uint *)(src+p.action_offset+j*4) : 0;
   *(device ulong *)(draw + p.draw_base_vertex) = ulong(args + (p.action == 1 ? 3 : 2));
   *(device ulong *)(draw + p.draw_base_instance) = ulong(args + (p.action == 1 ? 4 : 3));
   device ulong *index = (device ulong *)(record + p.index_offset);
   index[0] = p.index;
   *(device uint *)((device uchar *)index+12) = p.index_range;
   *(device uint *)(index+1) = p.index_size;
   if (p.index_token != ~0u) {
      const device uchar *token = src + p.index_token;
      index[0] = kk_dgc_address(token);
      *(device uint *)((device uchar *)index+12) = *(const device uint *)(token+8);
      uint type = *(const device uint *)(token + 12);
      *(device uint *)(index+1) = p.dx_index ? (type == 57 ? 2 : 4) : (type == 0 ? 2 : (type == 1 ? 4 : 1));
   }
}
kernel void kk_dgc_commands(constant Params &p [[buffer(KK_ARGUMENT_ROOT)]], constant ICB &icb [[buffer(KK_ARGUMENT_SAMPLERS)]], uint i [[thread_position_in_grid]]) {
   if(i >= p.maximum) return;
   uint count=p.count ? min(*(const device uint *)p.count,p.maximum+p.first) : p.maximum+p.first;
   if(p.condition && ((*(const device uint *)p.condition != 0) == bool(p.inverted))) count=0;
   uint active=count>p.first?min(count-p.first,p.maximum):0;
   if(i==0) {device uint *range=(device uint *)p.execution_range;range[0]=0;range[1]=active;}
   if(i>=active) return;
   device uchar *record = (device uchar *)p.output + ulong(i)*p.record_stride;
   const device uint *args = (const device uint *)(record+p.args_offset);
   if(p.action == 2) {
      compute_command cmd(icb.commands,i); cmd.reset();
      if(!args[0] || !args[1] || !args[2] || (p.condition && ((*(const device uint *)p.condition != 0) == bool(p.inverted)))) return;
      cmd.set_kernel_buffer(record,KK_ARGUMENT_ROOT);
      cmd.set_kernel_buffer((device void *)p.samplers,KK_ARGUMENT_SAMPLERS);
      cmd.concurrent_dispatch_threadgroups(uint3(args[0],args[1],args[2]),uint3(p.local));
   } else {
      render_command cmd(icb.commands,i); cmd.reset();
      if(!args[0] || !args[1] || (p.condition && ((*(const device uint *)p.condition != 0) == bool(p.inverted)))) return;
      cmd.set_vertex_buffer(record,KK_ARGUMENT_ROOT); cmd.set_fragment_buffer(record,KK_ARGUMENT_ROOT);
      cmd.set_vertex_buffer((device void *)p.samplers,KK_ARGUMENT_SAMPLERS); cmd.set_fragment_buffer((device void *)p.samplers,KK_ARGUMENT_SAMPLERS);
      cmd.set_vertex_buffer(record+p.draw_offset,KK_ARGUMENT_PER_DRAW); cmd.set_fragment_buffer(record+p.draw_offset,KK_ARGUMENT_PER_DRAW);
      if(p.action == 1) {
         const device uchar *idx = record+p.index_offset;
         ulong a=kk_dgc_address(idx); uint size=*(const device uint *)(idx+8);
         if(size==2) cmd.draw_indexed_primitives(primitive_type(p.primitive),args[0],(const device ushort *)a+args[2],args[1],int(args[3]),args[4]);
         else cmd.draw_indexed_primitives(primitive_type(p.primitive),args[0],(const device uint *)a+args[2],args[1],int(args[3]),args[4]);
      } else cmd.draw_primitives(primitive_type(p.primitive),args[2],args[0],args[1],args[3]);
   }
}

/* Snapshot only the token span per sequence and its count. This is
 * the persistent Vulkan preprocess representation; expanded roots are bounded
 * execution scratch and are never retained per maximum sequence. */
kernel void kk_dgc_preprocess(constant Params &p [[buffer(KK_ARGUMENT_ROOT)]], uint i [[thread_position_in_grid]]) {
   if(i >= p.maximum) return;
   device uchar *out=(device uchar *)p.output;
   uint count=p.count?min(*(const device uint *)p.count,p.maximum):p.maximum;
   if(i==0) {
      *(device uint *)p.execution_range=count;

   }
   if(i>=count) return;
   const device uchar *src=(const device uchar *)p.stream+ulong(i)*p.stream_stride;
   device uchar *dst=out+p.args_offset+ulong(i)*p.record_stride;
   for(uint j=0;j<p.record_stride;++j) dst[j]=src[j];
}

kernel void kk_dgc_offset_count(constant Params &p [[buffer(KK_ARGUMENT_ROOT)]]) {
   uint count=*(const device uint *)p.count;
   *(device uint *)p.output=count>p.first?count-p.first:0;
}
