/* Shared normalized semantic record selector. Text storage belongs to the
 * template VM; parsed records are copied and one target table is cached. */
#include "xh_semantic.h"
#include "xh_template.h"
#include "diag.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef struct {char family[32],op[32],sign[24],form[24];int width;XhSemanticResult result;}Record;
static unsigned loaded_generation;
static Record*records;static int count;static char active_target[32],active_key[96];
void xh_semantic_release(void){free(records);loaded_generation=0;records=0;count=0;active_target[0]=active_key[0]=0;}
static int error(void){diag_tool(DIAG_SEV_ERROR,"semantic selector","invalid semantic records, duplicate or effect",0,0,0,0,0,0);return -1;}
static int identifier(const char*s,size_t cap){size_t n=0;if(!s||!*s)return 0;for(;*s;s++,n++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='.'||*s=='-'))return 0;return n<cap;}
static char*token(char**p){char*s=*p,*start;while(*s==' '||*s=='\t'||*s=='\r')s++;if(!*s)return 0;start=s;while(*s&&*s!=' '&&*s!='\t'&&*s!='\r')s++;if(*s)*s++=0;*p=s;return start;}
static int integer(const char*s,int max,int*out){char*end;long n;if(!s||!*s||*s=='-')return -1;errno=0;n=strtol(s,&end,10);if(*end||errno||n<0||n>max)return -1;*out=(int)n;return 0;}
static int record_order(const Record*a,const Record*b){int n=strcmp(a->family,b->family);if(n)return n;n=strcmp(a->op,b->op);if(n)return n;if(a->width!=b->width)return a->width<b->width?-1:1;n=strcmp(a->sign,b->sign);return n?n:strcmp(a->form,b->form);}
static int record_compare(const void*a,const void*b){return record_order((const Record*)a,(const Record*)b);}
static int load(const char*target,const char*key){const char*source=xh_template_get(target,key);char*text,*line;Record*next;int n=0,header=0,capacity=0;if(!source)return -1;
 if(records&&!strcmp(active_target,target)&&!strcmp(active_key,key)&&loaded_generation==xh_template_generation())return 0;
 xh_semantic_release();if(strlen(source)>32768)return error();text=malloc(strlen(source)+1);if(!text)return error();strcpy(text,source);
 {const char*p=source;while(*p){if(*p!='#'&&*p!='\n')capacity++;p=strchr(p,'\n');if(!p)break;p++;}}if(capacity<1||capacity>XH_SEMANTIC_MAX_RECORDS+1){free(text);return error();}next=calloc((size_t)capacity,sizeof(*next));if(!next){free(text);return error();}line=text;
 while(*line){char*end=strchr(line,'\n'),*comment,*p,*w[9];int fields=0,i,offset;Record*r;if(end)*end=0;comment=strchr(line,'#');if(comment)*comment=0;p=line;while(fields<9&&(w[fields]=token(&p))!=0)fields++;if(!fields)goto advance;if(!header){if(fields==2&&(!strcmp(w[0],"XHSEMANTIC")||!strcmp(w[0],"CSELECT"))&&!strcmp(w[1],"1")){header=1;goto advance;}if(fields==1&&!strcmp(w[0],"CSELECT1")){header=1;goto advance;}goto bad;}
 if((fields!=7&&fields!=8)||n==XH_SEMANTIC_MAX_RECORDS)goto bad;r=&next[n];if(!identifier(w[0],sizeof(r->family))||!identifier(w[1],sizeof(r->op))||integer(w[2],128,&r->width)||!identifier(w[3],sizeof(r->sign)))goto bad;strcpy(r->family,w[0]);strcpy(r->op,w[1]);strcpy(r->sign,w[3]);offset=fields==8?1:0;if(offset){if(!identifier(w[4],sizeof(r->form)))goto bad;strcpy(r->form,w[4]);}else strcpy(r->form,"-");if(!identifier(w[4+offset],sizeof(r->result.template_key))||integer(w[5+offset],16,&r->result.pop)||integer(w[6+offset],16,&r->result.push))goto bad;strcpy(r->result.template_key,w[4+offset]);for(i=0;i<n;i++)if(next[i].width==r->width&&!strcmp(next[i].family,r->family)&&!strcmp(next[i].op,r->op)&&!strcmp(next[i].sign,r->sign)&&!strcmp(next[i].form,r->form))goto bad;n++;
 advance:if(!end)break;line=end+1;}
 if(!header||!n)goto bad;qsort(next,(size_t)n,sizeof(*next),record_compare);free(text);loaded_generation=xh_template_generation();records=next;count=n;strcpy(active_target,target);strcpy(active_key,key);return 0;
 bad:free(text);free(next);return error();}
int xh_semantic_select(const char*target,const char*key,const XhSemanticInput*in,XhSemanticResult*out){int lo=0,hi;Record probe;if(!identifier(target,32)||!identifier(key,96)||!in||!out||!identifier(in->family,sizeof(probe.family))||!identifier(in->operation,sizeof(probe.op))||!identifier(in->sign,sizeof(probe.sign))||!identifier(in->form,sizeof(probe.form))||in->width<0||in->width>128)return error();strcpy(probe.family,in->family);strcpy(probe.op,in->operation);strcpy(probe.sign,in->sign);strcpy(probe.form,in->form);probe.width=in->width;if(load(target,key))return -1;hi=count;while(lo<hi){int mid=lo+(hi-lo)/2;int n=record_order(&records[mid],&probe);if(!n){*out=records[mid].result;return 1;}if(n<0)lo=mid+1;else hi=mid;}return 0;}
