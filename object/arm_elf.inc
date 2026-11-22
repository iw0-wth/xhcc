/* Included by backend_arm.c: consume the real assembler labels/references and
 * IR initializers, never the lossy ARKOBJ export. All scratch storage is arena
 * allocated in proportion to this translation unit. ELF32 little endian REL. */
#include "flash_fs.h"
typedef struct { const char *name; unsigned value,size; int sec,bind,type,index; } XhSym;
typedef struct { unsigned off; int sym,type,sec; } XhRel;
typedef struct { unsigned name,type,flags,off,size,link,info,align,entsize; } XhSec;
static void xh_w16(unsigned char *p,unsigned v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void xh_w32(unsigned char *p,unsigned v){xh_w16(p,v);xh_w16(p+2,v>>16);}
static unsigned xh_align(unsigned n,unsigned a){return (n+a-1)&~(a-1);}
static int xh_find(XhSym *s,int n,const char *name){int i;for(i=0;i<n;i++)if(ark_streq(s[i].name,name))return i;return -1;}
static int xh_sym(XhSym *s,int *n,int cap,const char *name){int i=xh_find(s,*n,name);if(i>=0)return i;if(*n>=cap)return -1;i=(*n)++;s[i].name=name;s[i].value=s[i].size=0;s[i].sec=0;s[i].bind=1;s[i].type=0;return i;}
static int xh_rel(XhRel *r,int *n,int cap,unsigned off,int sym,int type,int sec){if(*n>=cap||sym<0)return -1;r[*n].off=off;r[*n].sym=sym;r[*n].type=type;r[*n].sec=sec;(*n)++;return 0;}
static void xh_mov_zero(unsigned char *p){unsigned h=p[0]|((unsigned)p[1]<<8),l=p[2]|((unsigned)p[3]<<8);h&=~0x40fu;l&=~0x70ffu;xh_w16(p,h);xh_w16(p+2,l);}
int xhcc_arm_write_elf(const char *path,unsigned char *code,int code_len){
    /* Raw assembly has no IR data layout contract. The public driver rejects it. */
    ArmAsm *a=&xh_elf_asm;IrProgram *p=xh_elf_prog;
    int i,j,n=0,nr=0,cap=a->label_count+a->set_count+a->ref_count+(p?p->global_count*3:0)+32;
    int rcap=a->ref_count*2+(p?p->func_count*2:0)+16,local_count=1;unsigned dsize=0,bsize=0,dalign=8,balign=8,total,strsz=1,stroff=0;
    XhSym *s;XhRel *r;unsigned char *data,*out;int *gidx;unsigned *goff;XhSec *sec;int nsec=14,custom_count=0;char *names;unsigned names_size;const char **custom_names;
    const char base_names[]="\0.text\0.data\0.bss\0.rel.text\0.rel.data\0.symtab\0.strtab\0.shstrtab\0.ARM.attributes\0.init_array\0.fini_array\0.rel.init_array\0.rel.fini_array\0";
    /* aeabi, Tag_File: v7-M, microcontroller, Thumb-2, base AAPCS. */
    const unsigned char attrs[]={65,33,0,0,0,97,101,97,98,105,0,1,23,0,0,0,6,10,7,77,8,0,9,2,18,4,20,1,21,1,24,1,25,1};
    if(!p){diag_tool(DIAG_SEV_ERROR,path,"ELF assembly input is not supported",0,0,0,0,0,0);return ARK_ERR_CODEGEN;}
    for(i=0;i<p->global_count;i++)rcap+=p->globals[i]->init_ref_count+1;
    custom_names=mem_pool_alloc((p->global_count+1)*(int)sizeof(char*));if(!custom_names)goto fail;
    for(i=0;i<p->global_count;i++){const char*nm=p->globals[i]->section;if(!nm)continue;for(j=0;j<custom_count;j++)if(ark_streq(custom_names[j],nm))break;if(j==custom_count)custom_names[custom_count++]=nm;}
    nsec+=custom_count*2;sec=mem_pool_alloc(nsec*(int)sizeof(*sec));if(!sec)goto fail;
    for(i=0;i<nsec;i++){unsigned*v=(unsigned*)&sec[i];for(j=0;j<9;j++)v[j]=0;}
    names_size=sizeof(base_names);for(i=0;i<custom_count;i++)names_size+=(unsigned)(ark_strlen(custom_names[i])*2)+7;
    names=mem_pool_alloc(names_size);if(!names)goto fail;for(i=0;i<(int)sizeof(base_names);i++)names[i]=base_names[i];
    {unsigned ns=sizeof(base_names);for(i=0;i<custom_count;i++){int len=ark_strlen(custom_names[i])+1;sec[14+i].name=ns;for(j=0;j<len;j++)names[ns+j]=custom_names[i][j];ns+=(unsigned)len;sec[14+i].type=1;sec[14+i].flags=3;sec[14+i].align=8;sec[14+custom_count+i].name=ns;names[ns++]='.';names[ns++]='r';names[ns++]='e';names[ns++]='l';for(j=0;j<len;j++)names[ns+j]=custom_names[i][j];ns+=(unsigned)len;sec[14+custom_count+i].type=9;sec[14+custom_count+i].link=6;sec[14+custom_count+i].info=14+i;sec[14+custom_count+i].align=4;sec[14+custom_count+i].entsize=8;}}
    s=mem_pool_alloc(cap*(int)sizeof(*s));r=mem_pool_alloc(rcap*(int)sizeof(*r));gidx=mem_pool_alloc((p->global_count+1)*(int)sizeof(int));
    goff=mem_pool_alloc((p->global_count+1)*(int)sizeof(unsigned));if(!s||!r||!gidx||!goff)goto fail;
    /* Define assembler labels, retaining locals required by absolute references. */
    for(i=0;i<a->label_count;i++){
        AsmLabel *l=&a->labels[i];int k=xh_sym(s,&n,cap,l->name);if(k<0)goto fail;
        s[k].sec=1;s[k].value=(unsigned)l->addr;s[k].bind=l->name[0]=='.'?0:(l->is_weak?2:1);s[k].type=l->is_func?2:0;
        if(l->is_func)s[k].value|=1;
        for(j=0;j<p->func_count;j++)if(ark_streq(p->funcs[j]->name,l->name)){
            IrFunc *f=p->funcs[j];s[k].bind=f->is_static?0:(f->is_weak?2:1);s[k].type=2;s[k].value|=1;
            if(f->alias_target){diag_tool(DIAG_SEV_ERROR,path,"ELF aliases are not supported",0,0,0,0,0,0);return ARK_ERR_CODEGEN;}
        }
    }
    for(i=0;i<a->set_count;i++){
        int k=xh_sym(s,&n,cap,a->sets[i].name);if(k<0)goto fail;s[k].sec=0xfff1;s[k].value=(unsigned)a->sets[i].addr;
    }
    for(i=0;i<p->global_count;i++){
        IrGlobal *g=p->globals[i];unsigned al=g->align_bytes>8?(unsigned)g->align_bytes:8u;int k=xh_sym(s,&n,cap,g->name);if(k<0)goto fail;gidx[i]=k;
        s[k].type=1;s[k].bind=g->is_static?0:1;s[k].size=(unsigned)(g->type?type_size(g->type):4);if(!s[k].size)s[k].size=4;
        if(g->is_extern)continue;
        if(g->is_tls){diag_tool(DIAG_SEV_ERROR,path,"ELF TLS is not supported",0,0,0,0,0,0);return ARK_ERR_CODEGEN;}
        if(g->has_at){s[k].sec=0xfff1;s[k].value=g->at_addr;continue;}
        if(al&(al-1))goto fail;
        if(g->section){int cs;for(cs=0;cs<custom_count;cs++)if(ark_streq(custom_names[cs],g->section))break;cs+=14;sec[cs].size=xh_align(sec[cs].size,al);s[k].sec=cs;s[k].value=sec[cs].size;sec[cs].size+=s[k].size;if(al>sec[cs].align)sec[cs].align=al;dsize=xh_align(dsize,al);goff[i]=dsize;dsize+=s[k].size;}
        else if(g->has_init){dsize=xh_align(dsize,al);s[k].sec=2;s[k].value=dsize;goff[i]=dsize;dsize+=s[k].size;if(al>dalign)dalign=al;}
        else{bsize=xh_align(bsize,al);s[k].sec=3;s[k].value=bsize;bsize+=s[k].size;if(al>balign)balign=al;}
    }
    data=mem_pool_alloc(dsize?dsize:1);if(!data)goto fail;for(i=0;i<(int)dsize;i++)data[i]=0;
    for(i=0;i<p->global_count;i++){
        IrGlobal *g=p->globals[i];XhSym *z=&s[gidx[i]];unsigned char *dst;int sz=(int)z->size;
        if(z->sec!=2&&z->sec<14)continue;dst=data+goff[i];
        if(g->init_sym){
            int k=xh_sym(s,&n,cap,g->init_sym);xh_w32(dst,(unsigned)g->init_sym_off);if(xh_rel(r,&nr,rcap,z->value,k,2,z->sec))goto fail;
        }else if(g->init_aggregate){
            if(ark_agg_fill_ex(g->type,g->init_aggregate,dst,sz,arm_agg_addr_hook,g))goto fail;
            for(j=0;j<g->init_ref_count;j++){
                IrSymRef *q=&g->init_refs[j];char label[80];const char *nm=q->sym;int k;
                if(q->offset<0||q->offset+4>sz)goto fail;
                if(q->str){snprintf(label,sizeof(label),".L_gstr_%d_%d",i,q->offset);nm=label;}
                k=xh_sym(s,&n,cap,nm);if(k<0)goto fail;
                /* Generated string label must already exist; never retain stack label. */
                if(q->str&&s[k].sec==0)goto fail;
                xh_w32(dst+q->offset,(unsigned)q->add);if(xh_rel(r,&nr,rcap,z->value+q->offset,k,2,z->sec))goto fail;
            }
        }else if(g->init_str){
            if(g->type&&g->type->kind==TYPE_PTR){char label[64];int k;snprintf(label,sizeof(label),".L_gstr_%d",i);k=xh_find(s,n,label);if(xh_rel(r,&nr,rcap,z->value,k,2,z->sec))goto fail;}
            else{int len=ark_strlen(g->init_str);if(len>sz)len=sz;for(j=0;j<len;j++)dst[j]=(unsigned char)g->init_str[j];}
        }else if(g->is_float){
            Type *bt=g->type;int cx=bt&&ARK_IS_CX(bt);if(cx)bt=bt->base;
            if(bt&&bt->kind==TYPE_DOUBLE){union{double d;unsigned char b[8];}v;v.d=g->init_float;for(j=0;j<8&&j<sz;j++)dst[j]=v.b[j];if(cx){v.d=g->init_float_im;for(j=0;j<8&&j+8<sz;j++)dst[j+8]=v.b[j];}}
            else{union{float f;unsigned char b[4];}v;v.f=(float)g->init_float;for(j=0;j<4&&j<sz;j++)dst[j]=v.b[j];if(cx){v.f=(float)g->init_float_im;for(j=0;j<4&&j+4<sz;j++)dst[j+4]=v.b[j];}}
        }else{unsigned long long v=(unsigned long long)g->init_int;for(j=0;j<sz&&j<8;j++)dst[j]=(unsigned char)(v>>(j*8));}
    }
    for(i=0;i<a->ref_count;i++){
        AsmRef *q=&a->refs[i];int k,tp=q->type;unsigned off=(unsigned)q->pc;
        if(tp==0||tp==7||tp==8||tp==9)continue;
        if((tp==1||tp==2||tp==3)&&q->name[0]=='.')continue;
        if(off+4>(unsigned)code_len)goto fail;
        k=xh_sym(s,&n,cap,q->name);if(k<0)goto fail;
        if(tp==10||tp==11){if(off+8>(unsigned)code_len)goto fail;xh_mov_zero(code+off);xh_mov_zero(code+off+4);if(xh_rel(r,&nr,rcap,off,k,47,1)||xh_rel(r,&nr,rcap,off+4,k,48,1))goto fail;}
        else if(tp==4){xh_w32(code+off,0);if(xh_rel(r,&nr,rcap,off,k,2,1))goto fail;}
        else if(tp>=1&&tp<=3){
            /* REL addend -4 accounts for Thumb's PC+4 pipeline bias. */
            if(tp==1){xh_w16(code+off,0xf7ff);xh_w16(code+off+2,0xfffe);}
            else if(tp==2){xh_w16(code+off,0xf7ff);xh_w16(code+off+2,0xbffe);}
            else{unsigned hi=code[off]|((unsigned)code[off+1]<<8);xh_w16(code+off,0xf43f|(hi&0x3c0));xh_w16(code+off+2,0xaffe);}
            if(xh_rel(r,&nr,rcap,off,k,tp==1?10:(tp==2?30:51),1))goto fail;
        }else{diag_tool(DIAG_SEV_ERROR,path,"private ARM relocation reached ELF writer",0,0,0,0,0,0);return ARK_ERR_CODEGEN;}
    }
    for(i=0;i<p->func_count;i++){IrFunc*f=p->funcs[i];int k=xh_find(s,n,f->name);if(k<0)continue;if(f->is_constructor){if(xh_rel(r,&nr,rcap,sec[10].size,k,2,10))goto fail;sec[10].size+=4;}if(f->is_destructor){if(xh_rel(r,&nr,rcap,sec[11].size,k,2,11))goto fail;sec[11].size+=4;}}
    for(i=0;i<n;i++){if(!s[i].bind)local_count++;strsz+=(unsigned)ark_strlen(s[i].name)+1;}
    {int lo=1,gl=local_count;for(i=0;i<n;i++)s[i].index=s[i].bind?gl++:lo++;}

    sec[1].name=1;sec[1].type=1;sec[1].flags=6;sec[1].size=(unsigned)code_len;sec[1].align=4;
    sec[2].name=7;sec[2].type=1;sec[2].flags=3;sec[2].size=dsize;sec[2].align=dalign;
    sec[3].name=13;sec[3].type=8;sec[3].flags=3;sec[3].size=bsize;sec[3].align=balign;
    for(i=4;i<=5;i++){sec[i].name=i==4?18:28;sec[i].type=9;sec[i].link=6;sec[i].info=i==4?1:2;sec[i].align=4;sec[i].entsize=8;for(j=0;j<nr;j++)if(r[j].sec==(i==4?1:2))sec[i].size+=8;}
    sec[6].name=38;sec[6].type=2;sec[6].size=(unsigned)(n+1)*16;sec[6].link=7;sec[6].info=(unsigned)local_count;sec[6].align=4;sec[6].entsize=16;
    sec[7].name=46;sec[7].type=3;sec[7].size=strsz;sec[7].align=1;
    sec[8].name=54;sec[8].type=3;sec[8].size=names_size;sec[8].align=1;
    sec[9].name=64;sec[9].type=0x70000003;sec[9].size=sizeof(attrs);sec[9].align=1;
    sec[10].name=80;sec[10].type=14;sec[10].flags=3;sec[10].align=4;sec[10].entsize=4;
    sec[11].name=92;sec[11].type=15;sec[11].flags=3;sec[11].align=4;sec[11].entsize=4;
    for(i=12;i<14;i++){sec[i].name=i==12?104:120;sec[i].type=9;sec[i].link=6;sec[i].info=i-2;sec[i].align=4;sec[i].entsize=8;}
    for(i=0;i<nr;i++){int rs=r[i].sec;if(rs>=10){int ri=rs<12?rs+2:rs+custom_count;sec[ri].size+=8;}}
    total=52;for(i=1;i<nsec;i++){total=xh_align(total,sec[i].align);sec[i].off=total;if(i!=3)total+=sec[i].size;}total=xh_align(total,4);stroff=total;total+=nsec*40;
    out=mem_pool_alloc(total);if(!out)goto fail;for(i=0;i<(int)total;i++)out[i]=0;
    out[0]=127;out[1]='E';out[2]='L';out[3]='F';out[4]=1;out[5]=1;out[6]=1;
    xh_w16(out+16,1);xh_w16(out+18,40);xh_w32(out+20,1);xh_w32(out+32,stroff);xh_w32(out+36,0x05000000);xh_w16(out+40,52);xh_w16(out+46,40);xh_w16(out+48,nsec);xh_w16(out+50,8);
    for(i=0;i<code_len;i++)out[sec[1].off+i]=code[i];for(i=0;i<(int)dsize;i++)out[sec[2].off+i]=data[i];for(i=0;i<p->global_count;i++){XhSym*z=&s[gidx[i]];if(z->sec>=14&&z->sec<nsec)for(j=0;j<(int)z->size;j++)out[sec[z->sec].off+z->value+j]=data[goff[i]+j];}
    strsz=1;for(i=0;i<n;i++){unsigned char *v=out+sec[6].off+s[i].index*16;int len=ark_strlen(s[i].name)+1;xh_w32(v,strsz);xh_w32(v+4,s[i].value);xh_w32(v+8,s[i].size);v[12]=(unsigned char)((s[i].bind<<4)|s[i].type);xh_w16(v+14,(unsigned)s[i].sec);for(j=0;j<len;j++)out[sec[7].off+strsz+j]=(unsigned char)s[i].name[j];strsz+=(unsigned)len;}
    {unsigned*rp=mem_pool_alloc(nsec*(int)sizeof(unsigned));if(!rp)goto fail;for(i=0;i<nsec;i++)rp[i]=sec[i].off;for(i=0;i<nr;i++){int rs=r[i].sec,ri=rs==1?4:rs==2?5:rs<12?rs+2:rs+custom_count;unsigned char*v=out+rp[ri];xh_w32(v,r[i].off);xh_w32(v+4,((unsigned)s[r[i].sym].index<<8)|(unsigned)r[i].type);rp[ri]+=8;}}
    for(i=0;i<(int)names_size;i++)out[sec[8].off+i]=(unsigned char)names[i];
    for(i=0;i<(int)sizeof(attrs);i++)out[sec[9].off+i]=attrs[i];
    for(i=1;i<nsec;i++){unsigned char *v=out+stroff+i*40;xh_w32(v,sec[i].name);xh_w32(v+4,sec[i].type);xh_w32(v+8,sec[i].flags);xh_w32(v+16,sec[i].off);xh_w32(v+20,sec[i].size);xh_w32(v+24,sec[i].link);xh_w32(v+28,sec[i].info);xh_w32(v+32,sec[i].align);xh_w32(v+36,sec[i].entsize);}
    if(fs_write_file(path,out,(int)total)!=(int)total){diag_tool(DIAG_SEV_ERROR,path,"cannot write ELF object",0,0,0,0,0,0);return ARK_ERR_IO;}return ARK_OK;
fail:diag_tool(DIAG_SEV_ERROR,path,"ELF object exceeds memory or unsupported initializer",0,0,0,0,0,0);return ARK_ERR_CODEGEN;
}
