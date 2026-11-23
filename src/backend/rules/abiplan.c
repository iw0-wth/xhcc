/* Standalone bounded ABI argument placement. External policy describes bank
 * demand; this mechanism budgets the whole object before committing registers. */
#include "xh_abiplan.h"
#include "xh_template.h"
#include "diag.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
static int error(void){diag_tool(DIAG_SEV_ERROR,"ABI plan","invalid policy, classification, state or placement overflow",0,0,0,0,0,0);return -1;}
static int power(unsigned n){return n&&!(n&(n-1));}
static int numbers(const char*s,unsigned*v,unsigned n){unsigned i;char*end;if(!s)return -1;for(i=0;i<n;i++){unsigned long x;while(*s==' '||*s=='\t'||*s=='\r'||*s=='\n')s++;if(*s<'0'||*s>'9')return -1;errno=0;x=strtoul(s,&end,10);if(errno||x>65535||end==s||(*end&&*end!=' '&&*end!='\t'&&*end!='\r'&&*end!='\n'))return -1;v[i]=(unsigned)x;s=end;}while(*s==' '||*s=='\t'||*s=='\r'||*s=='\n')s++;return *s?-1:0;}
static int policy_ok(const XhAbiPlanPolicy*p){unsigned i;if(!p||p->version!=1||p->capacity[0]>32||p->capacity[1]>32||!power(p->stack_slot)||p->stack_slot>64||!power(p->call_alignment)||p->call_alignment>256||!power(p->max_alignment)||p->max_alignment>256||p->stack_slot>p->max_alignment||p->sret_bank>1||p->sret_count>p->capacity[p->sret_bank]||p->class_count<1||p->class_count>XH_ABIPLAN_MAX_CLASSES)return 0;for(i=0;i<p->class_count;i++){const XhAbiPlanClass*c=&p->classes[i];if(c->bank>2||c->demand>1||c->share_previous>1||(c->bank==2&&(c->demand||c->share_previous))||(c->demand&&c->share_previous)||(!c->demand&&!c->share_previous&&c->bank!=2))return 0;}return 1;}
int xh_abiplan_load(const char*t,const char*prefix,XhAbiPlanPolicy*out){XhAbiPlanPolicy p;unsigned v[9],i;char key[96];if(!t||!prefix||!out||strlen(prefix)>70)return error();memset(&p,0,sizeof(p));snprintf(key,sizeof(key),"%s.config",prefix);if(numbers(xh_template_get(t,key),v,9))return error();p.version=v[0];p.capacity[0]=v[1];p.capacity[1]=v[2];p.stack_slot=v[3];p.call_alignment=v[4];p.max_alignment=v[5];p.sret_bank=v[6];p.sret_count=v[7];p.class_count=v[8];if(p.class_count>XH_ABIPLAN_MAX_CLASSES)return error();for(i=0;i<p.class_count;i++){snprintf(key,sizeof(key),"%s.class.%u",prefix,i);if(numbers(xh_template_get(t,key),v,3))return error();p.classes[i].bank=v[0];p.classes[i].demand=v[1];p.classes[i].share_previous=v[2];}if(!policy_ok(&p))return error();*out=p;return 0;}
static int state_ok(const XhAbiPlanPolicy*p,const XhAbiPlanState*s){return policy_ok(p)&&s&&s->version==1&&s->used[0]<=p->capacity[0]&&s->used[1]<=p->capacity[1]&&s->argument_count<=XH_ABIPLAN_MAX_ARGUMENTS&&s->sret_reserved<=1;}
static int aligned(unsigned value,unsigned alignment,unsigned*out){unsigned mask=alignment-1;if(value>UINT_MAX-mask)return -1;*out=(value+mask)&~mask;return 0;}
int xh_abiplan_begin(const XhAbiPlanPolicy*p,int sret,XhAbiPlanState*out){XhAbiPlanState s;if(!out||!policy_ok(p)||(sret!=0&&sret!=1))return error();memset(&s,0,sizeof(s));s.version=1;s.sret_reserved=(unsigned)sret;if(sret)s.used[p->sret_bank]=p->sret_count;*out=s;return 0;}
int xh_abiplan_argument(const XhAbiPlanPolicy*p,XhAbiPlanState*state,const XhTypeclassResult*c,unsigned size,unsigned alignment,XhAbiArgumentPlan*out){XhAbiPlanState next;XhAbiArgumentPlan plan;unsigned i,last_end=0,needed[2]={0,0},effective;int fallback;
 if(!out||!state_ok(p,state)||!c||state->argument_count==XH_ABIPLAN_MAX_ARGUMENTS||!power(alignment)||alignment>p->max_alignment||c->memory<0||c->memory>1||c->count>XH_TYPECLASS_MAX_FRAGMENTS||(c->memory&&c->count)||(!c->memory&&size&&!c->count))return error();
 memset(&plan,0,sizeof(plan));next=*state;plan.version=1;plan.size=size;plan.alignment=alignment;fallback=c->memory;
 for(i=0;i<c->count;i++){const XhTypeclassFragment*f=&c->fragments[i];const XhAbiPlanClass*cl;XhAbiPlanLocation*l;
  if(f->class_id<0||(unsigned)f->class_id>=p->class_count||!f->size||f->offset<last_end||f->offset>size||f->size>size-f->offset)return error();last_end=f->offset+f->size;cl=&p->classes[f->class_id];
  if(cl->bank==XH_ABIPLAN_BANK_NONE)continue;
  if(cl->share_previous){if(!plan.count||plan.fragments[plan.count-1].bank!=cl->bank)return error();}
  l=&plan.fragments[plan.count++];l->object_offset=f->offset;l->size=f->size;l->bank=cl->bank;
  if(cl->share_previous)l->index=plan.fragments[plan.count-2].index;
  else{l->index=next.used[cl->bank]+needed[cl->bank];needed[cl->bank]+=cl->demand;}
 }
 for(i=0;i<2;i++)if(needed[i]>p->capacity[i]-next.used[i])fallback=1;
 if(fallback){unsigned extent,offset;effective=alignment>p->stack_slot?alignment:p->stack_slot;if(aligned(next.stack_size,effective,&offset)||aligned(size,p->stack_slot,&extent)||offset>UINT_MAX-extent)return error();plan.by_value_stack=1;plan.stack_offset=offset;plan.stack_extent=extent;plan.alignment=effective;plan.count=0;memset(plan.fragments,0,sizeof(plan.fragments));next.stack_size=offset+extent;}
 else for(i=0;i<2;i++)next.used[i]+=needed[i];
 next.argument_count++;*state=next;*out=plan;return 0;
}
int xh_abiplan_finish(const XhAbiPlanPolicy*p,const XhAbiPlanState*s,unsigned*out){unsigned size;if(!out||!state_ok(p,s)||aligned(s->stack_size,p->call_alignment,&size))return error();*out=size;return 0;}

/* Validate the complete signature, including resources invisible in one arg. */
int xh_abiplan_validate_layout(const XhAbiPlanPolicy*p,const XhAbiArgumentPlan*args,unsigned count,unsigned stack_bytes,unsigned sret,const XhTypeclassResult*result,const unsigned*proof){
 unsigned occupied[2]={0,0},i,j,end=0,finished;unsigned starts[64],ends[64],nstack=0;
 if(!policy_ok(p)||count>64||(count&&!args)||sret>1||!result||result->memory<0||result->memory>1||result->count>2||(result->memory&&result->count)||(unsigned)result->memory!=sret)return error();
 if(sret)for(i=0;i<p->sret_count;i++)occupied[p->sret_bank]|=1u<<i;
 for(i=0;i<result->count;i++){const XhTypeclassFragment*f=&result->fragments[i];const XhAbiPlanClass*c;if(f->class_id<0||(unsigned)f->class_id>=p->class_count||!f->size||f->offset>65535||f->size>65535-f->offset||(i&&f->offset<result->fragments[i-1].offset+result->fragments[i-1].size))return error();c=&p->classes[f->class_id];if(c->share_previous){const XhAbiPlanClass*previous;if(!i)return error();previous=&p->classes[result->fragments[i-1].class_id];if(previous->bank!=c->bank||previous->bank==2)return error();}}
 for(i=0;i<count;i++){const XhAbiArgumentPlan*a=&args[i];unsigned last=0,extent;
  if(a->version!=1||a->by_value_stack>1||!power(a->alignment)||a->alignment>p->max_alignment||a->count>2)return error();
  if(a->by_value_stack){if(a->count||a->alignment<p->stack_slot||a->stack_offset%a->alignment||aligned(a->size,p->stack_slot,&extent)||a->stack_extent!=extent||a->stack_offset>UINT_MAX-a->stack_extent)return error();starts[nstack]=a->stack_offset;ends[nstack]=a->stack_offset+a->stack_extent;for(j=0;j<nstack;j++)if(starts[nstack]<ends[j]&&starts[j]<ends[nstack])return error();if(ends[nstack]>end)end=ends[nstack];nstack++;continue;}
  if(a->stack_offset||a->stack_extent||(!a->count&&a->size))return error();
  for(j=0;j<a->count;j++){const XhAbiPlanLocation*f=&a->fragments[j];const XhAbiPlanClass*c=0;unsigned bit;int share=0;
   if(f->bank>1||f->index>=p->capacity[f->bank]||!f->size||f->object_offset<last||f->object_offset>a->size||f->size>a->size-f->object_offset||f->stack_offset)return error();last=f->object_offset+f->size;
   if(proof){unsigned id=proof[2*i+j];if(id>=p->class_count)return error();c=&p->classes[id];if(c->bank!=f->bank)return error();share=(int)c->share_previous;}
   bit=1u<<f->index;
   if(share){const XhAbiPlanLocation*previous;unsigned previous_id;if(!j)return error();previous=&a->fragments[j-1];previous_id=proof[2*i+j-1];if(previous->bank!=f->bank||previous->index!=f->index||previous_id>=p->class_count||p->classes[previous_id].bank!=f->bank||!p->classes[previous_id].demand)return error();}
   else{if(occupied[f->bank]&bit)return error();occupied[f->bank]|=bit;}
  }
 }
 if(aligned(end,p->call_alignment,&finished)||finished!=stack_bytes)return error();return 0;
}
