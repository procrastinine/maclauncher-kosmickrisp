/* SPDX-License-Identifier: MIT */
/* Emit only for a native callable that consumes the approved Boolean. */
static void
kk_conservative_emit_inner(struct _mesa_string_buffer *text)
{
   P("static bool kk_cr_fully_covered(device const uchar *p,int4 rectangle) {\n"
     " ulong address=*(device const ulong *)(p+%u);\n"
     " if(!address||*(device const uint *)(p+%u)) return false;\n"
     " device const long *edges=(device const long *)address;\n"
     " uint count=*(device const uint *)(p+%u);\n"
     " for(uint i=0;i<count;++i) {\n"
     "  long a=edges[3*i],b=edges[3*i+1],c=edges[3*i+2];\n"
     "  long x=long(a<0?rectangle.z:rectangle.x)*256;\n"
     "  long y=long(b<0?rectangle.w:rectangle.y)*256;\n"
     "  long minimum=a*x+b*y+c;\n"
     "  if(2*minimum<(a<0?-a:a)+(b<0?-b:b)) return false;\n"
     " } return true;\n}\n", OFFSET(inner_edges), OFFSET(degenerate), OFFSET(count));
}
