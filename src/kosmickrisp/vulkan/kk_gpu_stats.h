/* SPDX-License-Identifier: MIT */
/* Included only by kk_perf.c; all storage exists only for GPU encoder tracking. */
#define KK_GPU_STATS_LIMIT 1024
struct kk_gpu_encoder_stat {
   struct mtl_gpu_encoder_result key;
   uint64_t count, draws, dispatches, blits;
   double total, max;
};
struct kk_gpu_pipeline_stat {
   char hash[65];
   uint64_t count;
   double total, max;
};
struct kk_gpu_stats {
   struct kk_gpu_encoder_stat encoders[KK_GPU_STATS_LIMIT];
   struct kk_gpu_pipeline_stat pipelines[KK_GPU_STATS_LIMIT];
   unsigned encoders_count, pipelines_count;
   uint64_t unavailable, dropped, truncated;
};

static void
kk_perf_gpu_encoder(void *data, const struct mtl_gpu_encoder_result *r)
{
   struct kk_perf *p = data;
   pthread_mutex_lock(&p->mutex);
   struct kk_gpu_stats *stats = p->gpu_encoders;
   if (!strcmp(r->kind, "tracking_overflow")) { stats->dropped += r->draws; pthread_mutex_unlock(&p->mutex); return; }
   if (!r->valid) { stats->unavailable++; pthread_mutex_unlock(&p->mutex); return; }
   if (r->hashes_truncated) stats->truncated++;
   unsigned i;
   for (i=0; i<stats->encoders_count; ++i) {
      struct mtl_gpu_encoder_result *key = &stats->encoders[i].key;
      if (!strcmp(key->kind, r->kind) && key->hash_count == r->hash_count &&
          key->width == r->width && key->height == r->height && key->depth == r->depth &&
          key->extent_known == r->extent_known && key->pipeline_unknown == r->pipeline_unknown &&
          !memcmp(key->hashes, r->hashes, sizeof(key->hashes))) break;
   }
   if (i < KK_GPU_STATS_LIMIT) {
      if (i == stats->encoders_count) { stats->encoders_count++; stats->encoders[i].key = *r; }
      struct kk_gpu_encoder_stat *s = &stats->encoders[i];
      s->count++; s->draws += r->draws; s->dispatches += r->dispatches; s->blits += r->blits;
      s->total += r->ms; s->max = fmax(s->max, r->ms);
   } else stats->dropped++;
   /* Inclusive encoder membership, not a fabricated division of a mixed
    * pass among its shaders. These totals must not be added across hashes. */
   for (unsigned h=0; h<r->hash_count; ++h) {
      for (i=0; i<stats->pipelines_count; ++i)
         if (!strcmp(stats->pipelines[i].hash, r->hashes[h])) break;
      if (i == KK_GPU_STATS_LIMIT) { stats->dropped++; continue; }
      if (i == stats->pipelines_count) { stats->pipelines_count++; memcpy(stats->pipelines[i].hash, r->hashes[h], 65); }
      struct kk_gpu_pipeline_stat *s = &stats->pipelines[i];
      s->count++; s->total += r->ms; s->max = fmax(s->max, r->ms);
   }
   pthread_mutex_unlock(&p->mutex);
}

static int gpu_encoder_compare(const void *a, const void *b)
{
   double x=((const struct kk_gpu_encoder_stat *)a)->total, y=((const struct kk_gpu_encoder_stat *)b)->total;
   return (y>x)-(y<x);
}
static int gpu_pipeline_compare(const void *a, const void *b)
{
   double x=((const struct kk_gpu_pipeline_stat *)a)->total, y=((const struct kk_gpu_pipeline_stat *)b)->total;
   return (y>x)-(y<x);
}
static void
gpu_encoders_flush_locked(struct kk_perf *p)
{
   struct kk_gpu_stats *g=p->gpu_encoders;
   if (!g) return;
   qsort(g->encoders,g->encoders_count,sizeof(g->encoders[0]),gpu_encoder_compare);
   qsort(g->pipelines,g->pipelines_count,sizeof(g->pipelines[0]),gpu_pipeline_compare);
   fprintf(p->log, ",\"gpu_encoder_scope\":\"synchronous_Metal4_counter_heap_encoder_boundaries\","
      "\"gpu_pipeline_scope\":\"inclusive_encoder_membership_not_independent_shader_time\","
      "\"gpu_encoder_unavailable\":%"PRIu64",\"gpu_encoder_dropped\":%"PRIu64
      ",\"gpu_encoder_hashes_truncated\":%"PRIu64",\"gpu_encoders\":[",g->unavailable,g->dropped,g->truncated);
   for(unsigned i=0;i<MIN2(g->encoders_count,20);++i) {
      struct kk_gpu_encoder_stat *s=&g->encoders[i];
      fprintf(p->log,"%s{\"kind\":\"%s\",\"pipeline_hashes\":[",i?",":"",s->key.kind);
      for(unsigned j=0;j<s->key.hash_count;++j)fprintf(p->log,"%s\"%s\"",j?",":"",s->key.hashes[j]);
      fprintf(p->log,"],\"pipeline_hashes_known\":%s,\"extent\":[%u,%u,%u],\"extent_known\":%s,\"draws\":%"PRIu64
         ",\"dispatches\":%"PRIu64",\"blits\":%"PRIu64",\"count\":%"PRIu64
         ",\"total_ms\":%.6f,\"mean_ms\":%.6f,\"max_ms\":%.6f}",s->key.pipeline_unknown?"false":"true",s->key.width,s->key.height,s->key.depth,
         s->key.extent_known?"true":"false",s->draws,s->dispatches,s->blits,s->count,s->total,s->total/s->count,s->max);
   }
   fprintf(p->log,"],\"gpu_pipelines\":[");
   for(unsigned i=0;i<MIN2(g->pipelines_count,20);++i) {
      struct kk_gpu_pipeline_stat *s=&g->pipelines[i];
      fprintf(p->log,"%s{\"blake3\":\"%s\",\"count\":%"PRIu64",\"total_ms\":%.6f,\"mean_ms\":%.6f,\"max_ms\":%.6f}",
         i?",":"",s->hash,s->count,s->total,s->total/s->count,s->max);
   }
   fprintf(p->log,"]");
   memset(g,0,sizeof(*g));
}

void kk_perf_gpu_attach_command(struct kk_perf *p, void *command)
{
   if (p->gpu_context) mtl_gpu_timing_attach_command(p->gpu_context,command);
}
void kk_perf_gpu_init(struct kk_device *dev)
{
   if (dev->perf->gpu_encoders)
      dev->perf->gpu_context=mtl_gpu_timing_configure(dev->mtl_handle,dev->perf,kk_perf_gpu_encoder);
}
