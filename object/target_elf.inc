/* Shared freestanding ELF ET_REL serializer. The assembler's image is retained
 * in one writable executable .text section so internal PC-relative distances
 * remain invariant. All absolute addresses and external references relocate.
 * No host assembler, linker, filesystem stdio or private object wrapper. */
#include "flash_fs.h"
typedef struct { const char *name; unsigned value,size; int bind,type,index,defined; } TeSym;
typedef struct { unsigned off; int sym,type,add; } TeRel;
static void te16(unsigned char *p,unsigned v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void te32(unsigned char *p,unsigned v){te16(p,v);te16(p+2,v>>16);}
static void te64(unsigned char *p,unsigned long long v){te32(p,(unsigned)v);te32(p+4,(unsigned)(v>>32));}
static unsigned tealign(unsigned v,unsigned a){return(v+a-1)&~(a-1);}
static int tesym(TeSym *s,int *n,int cap,const char *name){int i;for(i=0;i<*n;i++)if(ark_streq(s[i].name,name))return i;if(*n>=cap)return -1;i=(*n)++;s[i].name=name;s[i].value=s[i].size=0;s[i].bind=1;s[i].type=s[i].defined=0;return i;}
static int terel(TeRel*r,int*n,int cap,unsigned off,int sym,int type,int add){if(*n>=cap||sym<0)return -1;r[*n].off=off;r[*n].sym=sym;r[*n].type=type;r[*n].add=add;(*n)++;return 0;}
static int tewrite(const char*path,unsigned char*code,int len,int machine,int wide,TeSym*s,int n,TeRel*r,int nr){
 const char names[]="\0.text\0.rela.text\0.symtab\0.strtab\0.shstrtab\0.note.GNU-stack\0";
 unsigned off[7]={0},sz[7]={0},strsz=1,total,shoff;int i,j,locals=1,syment=wide?24:16,relent=wide?24:12,eh=wide?64:52,sh=wide?64:40;unsigned char*out;
 for(i=0;i<n;i++){if(!s[i].bind)locals++;strsz+=(unsigned)ark_strlen(s[i].name)+1;}
 {int l=1,g=locals;for(i=0;i<n;i++)s[i].index=s[i].bind?g++:l++;}
 sz[1]=(unsigned)len;sz[2]=(unsigned)nr*relent;sz[3]=(unsigned)(n+1)*syment;sz[4]=strsz;sz[5]=sizeof(names);
 total=(unsigned)eh;for(i=1;i<7;i++){total=tealign(total,i==1?16:i<=3?(wide?8:4):1);off[i]=total;total+=sz[i];}shoff=tealign(total,wide?8:4);total=shoff+7*sh;
 out=mem_pool_alloc((int)total);if(!out)goto fail;for(i=0;i<(int)total;i++)out[i]=0;
 out[0]=127;out[1]='E';out[2]='L';out[3]='F';out[4]=wide?2:1;out[5]=1;out[6]=1;te16(out+16,1);te16(out+18,(unsigned)machine);te32(out+20,1);
 if(wide){te64(out+40,shoff);te16(out+52,64);te16(out+58,64);te16(out+60,7);te16(out+62,5);}else{te32(out+32,shoff);te16(out+40,52);te16(out+46,40);te16(out+48,7);te16(out+50,5);}
 for(i=0;i<len;i++)out[off[1]+i]=code[i];strsz=1;
 for(i=0;i<n;i++){unsigned char*v=out+off[3]+s[i].index*syment;int z=ark_strlen(s[i].name)+1;te32(v,strsz);if(wide){v[4]=(unsigned char)((s[i].bind<<4)|s[i].type);te16(v+6,s[i].defined?1:0);te64(v+8,s[i].value);te64(v+16,s[i].size);}else{te32(v+4,s[i].value);te32(v+8,s[i].size);v[12]=(unsigned char)((s[i].bind<<4)|s[i].type);te16(v+14,s[i].defined?1:0);}for(j=0;j<z;j++)out[off[4]+strsz+j]=(unsigned char)s[i].name[j];strsz+=(unsigned)z;}
 for(i=0;i<nr;i++){unsigned char*v=out+off[2]+i*relent;if(wide){te64(v,r[i].off);te64(v+8,((unsigned long long)s[r[i].sym].index<<32)|(unsigned)r[i].type);te64(v+16,(unsigned long long)(long long)r[i].add);}else{te32(v,r[i].off);te32(v+4,((unsigned)s[r[i].sym].index<<8)|(unsigned)r[i].type);te32(v+8,(unsigned)r[i].add);}}
 for(i=0;i<(int)sizeof(names);i++)out[off[5]+i]=(unsigned char)names[i];
 for(i=1;i<7;i++){unsigned char*v=out+shoff+i*sh;unsigned nm=i==1?1:i==2?7:i==3?18:i==4?26:i==5?34:44;unsigned tp=i==2?4:i==3?2:i==4||i==5?3:1;unsigned flags=i==1?7:0,link=i==2?3:i==3?4:0,info=i==2?1:i==3?(unsigned)locals:0,al=i==1?16:i<=3?(wide?8:4):1,ent=i==2?(unsigned)relent:i==3?(unsigned)syment:0;te32(v,nm);te32(v+4,tp);if(wide){te64(v+8,flags);te64(v+24,off[i]);te64(v+32,sz[i]);te32(v+40,link);te32(v+44,info);te64(v+48,al);te64(v+56,ent);}else{te32(v+8,flags);te32(v+16,off[i]);te32(v+20,sz[i]);te32(v+24,link);te32(v+28,info);te32(v+32,al);te32(v+36,ent);}}
 if(fs_write_file(path,out,(int)total)!=(int)total){diag_tool(DIAG_SEV_ERROR,path,"cannot write ELF object",0,0,0,0,0,0);return ARK_ERR_IO;}return ARK_OK;
 fail:diag_tool(DIAG_SEV_ERROR,path,"ELF object exceeds compiler memory",0,0,0,0,0,0);return ARK_ERR_CODEGEN;
}
static int techeck(const char*path,IrProgram*p){int i;if(!p)return 0;for(i=0;i<p->global_count;i++){IrGlobal*g=p->globals[i];if(g->is_tls||g->section||g->has_at){diag_tool(DIAG_SEV_ERROR,path,"ELF TLS/custom sections/absolute data placement are not supported",0,0,0,0,0,0);return -1;}}for(i=0;i<p->func_count;i++){IrFunc*f=p->funcs[i];if(f->is_constructor||f->is_destructor||f->alias_target){diag_tool(DIAG_SEV_ERROR,path,"ELF constructors/destructors/aliases are not supported",0,0,0,0,0,0);return -1;}}return 0;}
static void temeta(TeSym*s,int n,IrProgram*p){int i,j;if(!p)return;for(i=0;i<n;i++){for(j=0;j<p->func_count;j++)if(ark_streq(s[i].name,p->funcs[j]->name)){s[i].type=2;if(s[i].defined)s[i].bind=p->funcs[j]->is_static?0:p->funcs[j]->is_weak?2:1;}for(j=0;j<p->global_count;j++)if(ark_streq(s[i].name,p->globals[j]->name)){s[i].type=1;s[i].size=(unsigned)type_size(p->globals[j]->type);if(s[i].defined)s[i].bind=p->globals[j]->is_static?0:1;}}}
