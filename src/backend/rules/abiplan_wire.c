/* Canonical v1 inspection-plan codec. Explicit fixed-width LE fields;
 * validates before encode or commit, never serializes C struct padding. */
#include "xh_abiplan_wire.h"
#include <string.h>
#define MAGIC 0x50414858u
static int power(unsigned n){return n&&!(n&(n-1));}
static int valid(const XhSysvPlan*p){unsigned i,j;if(!p||(p->version!=1&&p->version!=2)||p->abi_id!=1||p->flags>1||(p->version==1&&p->flags)||p->arg_count>64||p->sret>1||p->result.memory<0||p->result.memory>1||(unsigned)p->result.memory!=p->sret||p->result.count>2||(p->result.memory&&p->result.count)||p->stack_bytes>0x7fffffffu||p->stack_bytes%16)return 0;
 for(j=0;j<p->result.count;j++){const XhTypeclassFragment*f=&p->result.fragments[j];if(f->class_id<0||f->class_id>7||!f->size||f->size>16||f->offset>65535||f->size>65535-f->offset||(j&&f->offset<p->result.fragments[j-1].offset+p->result.fragments[j-1].size))return 0;}
 for(i=0;i<p->arg_count;i++){const XhAbiArgumentPlan*a=&p->args[i];if(a->version!=1||a->by_value_stack>1||a->size>0x7fffffffu||!power(a->alignment)||a->alignment>256||a->count>2||(a->by_value_stack&&a->count))return 0;
  if(a->by_value_stack){if(a->stack_offset%a->alignment||a->stack_offset>p->stack_bytes||a->stack_extent<a->size||a->stack_extent>p->stack_bytes-a->stack_offset)return 0;}
  else if(a->stack_offset||a->stack_extent)return 0;
  for(j=0;j<a->count;j++){const XhAbiPlanLocation*f=&a->fragments[j];if(f->bank>1||f->index>=32||!f->size||f->object_offset>a->size||f->size>a->size-f->object_offset||f->stack_offset||(j&&f->object_offset<a->fragments[j-1].object_offset+a->fragments[j-1].size))return 0;}
 }
 return 1;}
unsigned xh_sysv_plan_wire_size(const XhSysvPlan*p){return valid(p)?4u*(15u+(p->version>=2?1u:0u)+17u*p->arg_count):0;}
static void put(unsigned char**p,unsigned v){unsigned i;for(i=0;i<4;i++)(*p)[i]=(unsigned char)(v>>(8*i));*p+=4;}
static unsigned get(const unsigned char**p){unsigned v=(unsigned)(*p)[0]|((unsigned)(*p)[1]<<8)|((unsigned)(*p)[2]<<16)|((unsigned)(*p)[3]<<24);*p+=4;return v;}
int xh_sysv_plan_encode(const XhSysvPlan*p,unsigned char*out,unsigned cap){unsigned bytes=xh_sysv_plan_wire_size(p),i,j;unsigned char*q=out;if(!out||!bytes||cap<bytes)return -1;put(&q,MAGIC);put(&q,p->version);put(&q,p->abi_id);put(&q,p->rule_fingerprint);put(&q,p->arg_count);put(&q,p->stack_bytes);put(&q,p->sret);if(p->version>=2)put(&q,p->flags);put(&q,(unsigned)p->result.memory);put(&q,p->result.count);for(j=0;j<2;j++){const XhTypeclassFragment*f=&p->result.fragments[j];put(&q,j<p->result.count?f->offset:0);put(&q,j<p->result.count?f->size:0);put(&q,j<p->result.count?(unsigned)f->class_id:0);}
 for(i=0;i<p->arg_count;i++){const XhAbiArgumentPlan*a=&p->args[i];put(&q,a->version);put(&q,a->by_value_stack);put(&q,a->size);put(&q,a->alignment);put(&q,a->count);put(&q,a->stack_offset);put(&q,a->stack_extent);for(j=0;j<2;j++){const XhAbiPlanLocation*f=&a->fragments[j];put(&q,j<a->count?f->object_offset:0);put(&q,j<a->count?f->size:0);put(&q,j<a->count?f->bank:0);put(&q,j<a->count?f->index:0);put(&q,j<a->count?f->stack_offset:0);}}return (int)bytes;}
int xh_sysv_plan_decode(const unsigned char*data,unsigned len,XhSysvPlan*out){XhSysvPlan p;const unsigned char*q=data;unsigned i,j;if(!data||!out||len<60)return -1;memset(&p,0,sizeof(p));if(get(&q)!=MAGIC)return -1;p.version=get(&q);p.abi_id=get(&q);p.rule_fingerprint=get(&q);p.arg_count=get(&q);p.stack_bytes=get(&q);p.sret=get(&q);if(p.version>=2){if(len<64)return -1;p.flags=get(&q);}p.result.memory=(int)get(&q);p.result.count=get(&q);if(p.arg_count>64||len!=4u*(15u+(p.version>=2?1u:0u)+17u*p.arg_count))return -1;for(j=0;j<2;j++){XhTypeclassFragment*f=&p.result.fragments[j];f->offset=get(&q);f->size=get(&q);f->class_id=(int)get(&q);if(j>=p.result.count&&(f->offset||f->size||f->class_id))return -1;}
 for(i=0;i<p.arg_count;i++){XhAbiArgumentPlan*a=&p.args[i];a->version=get(&q);a->by_value_stack=get(&q);a->size=get(&q);a->alignment=get(&q);a->count=get(&q);a->stack_offset=get(&q);a->stack_extent=get(&q);for(j=0;j<2;j++){XhAbiPlanLocation*f=&a->fragments[j];f->object_offset=get(&q);f->size=get(&q);f->bank=get(&q);f->index=get(&q);f->stack_offset=get(&q);if(j>=a->count&&(f->object_offset||f->size||f->bank||f->index||f->stack_offset))return -1;}}
 if(!valid(&p))return -1;*out=p;return 0;}
