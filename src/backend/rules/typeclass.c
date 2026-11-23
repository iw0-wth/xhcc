/* Generic bounded type-graph projection and finite class reduction.
 * Standalone only: switching shared-template targets invalidates raw template
 * pointers. Production integration must address that cache lifetime boundary. */
#include "xh_typeclass.h"
#include "xh_template.h"
#include "diag.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define MAX_CLASSES 8
#define MAX_LEAVES 32
#define MAX_CLEANUP 16
typedef struct {unsigned kind,class_id,alignment;} Leaf;
typedef struct {unsigned predicate,input,action,output;} Cleanup;
typedef struct {unsigned unit,limit,classes,empty,memory,nleaf,nclean;Leaf leaf[MAX_LEAVES];unsigned char merge[MAX_CLASSES][MAX_CLASSES];Cleanup cleanup[MAX_CLEANUP];} Rules;
static Rules *cache;static unsigned generation;static char active[32],prefix_saved[64];
void xh_typeclass_release(void){free(cache);cache=0;generation=0;active[0]=prefix_saved[0]=0;}
static int error(void){diag_tool(DIAG_SEV_ERROR,"type classifier","invalid, unsupported or oversized type graph/class rules",0,0,0,0,0,0);return -1;}
static int id(const char*s,unsigned cap){unsigned n=0;if(!s||!*s)return 0;for(;*s;s++,n++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='.'||*s=='-'))return 0;return n<cap;}
int xh_typeclass_set_path(const char *target,const char *path){char alias[32];if(!id(target,22))return error();snprintf(alias,sizeof(alias),"%s.aggregate",target);return xh_template_set_path(alias,path);}
static int fields(const char*t,unsigned*v,int n){int i;char*end;for(i=0;i<n;i++){unsigned long x;while(*t==' '||*t=='\t'||*t=='\r'||*t=='\n')t++;if(*t<'0'||*t>'9')return -1;errno=0;x=strtoul(t,&end,10);if(errno||x>65535||end==t||(*end&&*end!=' '&&*end!='\t'&&*end!='\r'&&*end!='\n'))return -1;v[i]=(unsigned)x;t=end;}while(*t==' '||*t=='\t'||*t=='\r'||*t=='\n')t++;return *t?-1:0;}
static const char *get(const char*t,const char*p,const char*k){char key[96];snprintf(key,sizeof(key),"%s.%s",p,k);return xh_template_get(t,key);}
static int load(const char*t,const char*p){Rules*r;const char*s;unsigned v[6],i,j;char key[40];unsigned char seen[MAX_CLASSES][MAX_CLASSES]={{0}};
 s=get(t,p,"config");if(!s)return -1;if(cache&&!strcmp(t,active)&&!strcmp(p,prefix_saved)&&generation==xh_template_generation())return 0;
 xh_typeclass_release();r=calloc(1,sizeof(*r));if(!r)return error();
 if(fields(s,v,6)||v[0]!=1||!v[1]||v[1]>16||!v[2]||v[2]>2*v[1]||v[3]<1||v[3]>MAX_CLASSES||v[4]>=v[3]||v[5]>=v[3]||v[4]==v[5])goto bad;
 r->unit=v[1];r->limit=v[2];r->classes=v[3];r->empty=v[4];r->memory=v[5];
 s=get(t,p,"leaf.count");if(!s||fields(s,v,1)||!v[0]||v[0]>MAX_LEAVES)goto bad;r->nleaf=v[0];
 for(i=0;i<r->nleaf;i++){snprintf(key,sizeof(key),"leaf.%u",i);s=get(t,p,key);if(!s||fields(s,v,3)||v[0]>255||v[1]>=r->classes||!v[2]||v[2]>4096||(v[2]&(v[2]-1)))goto bad;for(j=0;j<i;j++)if(r->leaf[j].kind==v[0])goto bad;r->leaf[i].kind=v[0];r->leaf[i].class_id=v[1];r->leaf[i].alignment=v[2];}
 s=get(t,p,"merge.count");if(!s||fields(s,v,1)||v[0]!=r->classes*r->classes)goto bad;
 for(i=0;i<r->classes*r->classes;i++){snprintf(key,sizeof(key),"merge.%u",i);s=get(t,p,key);if(!s||fields(s,v,3)||v[0]>=r->classes||v[1]>=r->classes||v[2]>=r->classes||seen[v[0]][v[1]])goto bad;seen[v[0]][v[1]]=1;r->merge[v[0]][v[1]]=(unsigned char)v[2];}
 s=get(t,p,"cleanup.count");if(!s||fields(s,v,1)||v[0]>MAX_CLEANUP)goto bad;r->nclean=v[0];
 /* Predicates: 0 any(class); 1 unaligned; 2 current(class) with previous
  * not class(output). Actions: 0 MEMORY; 1 replace current with output. */
 for(i=0;i<r->nclean;i++){snprintf(key,sizeof(key),"cleanup.%u",i);s=get(t,p,key);if(!s||fields(s,v,4)||v[0]>2||v[1]>=r->classes||v[2]>1||v[3]>=r->classes)goto bad;r->cleanup[i].predicate=v[0];r->cleanup[i].input=v[1];r->cleanup[i].action=v[2];r->cleanup[i].output=v[3];}
 cache=r;generation=xh_template_generation();strcpy(active,t);strcpy(prefix_saved,p);return 0;
 bad:free(r);return error();
}
typedef struct {unsigned node,offset,next;} Frame;
int xh_typeclass_select(const char*t,const char*p,const XhTypeclassGraph*g,unsigned root,XhTypeclassResult*out){Frame stack[XH_TYPECLASS_MAX_DEPTH];unsigned char path[XH_TYPECLASS_MAX_NODES]={0};unsigned cls[2],count,depth=0,visits=0,i,j;int unaligned=0,memory=0;unsigned rootsize;
 if(out)memset(out,0,sizeof(*out));if(!out||!id(t,32)||!id(p,64)||!g||!g->nodes||g->node_count<1||g->node_count>XH_TYPECLASS_MAX_NODES||g->edge_count>XH_TYPECLASS_MAX_EDGES||(g->edge_count&&!g->edges)||root>=g->node_count)return error();
 if(load(t,p))return -1;rootsize=g->nodes[root].size;
 /* Structural validation is independent of early MEMORY classification. */
 for(i=0;i<g->node_count;i++){const XhTypeclassNode*n=&g->nodes[i];if(n->form>1||!n->alignment||n->alignment>4096||(n->alignment&(n->alignment-1))||n->first_edge>g->edge_count||n->edge_count>g->edge_count-n->first_edge||(n->form==XH_TYPECLASS_LEAF&&n->edge_count))return error();for(j=0;j<n->edge_count;j++){const XhTypeclassEdge*e=&g->edges[n->first_edge+j];if(e->node>=g->node_count||e->offset>n->size||g->nodes[e->node].size>n->size-e->offset)return error();}}
 if(rootsize>cache->limit)memory=1;
 count=memory?0:(rootsize+cache->unit-1)/cache->unit;for(i=0;i<2;i++)cls[i]=cache->empty;
 stack[0].node=root;stack[0].offset=0;stack[0].next=0;path[root]=1;depth=1;
 while(depth){Frame*f=&stack[depth-1];const XhTypeclassNode*n=&g->nodes[f->node];
  if(!f->next){if(++visits>XH_TYPECLASS_MAX_VISITS)goto bad;if(f->offset%n->alignment)unaligned=1;}
  if(n->form==XH_TYPECLASS_LEAF){const Leaf*leaf=0;unsigned end;
   for(i=0;i<cache->nleaf;i++)if(cache->leaf[i].kind==(unsigned)n->kind){leaf=&cache->leaf[i];break;}if(!leaf)goto bad;
   if(f->offset%leaf->alignment || g->nodes[root].alignment<leaf->alignment)unaligned=1;
   end=f->offset+n->size;if(end>rootsize)goto bad;
   if(!memory&&n->size)for(i=f->offset/cache->unit;i<=(end-1)/cache->unit;i++){if(i>=2)goto bad;cls[i]=cache->merge[cls[i]][leaf->class_id];}
   path[f->node]=0;depth--;continue;
  }
  if(f->next<n->edge_count){const XhTypeclassEdge*e=&g->edges[n->first_edge+f->next++];unsigned offset=f->offset+e->offset;if(depth==XH_TYPECLASS_MAX_DEPTH||path[e->node]||offset>rootsize)goto bad;stack[depth].node=e->node;stack[depth].offset=offset;stack[depth].next=0;path[e->node]=1;depth++;}
  else{path[f->node]=0;depth--;}
 }
 for(i=0;i<cache->nclean;i++){Cleanup*c=&cache->cleanup[i];for(j=0;j<count;j++){int match=c->predicate==1?unaligned:c->predicate==0?cls[j]==c->input:cls[j]==c->input&&(j==0||cls[j-1]!=c->output);if(match){if(c->action==0)memory=1;else cls[j]=c->output;}}}
 for(i=0;i<count;i++)if(cls[i]==cache->memory)memory=1;
 if(memory){out->memory=1;return 0;}out->count=count;for(i=0;i<count;i++){out->fragments[i].offset=i*cache->unit;out->fragments[i].size=rootsize-i*cache->unit<cache->unit?rootsize-i*cache->unit:cache->unit;out->fragments[i].class_id=(int)cls[i];}return 0;
 bad:memset(out,0,sizeof(*out));return error();
}
