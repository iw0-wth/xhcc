/* Type adapter for explicit inspection plans. No machine-code lowering. */
#include "xh_sysv_plan.h"
#include "xh_template.h"
#include "diag.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
typedef struct {XhTypeclassNode nodes[128];XhTypeclassEdge edges[256];unsigned n,e;Type *path[32];} Graph;
int xh_sysv_plan_set_path(const char*path){return xh_typeclass_set_path("x86_64",path&&*path?path:"packages/x86_64/aggregate.rules");}
static Type *adjust_parameter(Type*t,Type*storage){if(t&&(t->kind==TYPE_ARRAY||t->kind==TYPE_FUNC)){memset(storage,0,sizeof(*storage));storage->kind=TYPE_PTR;storage->base=t->kind==TYPE_ARRAY?t->base:t;storage->size=8;storage->align_override=8;return storage;}return t;}
static int scalar(Type*t){return t&&(t->kind==TYPE_VOID||t->kind==TYPE_PTR||type_is_integer(t)||t->kind==TYPE_FLOAT||t->kind==TYPE_DOUBLE);}
static int small_struct(Type*t,unsigned depth){StructField*f;int size,align;if(!t||depth>=32)return 0;if(t->kind==TYPE_PTR)return 1;if(type_is_integer(t))return type_size(t)>0&&type_size(t)<=8;if(t->kind!=TYPE_STRUCT||t->is_packed)return 0;size=type_size(t);align=type_align(t);if(size<1||size>8||align<1)return 0;for(f=t->fields;f;f=f->next){int fs=type_size(f->type),fa=type_align(f->type);if(f->bit_width||f->bit_offset||f->offset<0||fa<1||f->offset%fa||align<fa||fs<0||f->offset>size-fs||!small_struct(f->type,depth+1))return 0;}return t->fields!=0;}
int xh_sysv_small_integer_struct(Type*t){return t&&t->kind==TYPE_STRUCT&&small_struct(t,0);}
int xh_sysv_small_sse_struct(Type*t){StructField*f;int count=0,kind=0,size,align;if(!t||t->kind!=TYPE_STRUCT||t->is_packed)return 0;size=type_size(t);align=type_align(t);if((size!=4&&size!=8)||align<1)return 0;for(f=t->fields;f;f=f->next){int fs=type_size(f->type),fa=type_align(f->type);if(!f->type||(f->type->kind!=TYPE_FLOAT&&f->type->kind!=TYPE_DOUBLE)||f->bit_width||f->bit_offset||f->offset<0||fa<1||f->offset%fa||align<fa||fs<0||f->offset>size-fs)return 0;if(kind&&kind!=(int)f->type->kind)return 0;kind=(int)f->type->kind;count++;}return (kind==TYPE_DOUBLE&&count==1&&size==8)||(kind==TYPE_FLOAT&&((count==1&&size==4)||(count==2&&size==8)));}
int xh_sysv_integer_pair_struct(Type*t){StructField*a,*b;if(!t||t->kind!=TYPE_STRUCT||t->is_packed||type_size(t)!=16||type_align(t)!=8)return 0;a=t->fields;b=a?a->next:0;if(!a||!b||b->next||a->offset!=0||b->offset!=8||a->bit_width||b->bit_width||a->bit_offset||b->bit_offset)return 0;return a->type&&b->type&&a->type->kind==TYPE_LONG&&b->type->kind==TYPE_LONG&&type_size(a->type)==8&&type_size(b->type)==8;}
int xh_sysv_mixed_pair_struct(Type*t){StructField*a,*b;int x,y;if(!t||t->kind!=TYPE_STRUCT||t->is_packed||type_size(t)!=16||type_align(t)!=8)return 0;a=t->fields;b=a?a->next:0;if(!a||!b||b->next||!a->type||!b->type||a->offset!=0||b->offset!=8||a->bit_width||b->bit_width||a->bit_offset||b->bit_offset||type_size(a->type)!=8||type_size(b->type)!=8)return 0;x=a->type->kind;y=b->type->kind;return (x==TYPE_LONG&&y==TYPE_DOUBLE)||(x==TYPE_DOUBLE&&(y==TYPE_LONG||y==TYPE_DOUBLE));}
static unsigned eligibility(Type*t){return scalar(t)?XH_SYSV_PLAN_SCALAR_VERIFIED:xh_sysv_small_integer_struct(t)?XH_SYSV_PLAN_SMALL_INTEGER_AGGREGATE_VERIFIED:xh_sysv_small_sse_struct(t)?XH_SYSV_PLAN_SMALL_SSE_AGGREGATE_VERIFIED:xh_sysv_integer_pair_struct(t)?XH_SYSV_PLAN_INTEGER_PAIR_AGGREGATE_VERIFIED:xh_sysv_mixed_pair_struct(t)?XH_SYSV_PLAN_MIXED_PAIR_AGGREGATE_VERIFIED:0;}
static unsigned combine(unsigned current,unsigned next){if(!current||!next)return 0;return current==16||next==16?16:current==8||next==8?8:current==4||next==4?4:current==2||next==2?2:1;}
static int error(void){diag_tool(DIAG_SEV_ERROR,"SysV plan","unsupported type or invalid bounded ABI inspection plan",0,0,0,0,0,0);return -1;}
static int node(Graph*g,Type*t,unsigned depth,unsigned*out){unsigned id,i,count=0,first;StructField*f;int size,align,kind=0;
 if(!t||depth>=32||g->n>=128)return -1;for(i=0;i<depth;i++)if(g->path[i]==t)return -1;g->path[depth]=t;size=type_size(t);align=type_align(t);if(size<0||align<=0)return -1;id=g->n++;*out=id;memset(&g->nodes[id],0,sizeof(g->nodes[id]));g->nodes[id].size=(unsigned)size;g->nodes[id].alignment=(unsigned)align;
 switch(t->kind){case TYPE_CHAR:case TYPE_SHORT:case TYPE_INT:case TYPE_LONG:case TYPE_LLONG:case TYPE_BOOL:case TYPE_ENUM:case TYPE_WCHAR:kind=1;break;case TYPE_FLOAT:kind=2;break;case TYPE_DOUBLE:kind=3;break;case TYPE_PTR:kind=4;break;case TYPE_VOID:if(size)return -1;break;case TYPE_ARRAY:if(t->size<0)return -1;count=(unsigned)t->size;break;case TYPE_STRUCT:case TYPE_UNION:for(f=t->fields;f;f=f->next){if(f->bit_width||f->bit_offset||f->offset<0)return -1;count++;}break;default:return -1;}
 if(kind){g->nodes[id].form=1;g->nodes[id].kind=kind;return 0;}
 if(count>256-g->e)return -1;first=g->e;g->e+=count;g->nodes[id].first_edge=first;g->nodes[id].edge_count=count;
 if(t->kind==TYPE_ARRAY){int stride=type_size(t->base);if(stride<0)return -1;for(i=0;i<count;i++){unsigned child;if(node(g,t->base,depth+1,&child))return -1;g->edges[first+i].node=child;g->edges[first+i].offset=i*(unsigned)stride;}}
 else for(i=0,f=t->fields;f;f=f->next,i++){unsigned child;if(node(g,f->type,depth+1,&child))return -1;g->edges[first+i].node=child;g->edges[first+i].offset=(unsigned)f->offset;}
 return 0;
}
static int classify(const char*target,Type*t,XhTypeclassResult*out){Graph graph;XhTypeclassGraph input;unsigned root;if(t&&t->kind==TYPE_VOID){memset(out,0,sizeof(*out));return 0;}memset(&graph,0,sizeof(graph));if(node(&graph,t,0,&root))return -1;input.nodes=graph.nodes;input.node_count=graph.n;input.edges=graph.edges;input.edge_count=graph.e;return xh_typeclass_select(target,"typeclass",&input,root,out);}
int xh_build_sysv_plan(const char*target,const char*prefix,Type*fn,Type*const*actual,unsigned count,XhSysvPlan*out){XhSysvPlan plan;XhAbiPlanPolicy policy;XhAbiPlanState state;unsigned i;
 if(!out||!target||!prefix||!fn||fn->kind!=TYPE_FUNC||fn->param_count<0||count>64||count<(unsigned)fn->param_count||(!fn->is_variadic&&count!=(unsigned)fn->param_count))return error();memset(&plan,0,sizeof(plan));plan.version=XH_SYSV_PLAN_VERSION;plan.abi_id=1;plan.arg_count=count;plan.flags=eligibility(fn->base);
 for(i=0;i<(unsigned)fn->param_count;i++){Type adjusted;plan.flags=combine(plan.flags,fn->params?eligibility(adjust_parameter(fn->params[i],&adjusted)):0);}
 if(classify(target,fn->base,&plan.result)||xh_abiplan_load(target,prefix,&policy))return error();plan.sret=(unsigned)plan.result.memory;if(xh_sysv_small_sse_struct(fn->base)&&(plan.result.memory||plan.result.count!=1||plan.result.fragments[0].class_id!=2))return error();
 plan.rule_fingerprint=xh_template_fingerprint(target);
 if(xh_abiplan_begin(&policy,(int)plan.sret,&state))return error();
 for(i=0;i<count;i++){XhTypeclassResult c;Type adjusted;Type*t=actual?actual[i]:(i<(unsigned)fn->param_count?fn->params[i]:0);int size,align;t=adjust_parameter(t,&adjusted);if(!t||t->kind==TYPE_VOID||classify(target,t,&c))return error();if(xh_sysv_small_sse_struct(t)&&(c.memory||c.count!=1||c.fragments[0].class_id!=2))return error();plan.flags=combine(plan.flags,eligibility(t));if(i>=(unsigned)fn->param_count&&!scalar(t))plan.flags=0;size=type_size(t);align=type_align(t);if(size<0||align<1||xh_abiplan_argument(&policy,&state,&c,(unsigned)size,(unsigned)align,&plan.args[i]))return error();}
 if(xh_abiplan_finish(&policy,&state,&plan.stack_bytes))return error();*out=plan;return 0;
}
