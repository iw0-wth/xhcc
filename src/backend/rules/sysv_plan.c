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
 if(!out||!target||!prefix||!fn||fn->kind!=TYPE_FUNC||fn->param_count<0||count>64||count<(unsigned)fn->param_count||(!fn->is_variadic&&count!=(unsigned)fn->param_count))return error();memset(&plan,0,sizeof(plan));plan.version=XH_SYSV_PLAN_VERSION;plan.abi_id=1;plan.arg_count=count;plan.flags=scalar(fn->base)?XH_SYSV_PLAN_SCALAR_VERIFIED:0;
 for(i=0;i<(unsigned)fn->param_count;i++){Type adjusted;if(!fn->params||!scalar(adjust_parameter(fn->params[i],&adjusted)))plan.flags=0;}
 if(classify(target,fn->base,&plan.result)||xh_abiplan_load(target,prefix,&policy))return error();plan.sret=(unsigned)plan.result.memory;
 plan.rule_fingerprint=xh_template_fingerprint(target);
 if(xh_abiplan_begin(&policy,(int)plan.sret,&state))return error();
 for(i=0;i<count;i++){XhTypeclassResult c;Type adjusted;Type*t=actual?actual[i]:(i<(unsigned)fn->param_count?fn->params[i]:0);int size,align;t=adjust_parameter(t,&adjusted);if(!t||t->kind==TYPE_VOID||classify(target,t,&c))return error();if(!scalar(t))plan.flags=0;size=type_size(t);align=type_align(t);if(size<0||align<1||xh_abiplan_argument(&policy,&state,&c,(unsigned)size,(unsigned)align,&plan.args[i]))return error();}
 if(xh_abiplan_finish(&policy,&state,&plan.stack_bytes))return error();*out=plan;return 0;
}
