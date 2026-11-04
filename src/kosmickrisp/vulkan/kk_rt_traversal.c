/* SPDX-License-Identifier: MIT */
#include "kosmickrisp-rt-cps.h"
#include "kk_ray_tracing.h"
#include "nir/nir_builder.h"
#include "kosmickrisp/compiler/nir_to_msl.h"
#include "util/ralloc.h"
/* The driver traversal is a normal split NIR shader. It shares precisely the
 * rq state and portable intersection arithmetic used by application queries.
 * Its internal yields carry explicit actions, never application SBT markers. */
static nir_def *load(nir_builder *b,unsigned off,unsigned n,unsigned bits){return nir_load_global(b,n,bits,nir_iadd_imm(b,nir_load_param(b,0),off),.align_mul=bits/8);}
static void store(nir_builder *b,unsigned off,nir_def *v){nir_store_global(b,v,nir_iadd_imm(b,nir_load_param(b,0),off),.align_mul=v->bit_size/8);}
#define L(field,n,bits) load(&b,offsetof(struct kk_rt_cps_context,field),n,bits)
#define S(field,v) store(&b,offsetof(struct kk_rt_cps_context,field),v)
static void snapshot(nir_builder *b,nir_def *q,bool committed){
   const struct {nir_ray_query_value value;unsigned off,n,bits;} fields[]={
#define F(v,f,n,bits) {nir_ray_query_value_##v,offsetof(struct kk_rt_cps_context,f),n,bits}
      F(intersection_t,candidate_t,1,32),F(intersection_geometry_index,geometry_id,1,32),F(intersection_primitive_index,primitive_id,1,32),
      F(intersection_instance_id,instance_id,1,32),F(intersection_instance_custom_index,instance_custom_index,1,32),F(intersection_instance_sbt_index,candidate_sbt,1,32),
      F(intersection_object_ray_origin,object_origin,3,32),F(intersection_object_ray_direction,object_direction,3,32),
#undef F
   };
   for(unsigned i=0;i<ARRAY_SIZE(fields);++i){const __typeof__(fields[0]) *f=&fields[i];store(b,f->off,nir_rq_load(b,f->n,f->bits,q,.ray_query_value=f->value,.committed=committed));}
   for(unsigned col=0;col<4;++col)for(unsigned which=0;which<2;++which){
      nir_def *v=nir_rq_load(b,3,32,q,.ray_query_value=which?nir_ray_query_value_intersection_world_to_object:nir_ray_query_value_intersection_object_to_world,.committed=committed,.column=col);
      unsigned base=which?offsetof(struct kk_rt_cps_context,world_to_object):offsetof(struct kk_rt_cps_context,object_to_world);
      for(unsigned row=0;row<3;++row)store(b,base+(row*4+col)*4,nir_channel(b,v,row));
   }
   /* Triangle-only getters are guarded by the actual query type. */
   nir_def *type=nir_rq_load(b,1,32,q,.ray_query_value=nir_ray_query_value_intersection_type,.committed=committed);
   nir_push_if(b,nir_ieq_imm(b,type,committed?1:0));
   store(b,offsetof(struct kk_rt_cps_context,candidate_bary),nir_rq_load(b,2,32,q,.ray_query_value=nir_ray_query_value_intersection_barycentrics,.committed=committed));
   nir_def *front=nir_rq_load(b,1,1,q,.ray_query_value=nir_ray_query_value_intersection_front_face,.committed=committed);
   store(b,offsetof(struct kk_rt_cps_context,hit_kind),nir_bcsel(b,front,nir_imm_int(b,0xfe),nir_imm_int(b,0xff)));
   for(unsigned vertex=0;vertex<3;++vertex)
      store(b,offsetof(struct kk_rt_cps_context,triangle_vertices)+vertex*12,
         nir_rq_load(b,3,32,q,.ray_query_value=nir_ray_query_value_intersection_triangle_vertex_positions,.committed=committed,.column=vertex));
   nir_pop_if(b,NULL);
}
/* Build param loads before splitting without introducing a fixed Metal slot.
 * The splitter adds the sole context parameter. */
/* pipeline_ray_flags holds the SPIR-V skip flags that the pipeline's
 * SKIP_TRIANGLES/SKIP_AABBS create flags add to every trace. They select
 * traversal only; shaders still read the trace's own incoming flags. */
nir_shader *kk_rt_build_traversal(void *mem_ctx, uint32_t pipeline_ray_flags){
   nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_RAYGEN,&kk_nir_options,"kk_rt_traversal");
   b.impl->function->num_params=1;
   b.impl->function->params=ralloc_array(b.impl->function,nir_parameter,1);
   b.impl->function->params[0]=(nir_parameter){.num_components=1,.bit_size=64};
   nir_variable *query=nir_local_variable_create(b.impl,glsl_uint64_t_type(),"traversal");query->data.ray_query=true;
   nir_def *q=&nir_build_deref_var(&b,query)->def;
   nir_rq_initialize(&b,q,L(acceleration_structure,1,64),nir_ior_imm(&b,L(flags,1,32),pipeline_ray_flags),L(cull_mask,1,32),L(origin,3,32),L(tmin,1,32),L(direction,3,32),L(tmax,1,32));
   nir_loop *outer=nir_push_loop(&b);
   nir_push_if(&b,nir_inot(&b,nir_rq_proceed(&b,1,q)));nir_jump(&b,nir_jump_break);nir_pop_if(&b,NULL);
   snapshot(&b,q,false);
   S(candidate_type,nir_rq_load(&b,1,32,q,.ray_query_value=nir_ray_query_value_intersection_type,.committed=false));
   S(candidate_opaque,nir_b2i32(&b,nir_rq_load(&b,1,1,q,.ray_query_value=nir_ray_query_value_intersection_candidate_aabb_opaque,.committed=false)));
   S(query_event,nir_imm_int(&b,0));S(action,nir_imm_int(&b,KK_RT_CANDIDATE));nir_execute_callable(&b,nir_imm_int(&b,0),nir_imm_int64(&b,0));
   nir_loop *events=nir_push_loop(&b);
   nir_def *event=L(query_event,1,32);
   nir_push_if(&b,nir_ieq_imm(&b,event,1));
   nir_push_if(&b,nir_ine_imm(&b,L(query_accept,1,32),0));nir_rq_confirm_intersection(&b,q);nir_pop_if(&b,NULL);
   nir_push_if(&b,nir_ine_imm(&b,L(terminate,1,32),0));nir_rq_terminate(&b,q);nir_pop_if(&b,NULL);
   nir_jump(&b,nir_jump_break);nir_pop_if(&b,NULL);
   nir_push_if(&b,nir_ieq_imm(&b,event,2));
   nir_push_if(&b,nir_ine_imm(&b,L(query_accept,1,32),0));nir_rq_generate_intersection(&b,q,L(report_t,1,32));nir_pop_if(&b,NULL);
   S(tmax,nir_rq_load(&b,1,32,q,.ray_query_value=nir_ray_query_value_intersection_t,.committed=true));
   S(action,nir_imm_int(&b,KK_RT_REPORT_DONE));nir_execute_callable(&b,nir_imm_int(&b,0),nir_imm_int64(&b,0));
   nir_push_else(&b,NULL);
   nir_push_if(&b,nir_ieq_imm(&b,event,4));nir_rq_terminate(&b,q);nir_pop_if(&b,NULL);
   nir_jump(&b,nir_jump_break);nir_pop_if(&b,NULL);
   nir_pop_loop(&b,events);
   nir_pop_loop(&b,outer);
   S(committed_type,nir_rq_load(&b,1,32,q,.ray_query_value=nir_ray_query_value_intersection_type,.committed=true));
   nir_push_if(&b,nir_ine_imm(&b,L(committed_type,1,32),0));snapshot(&b,q,true);nir_pop_if(&b,NULL);
   S(action,nir_imm_int(&b,KK_RT_TRAVERSAL_DONE));
   nir_shader_gather_info(b.shader,b.impl);kk_nir_lower_ray_queries(b.shader);
   ralloc_steal(mem_ctx,b.shader);return b.shader;
}
