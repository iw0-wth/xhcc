/* Shared bounded ELF32 linker, independently implemented from ELF and psABIs. */
#include "xh_elf_link.h"
#include "xh_elf_read.h"
#include "xh_arm_elf_reloc.h"
#include "xh_rv_elf_reloc.h"
#include "xh_ld_script.h"
#include "flash_fs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>

typedef struct {
    uint32_t type,flags,size,align,name,addr,off,lma;
    uint16_t out,group;
} Sec;
typedef struct {
    uint32_t name,value,size,resolved;
    int32_t winner; /* global symbol index; -1 means this record */
    uint16_t shndx;
    uint8_t bind,type,other;
} Sym;
typedef struct {
    Sec *sec; Sym *sym; char *names;
    size_t secbytes,symbytes,namebytes;
    unsigned ns,ny,tab,base;
    uint32_t fingerprint;
    int filelen;
} Obj;
typedef struct {
    uint32_t addr,lma,size,filesz,off,flags,align;
} Group;
typedef struct {
    XhLdScript ir;
    Group groups[XH_LD_MAX_SECTIONS];
    uint32_t dot[XH_LD_MAX_SYMBOLS],value[XH_LD_MAX_SYMBOLS];
    uint32_t assert_dot[XH_LD_MAX_ASSERTIONS];
    unsigned char state[XH_LD_MAX_SYMBOLS],ready[XH_LD_MAX_SECTIONS];
    unsigned char common_seen[XH_ELF_LINK_MAX_SYMBOLS];
    uint32_t common_start,common_end;
    unsigned common_group,has_common;
} Script;
typedef struct { XhElfTarget id;unsigned machine;uint32_t flags;unsigned thumb; } Target;
static const Target arm_target={XH_ELF_TARGET_ARM,40,0x05000000u,1};
static const Target rv_target={XH_ELF_TARGET_RISCV32,243,0,0};
typedef struct {
    Obj obj[XH_ELF_LINK_MAX_OBJECTS];
    const Target *target;
    Script *script;

    size_t used,limit,peak;
    unsigned objects;
    char *err; size_t errcap;
    unsigned total_symbols,total_sections;
} Link;
static int fail(Link *l,const char *fmt,...) {
    va_list ap;
    if(l->err && l->errcap) { va_start(ap,fmt); vsnprintf(l->err,l->errcap,fmt,ap); va_end(ap); l->err[l->errcap-1]=0; }
    return -1;
}
static void *take(Link *l,size_t n) {
    void *p;
    if(!n)n=1;
    if(l->used>l->limit || n>l->limit-l->used) { fail(l,"ELF link memory budget exceeded"); return NULL; }
    p=malloc(n);
    if(!p) { fail(l,"ELF link allocation failed"); return NULL; }
    l->used+=n;if(l->used>l->peak)l->peak=l->used;return p;
}
static void drop(Link *l,void *p,size_t n) { if(p) { free(p); l->used-=n?n:1; } }
static int plus(uint32_t a,uint32_t b,uint32_t *v) { if(b>UINT32_MAX-a)return -1;*v=a+b;return 0; }
static int aligned(uint32_t a,uint32_t b,uint32_t *v) { if(!b)b=1; if(b>65536u || (b&(b-1u)) || a>UINT32_MAX-(b-1u))return -1;*v=(a+b-1u)&~(b-1u);return 0; }
static uint32_t fingerprint(const unsigned char *p,size_t n) { uint32_t h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h; }
static unsigned char *load(Link *l,const char *path,int *len,XhElfView *v) {
    unsigned char *p; int n=fs_file_size(path),got;
    if(n<52 || (unsigned)n>XH_ELF_LINK_MAX_MEMORY) { fail(l,"invalid or oversized ELF input: %s",path);return NULL; }
    p=(unsigned char *)take(l,(size_t)n+1u); if(!p)return NULL;
    got=fs_read_file(path,(char *)p,n+1);
    if(got!=n || fs_file_size(path)!=n) { fail(l,"ELF input read failed or changed: %s",path);drop(l,p,(size_t)n+1u);return NULL; }
    if(xh_elf_open_target(p,(size_t)n,l->target->id,v) || (l->target->thumb && xh_elf_validate_thumb(v))) { fail(l,"invalid or unsupported target ELF: %s",path);drop(l,p,(size_t)n+1u);return NULL; }
    *len=n;return p;
}
static Sym *symbol(Link *l,unsigned index,Obj **owner) {
    unsigned i;
    for(i=0;i<XH_ELF_LINK_MAX_OBJECTS;i++) { Obj *o=&l->obj[i];if(index>=o->base && index-o->base<o->ny) {if(owner)*owner=o;return &o->sym[index-o->base];} }
    return NULL;
}
static const char *symname(Link *l,unsigned index) { Obj *o;Sym *s=symbol(l,index,&o);return o->names+s->name; }
static int rank_sym(const Sym *s) {
    if(!s->shndx)return 0;
    if(s->shndx==XH_ELF_SHN_COMMON)return 2;
    return s->bind==XH_ELF_STB_WEAK?1:3;
}
static int scan(Link *l,const char *path,Obj *o) {
    XhElfView v;unsigned char *p;unsigned i;size_t names=0;int len;
    p=load(l,path,&len,&v);if(!p)return -1;
    o->ns=v.shnum;o->base=l->total_symbols;o->filelen=len;o->fingerprint=fingerprint(p,(size_t)len);
    if(o->ns>XH_ELF_LINK_MAX_SECTIONS-l->total_sections) {fail(l,"ELF link section limit exceeded");goto bad;}
    for(i=1;i<v.shnum;i++) {
        XhElfSection s;xh_elf_section(&v,i,&s);
        if(s.type==XH_ELF_SHT_SYMTAB) {if(o->tab){fail(l,"multiple SYMTAB sections unsupported");goto bad;}o->tab=i;o->ny=s.size/16u;}
        if(s.flags&2u) {
            if((s.flags&~7u) || ((s.flags&5u)==5u) || (s.type!=1u && s.type!=8u && s.type!=14u && s.type!=15u) || (s.type==8u && !(s.flags&1u))) { fail(l,"unsupported alloc section flags/type: %s",s.name);goto bad; }
        }
        names+=strlen(s.name)+1u;
        if(names>XH_ELF_LINK_MAX_NAMES){fail(l,"ELF name limit exceeded");goto bad;}
    }
    if(o->ny>XH_ELF_LINK_MAX_SYMBOLS-l->total_symbols){fail(l,"ELF link symbol limit exceeded");goto bad;}
    for(i=0;i<o->ny;i++) {
        XhElfSymbol s;xh_elf_symbol(&v,o->tab,i,&s);
        if(s.bind>2u || (l->target->thumb && s.type==2u && s.shndx && !(s.value&1u))) {fail(l,"unsupported symbol binding or non-Thumb function");goto bad;}
        names+=strlen(s.name)+1u;
        if(names>XH_ELF_LINK_MAX_NAMES){fail(l,"ELF name limit exceeded");goto bad;}
    }
    { size_t aggregate=names;unsigned k;for(k=0;k<XH_ELF_LINK_MAX_OBJECTS;k++)aggregate+=l->obj[k].namebytes;
      if(aggregate>XH_ELF_LINK_MAX_NAMES){fail(l,"ELF aggregate name limit exceeded");goto bad;} }
    o->secbytes=(size_t)o->ns*sizeof(Sec);o->symbytes=(size_t)o->ny*sizeof(Sym);o->namebytes=names;
    o->sec=(Sec *)take(l,o->secbytes);if(!o->sec)goto bad;
    if(o->ny){o->sym=(Sym *)take(l,o->symbytes);if(!o->sym)goto bad;}
    o->names=(char *)take(l,names);if(!o->names)goto bad;
    memset(o->sec,0,o->secbytes);names=0;
    for(i=1;i<o->ns;i++) {
        XhElfSection s;Sec *t=&o->sec[i];size_t z;xh_elf_section(&v,i,&s);
        t->type=s.type;t->flags=s.flags;t->size=s.size;t->align=s.align?s.align:1u;t->name=(uint32_t)names;
        z=strlen(s.name)+1u;memcpy(o->names+names,s.name,z);names+=z;
    }
    for(i=0;i<o->ny;i++) {
        XhElfSymbol s;Sym *t=&o->sym[i];size_t z;xh_elf_symbol(&v,o->tab,i,&s);
        t->name=(uint32_t)names;t->value=s.value;t->size=s.size;t->shndx=s.shndx;t->bind=s.bind;t->type=s.type;t->other=s.other;t->winner=-1;t->resolved=0;
        z=strlen(s.name)+1u;memcpy(o->names+names,s.name,z);names+=z;
    }
    l->total_sections+=o->ns;l->total_symbols+=o->ny;drop(l,p,(size_t)len+1u);return 0;
bad: drop(l,p,(size_t)len+1u);return -1;
}
static int globals(Link *l) {
    unsigned i,j;
    for(i=0;i<l->total_symbols;i++) {
        Sym *a=symbol(l,i,NULL);unsigned best=i;uint32_t common_size=0,common_align=1;int strong=-1;
        if(!a->bind || !symname(l,i)[0])continue;
        for(j=0;j<l->total_symbols;j++) {
            Sym *b=symbol(l,j,NULL);int r;
            if(!b->bind || strcmp(symname(l,i),symname(l,j)))continue;
            r=rank_sym(b);
            if(r==3){if(strong>=0 && (unsigned)strong!=j)return fail(l,"duplicate strong symbol: %s",symname(l,i));strong=(int)j;}
            if(r>rank_sym(symbol(l,best,NULL)))best=j;
            if(b->shndx==XH_ELF_SHN_COMMON) {
                if(b->size>common_size)common_size=b->size;
                if(!b->value || b->value>65536u || (b->value&(b->value-1u)))return fail(l,"invalid COMMON alignment: %s",symname(l,i));
                if(b->value>common_align)common_align=b->value;
            }
        }
        a->winner=(int32_t)best;
        if(symbol(l,best,NULL)->shndx==XH_ELF_SHN_COMMON) {Sym *b=symbol(l,best,NULL);b->size=common_size;b->value=common_align;}
    }
    return 0;
}
static int resolve(Link *l,unsigned idx,uint32_t *value,unsigned *thumb,unsigned *undefweak);
static int script_lookup(void *user,int kind,const char *name,uint32_t *value) {
    Link *l=(Link *)user;Script *s=l->script;unsigned i;
    if(kind==XH_LD_SYMBOL) {
        for(i=0;i<s->ir.symbol_count;i++)if(!strcmp(name,s->ir.symbols[i].name)) {
            if(s->state[i]==2){*value=s->value[i];return 0;}
            if(s->state[i]==1)return -1;
            s->state[i]=1;
            if(xh_ld_script_eval(&s->ir,s->ir.symbols[i].expr,s->dot[i],script_lookup,l,&s->value[i],l->err,l->errcap)){s->state[i]=0;return -1;}
            s->state[i]=2;*value=s->value[i];return 0;
        }
        for(i=0;i<l->total_symbols;i++) {
            Sym *sym=symbol(l,i,NULL);unsigned thumb,weak;
            if(sym->bind && sym->shndx && !strcmp(name,symname(l,i)))return resolve(l,i,value,&thumb,&weak);
        }
        return -1;
    }
    for(i=0;i<s->ir.section_count;i++)if(!strcmp(name,s->ir.sections[i].name) && s->ready[i]) {
        Group *g=&s->groups[i];
        *value=kind==XH_LD_ADDR?g->addr:kind==XH_LD_LOADADDR?g->lma:g->size;return 0;
    }
    return -1;
}
static int resolve(Link *l,unsigned idx,uint32_t *value,unsigned *thumb,unsigned *undefweak) {
    Obj *o;Sym *s=symbol(l,idx,&o);uint32_t offset;
    if(s->winner>=0)s=symbol(l,(unsigned)s->winner,&o);
    *undefweak=0;*thumb=(l->target->thumb && s->type==2u && (s->value&1u))?1u:0u;
    if(!s->shndx) {
        if(s->bind && l->script && !script_lookup(l,XH_LD_SYMBOL,o->names+s->name,value)){*thumb=0;return 0;}
        if(s->bind!=2u)return fail(l,"undefined symbol: %s",o->names+s->name);
        *value=0;*thumb=0;*undefweak=1;return 0;
    }
    if(s->shndx==XH_ELF_SHN_ABS){*value=s->value;return 0;}
    if(s->shndx==XH_ELF_SHN_COMMON){*value=s->resolved;*thumb=0;return 0;}
    if(s->shndx>=o->ns || !(o->sec[s->shndx].flags&2u))return fail(l,"symbol refers to non-alloc section: %s",o->names+s->name);
    if(l->script && !o->sec[s->shndx].out)return fail(l,"symbol refers to unmatched empty section: %s",o->names+s->name);
    offset=*thumb?(s->value&~1u):s->value;
    if(offset>o->sec[s->shndx].size || s->size>o->sec[s->shndx].size-offset || plus(o->sec[s->shndx].addr,offset,value))return fail(l,"symbol outside section: %s",o->names+s->name);
    *value|=*thumb;return 0;
}
static int rv_reloc(Link *l,Obj *o,const XhElfView *v,unsigned relsec,const XhElfSection *rs,const XhElfRela *r,unsigned char *image) {
    Sec *dst=&o->sec[rs->info];XhRvElfRelContext c;unsigned width,thumb,weak;uint32_t k;int branch;
    memset(&c,0,sizeof(c));
    if(xh_rv_elf_rela_width(r->type,&width))return fail(l,"unsupported RV32 relocation %u",(unsigned)r->type);
    if(r->type==51u) {
        unsigned matches=0;
        if(r->symbol || r->addend)return fail(l,"invalid RV32 RELAX marker");
        for(k=0;k<rs->size/12u;k++){XhElfRela a;xh_elf_rela(v,relsec,k,&a);if(a.offset==r->offset && (a.type==18u || a.type==19u || a.type==23u || a.type==24u || a.type==25u || a.type==26u || a.type==27u || a.type==28u))matches++;}
        if(matches!=1)return fail(l,"RV32 RELAX requires exactly one associated relocation");
    }
    if(!dst->out)return 0;
    if(!width)return 0;
    if(dst->type==8u)return fail(l,"RV32 relocation into NOBITS unsupported");
    if(plus(dst->addr,r->offset,&c.place))return fail(l,"RV32 relocation place overflow");
    c.addend=r->addend;
    if(r->type==24u || r->type==25u) {
        XhElfSymbol anchor;XhElfRela high;unsigned matches=0;
        xh_elf_symbol(v,o->tab,r->symbol,&anchor);
        if(r->addend || anchor.shndx!=rs->info || anchor.bind!=0u)return fail(l,"RV32 PCREL_LO requires local same-section HI anchor");
        memset(&high,0,sizeof(high));
        for(k=0;k<rs->size/12u;k++){XhElfRela a;xh_elf_rela(v,relsec,k,&a);if(a.offset==anchor.value && a.type==23u){high=a;matches++;}}
        if(matches!=1)return fail(l,"RV32 PCREL_LO missing or duplicate HI anchor");
        if(resolve(l,o->base+high.symbol,&c.pair_symbol,&thumb,&weak))return -1;
        if(weak)return fail(l,"RV32 PCREL_HI to undefined weak unsupported");
        if(plus(dst->addr,high.offset,&c.pair_place))return fail(l,"RV32 HI place overflow");
        c.pair_addend=high.addend;c.has_pair=1;
    } else {
        if(resolve(l,o->base+r->symbol,&c.symbol,&thumb,&weak))return -1;
        branch=r->type==16u || r->type==17u || r->type==18u || r->type==19u;
        if(branch && weak)return fail(l,"RV32 branch to undefined weak unsupported");
    }
    if(xh_rv_elf_rela_apply(r->type,image+dst->off,dst->size,r->offset,&c))return fail(l,"RV32 relocation range/encoding error (type %u)",(unsigned)r->type);
    return 0;
}
static int script_expr(Link *l,int expr,uint32_t dot,uint32_t *value) {
    return xh_ld_script_eval(&l->script->ir,expr,dot,script_lookup,l,value,l->err,l->errcap);
}
static int script_load(Link *l,const char *path) {
    int len=fs_file_size(path),got;char *text;
    if(len<1 || len>65536)return fail(l,"invalid linker script size");
    l->script=(Script *)take(l,sizeof(Script));if(!l->script)return -1;memset(l->script,0,sizeof(Script));
    text=(char *)take(l,(size_t)len+1u);if(!text)return -1;
    got=fs_read_file(path,text,len+1);
    if(got!=len || fs_file_size(path)!=len){drop(l,text,(size_t)len+1u);return fail(l,"linker script read failed");}
    got=xh_ld_script_parse(text,(size_t)len,&l->script->ir,l->err,l->errcap);drop(l,text,(size_t)len+1u);return got;
}
static int script_layout(Link *l,unsigned n,unsigned *outsec,uint32_t *commonidx) {
    Script *s=l->script;uint32_t cursor[XH_LD_MAX_REGIONS],dot=0;unsigned k,i,j,e=0;
    for(i=0;i<s->ir.symbol_count;i++)for(j=0;j<l->total_symbols;j++)if(rank_sym(symbol(l,j,NULL))==3 && !strcmp(s->ir.symbols[i].name,symname(l,j)))return fail(l,"script symbol conflicts with strong definition: %s",s->ir.symbols[i].name);
    for(k=0;k<s->ir.region_count;k++)cursor[k]=s->ir.regions[k].origin;
    for(k=0;k<s->ir.section_count;k++) {
        XhLdSection *sec=&s->ir.sections[k];Group *g=&s->groups[k];uint32_t start,maxalign=1,loadend;unsigned end=sec->first_event+sec->event_count;
        while(e<sec->first_event) {
            XhLdEvent *ev=&s->ir.events[e++];
            if(ev->kind==XH_LD_DEFINE){s->dot[ev->index]=dot;s->state[ev->index]=0;}
            else if(ev->kind==XH_LD_ASSERT)s->assert_dot[ev->index]=dot;
            else if(ev->kind==XH_LD_SET_DOT){uint32_t next;if(script_expr(l,ev->index,dot,&next))return -1;if(next<dot)return fail(l,"script location counter moves backwards");dot=next;}
        }
        /* Precompute group alignment so both VMA and AT> LMA honor inputs. */
        for(i=0;i<n;i++)for(j=1;j<l->obj[i].ns;j++) {
            Sec *in=&l->obj[i].sec[j];unsigned ev;
            if(!(in->flags&2u) || in->out)continue;
            for(ev=sec->first_event;ev<end;ev++)if(s->ir.events[ev].kind==XH_LD_PATTERN && xh_ld_script_match(&s->ir.patterns[s->ir.events[ev].index],l->obj[i].names+in->name,0)) {if(in->align>maxalign)maxalign=in->align;break;}
        }
        for(i=0;i<l->total_symbols;i++){Sym *sym=symbol(l,i,NULL);unsigned ev;if(sym->shndx!=XH_ELF_SHN_COMMON || (sym->winner>=0 && (unsigned)sym->winner!=i))continue;for(ev=sec->first_event;ev<end;ev++)if(s->ir.events[ev].kind==XH_LD_PATTERN && s->ir.patterns[s->ir.events[ev].index].common){if(sym->value>maxalign)maxalign=sym->value;}}
        /* Explicit >region uses its own cursor; top-level dot still supplies
           the value captured by any intervening symbol assignments. */
        start=cursor[sec->region];
        if(sec->address_expr>=0 && script_expr(l,sec->address_expr,start,&start))return -1;
        if(sec->align_expr>=0 && script_expr(l,sec->align_expr,start,&start))return -1;
        if(aligned(start,maxalign,&start))return fail(l,"script section address overflow");
        g->addr=start;g->align=maxalign;dot=start;
        if(sec->load_region==sec->region)g->lma=start;
        else if(aligned(cursor[sec->load_region],maxalign,&g->lma))return fail(l,"script LMA alignment overflow");
        while(e<end) {
            XhLdEvent *ev=&s->ir.events[e++];
            if(ev->kind==XH_LD_DEFINE){s->dot[ev->index]=dot;s->state[ev->index]=0;continue;}
            if(ev->kind==XH_LD_ASSERT){s->assert_dot[ev->index]=dot;continue;}
            if(ev->kind==XH_LD_SET_DOT){uint32_t next;if(script_expr(l,ev->index,dot,&next))return -1;if(next<dot)return fail(l,"script location counter moves backwards");dot=next;continue;}
            if(s->ir.patterns[ev->index].common) {
                for(i=0;i<l->total_symbols;i++) {
                    Sym *sym=symbol(l,i,NULL);uint32_t next;
                    if(sym->shndx!=XH_ELF_SHN_COMMON || (sym->winner>=0 && (unsigned)sym->winner!=i) || s->common_seen[i])continue;
                    if(!sec->noload)return fail(l,"COMMON requires NOLOAD output section");
                    if(aligned(dot,sym->value,&next) || plus(next,sym->size,&dot))return fail(l,"script COMMON overflow");
                    sym->resolved=next;s->common_seen[i]=1;if(!s->has_common){s->common_start=next;s->common_group=k;s->has_common=1;*commonidx=(*outsec)++;}else if(s->common_group!=k)return fail(l,"COMMON split across output sections unsupported");
                    s->common_end=dot;
                }
                continue;
            }
            for(i=0;i<n;i++)for(j=1;j<l->obj[i].ns;j++) {
                Sec *in=&l->obj[i].sec[j];uint32_t next;
                if(!(in->flags&2u) || in->out || !xh_ld_script_match(&s->ir.patterns[ev->index],l->obj[i].names+in->name,0))continue;
                if(sec->noload && in->type!=8u)return fail(l,"initialized input in NOLOAD section");
                if(aligned(dot,in->align,&next) || plus(next,in->size,&dot))return fail(l,"script section size overflow");
                in->addr=next;in->group=(uint16_t)k;in->out=(uint16_t)(*outsec)++;
                if(plus(g->lma,next-start,&in->lma))return fail(l,"script input LMA overflow");
                g->flags|=(in->flags&1u)?6u:((in->flags&4u)?5u:4u);
                if(in->type!=8u)g->filesz=dot-start;
            }
        }
        g->size=dot-start;if(sec->noload)g->filesz=0;if(!g->flags)g->flags=sec->noload?6u:4u;
        if(start<s->ir.regions[sec->region].origin || (uint64_t)dot>(uint64_t)s->ir.regions[sec->region].origin+s->ir.regions[sec->region].length)return fail(l,"script VMA region overflow");
        if(plus(g->lma,g->filesz,&loadend) || g->lma<s->ir.regions[sec->load_region].origin || (uint64_t)loadend>(uint64_t)s->ir.regions[sec->load_region].origin+s->ir.regions[sec->load_region].length)return fail(l,"script LMA region overflow");
        cursor[sec->region]=dot;
        if(sec->load_region!=sec->region && loadend>cursor[sec->load_region])cursor[sec->load_region]=loadend;
        s->ready[k]=1;
    }
    while(e<s->ir.event_count){XhLdEvent *ev=&s->ir.events[e++];if(ev->kind==XH_LD_DEFINE){s->dot[ev->index]=dot;s->state[ev->index]=0;}else if(ev->kind==XH_LD_ASSERT)s->assert_dot[ev->index]=dot;else if(ev->kind==XH_LD_SET_DOT && script_expr(l,ev->index,dot,&dot))return -1;}
    for(i=0;i<n;i++)for(j=1;j<l->obj[i].ns;j++)if((l->obj[i].sec[j].flags&2u) && l->obj[i].sec[j].size && !l->obj[i].sec[j].out)return fail(l,"unmatched alloc input section: %s",l->obj[i].names+l->obj[i].sec[j].name);
    for(i=0;i<l->total_symbols;i++){Sym *sym=symbol(l,i,NULL);if(sym->shndx==XH_ELF_SHN_COMMON && (sym->winner<0 || (unsigned)sym->winner==i) && !s->common_seen[i])return fail(l,"unmatched COMMON input");}
    for(i=0;i<s->ir.section_count;i++)for(j=0;j<i;j++) {
        Group *a=&s->groups[i],*b=&s->groups[j];
        if(a->size && b->size && (uint64_t)a->addr<(uint64_t)b->addr+b->size && (uint64_t)b->addr<(uint64_t)a->addr+a->size)return fail(l,"overlapping script VMA sections");
        if(a->filesz && b->filesz && (uint64_t)a->lma<(uint64_t)b->lma+b->filesz && (uint64_t)b->lma<(uint64_t)a->lma+a->filesz)return fail(l,"overlapping script LMA sections");
    }
    for(i=0;i<s->ir.symbol_count;i++){uint32_t v;s->state[i]=0;if(script_lookup(l,XH_LD_SYMBOL,s->ir.symbols[i].name,&v))return fail(l,"unresolved script symbol: %s",s->ir.symbols[i].name);}
    for(i=0;i<s->ir.assertion_count;i++) {
        uint32_t v;
        if(script_expr(l,s->ir.assertions[i].expr,s->assert_dot[i],&v))return fail(l,"linker ASSERT could not be evaluated: %s",s->ir.assertions[i].message);
        if(!v)return fail(l,"linker ASSERT failed: %s",s->ir.assertions[i].message);
    }
    return 0;
}
static void w16(unsigned char *p,unsigned v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void w32(unsigned char *p,uint32_t v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);p[2]=(unsigned char)(v>>16);p[3]=(unsigned char)(v>>24);}
static void shdr(unsigned char *p,uint32_t name,uint32_t type,uint32_t flags,uint32_t addr,uint32_t off,uint32_t size,uint32_t link,uint32_t info,uint32_t align,uint32_t entsize) {
    w32(p,name);w32(p+4,type);w32(p+8,flags);w32(p+12,addr);w32(p+16,off);w32(p+20,size);w32(p+24,link);w32(p+28,info);w32(p+32,align);w32(p+36,entsize);
}
static void phdr(unsigned char *p,uint32_t off,uint32_t vma,uint32_t lma,uint32_t filesz,uint32_t memsz,uint32_t flags) {
    w32(p,1);w32(p+4,off);w32(p+8,vma);w32(p+12,lma);w32(p+16,filesz);w32(p+20,memsz);w32(p+24,flags);w32(p+28,1);
}
int xh_elf_link_report(XhElfTarget target,const char *const *inputs,unsigned n,const XhElfLinkOptions *opt,char *err,size_t errcap,XhElfLinkReport *report) {
    Link l;unsigned i,j,pass,outsec=1,locals=1,nsym=1;int result=-1;
    uint32_t text,data,lma,flashend,dataend,bssend,ramalign=1,flashsize,datasize,memsize;
    uint32_t dataoff,stroff,strsize=1,symoff,shoff,total,entry=0,common_addr;
    uint32_t symtabidx,strtabidx,commonidx=0,flatlen=0,flatbase=0,headerbytes=116;unsigned char *image=NULL;size_t imagebytes=0;
    const char *entryname;
    memset(&l,0,sizeof(l));l.err=err;l.errcap=errcap;l.limit=XH_ELF_LINK_MAX_MEMORY;
    if(report)memset(report,0,sizeof(*report));
    l.target=target==XH_ELF_TARGET_ARM?&arm_target:target==XH_ELF_TARGET_RISCV32?&rv_target:NULL;
    if(!l.target)return fail(&l,"unsupported ELF link target");
    if(err && errcap)err[0]=0;
    if(!inputs || !n || n>XH_ELF_LINK_MAX_OBJECTS || !opt || !opt->output_path || !opt->output_path[0] || !strcmp(opt->output_path,"-"))return fail(&l,"invalid ELF link arguments/output");
    if(opt->output_format>1u)return fail(&l,"unsupported output format");
    if(opt->max_memory){if(opt->max_memory>XH_ELF_LINK_MAX_MEMORY)return fail(&l,"ELF memory budget exceeds configured linker maximum");l.limit=opt->max_memory;}
    text=opt->text_addr?opt->text_addr:0x08000000u;data=opt->data_addr?opt->data_addr:0x20000000u;
    entryname=opt->entry_name?opt->entry_name:"_start";
    for(i=0;i<n;i++){if(!inputs[i] || scan(&l,inputs[i],&l.obj[i]))goto done;l.objects++;}
    if(globals(&l))goto done;
    if(opt->script_path) {
        if(script_load(&l,opt->script_path) || script_layout(&l,n,&outsec,&commonidx))goto done;
        if(!opt->entry_name && l.script->ir.entry[0])entryname=l.script->ir.entry;
        headerbytes=52u+l.script->ir.section_count*32u;
        common_addr=l.script->common_start;bssend=l.script->common_end;
        flashsize=datasize=memsize=0;lma=0;
        goto layout_ready;
    }
    flashend=text;dataend=data;
    for(pass=0;pass<3;pass++)for(i=0;i<n;i++)for(j=1;j<l.obj[i].ns;j++) {
        Sec *s=&l.obj[i].sec[j];unsigned kind;
        if(!(s->flags&2u))continue;
        kind=!(s->flags&1u)?0u:(s->type==8u?2u:1u);
        if(kind!=pass)continue;
        if(pass==0) {if(aligned(flashend,s->align,&s->addr) || plus(s->addr,s->size,&flashend))goto address_error;}
        else {if(aligned(dataend,s->align,&s->addr) || plus(s->addr,s->size,&dataend))goto address_error;if(s->align>ramalign)ramalign=s->align;}
        s->out=(uint16_t)outsec++;
    }
    /* RAM initialized extent excludes trailing NOBITS, but includes its leading padding only in memsz. */
    bssend=dataend;dataend=data;
    for(i=0;i<n;i++)for(j=1;j<l.obj[i].ns;j++){Sec *s=&l.obj[i].sec[j];if((s->flags&3u)==3u && s->type!=8u){uint32_t end;if(plus(s->addr,s->size,&end))goto address_error;if(end>dataend)dataend=end;}}
    common_addr=bssend;
    for(i=0;i<l.total_symbols;i++) {
        Sym *s=symbol(&l,i,NULL);
        if(s->shndx==XH_ELF_SHN_COMMON && (s->winner<0 || (unsigned)s->winner==i)) {
            if(aligned(bssend,s->value,&s->resolved) || plus(s->resolved,s->size,&bssend))goto address_error;
            if(s->value>ramalign)ramalign=s->value;
            if(!commonidx)commonidx=outsec++;
        }
    }
    flashsize=flashend-text;datasize=dataend-data;memsize=bssend-data;
    if(opt->data_lma)lma=opt->data_lma;else if(aligned(flashend,ramalign,&lma))goto address_error;
    {uint32_t lend;if(plus(lma,datasize,&lend))goto address_error;
     if((memsize && flashsize && text<bssend && data<flashend) || (datasize && flashsize && text<lend && lma<flashend) || (memsize && datasize && data<lend && lma<bssend)) {fail(&l,"overlapping Flash/RAM address intervals");goto done;}}
layout_ready:
    /* All input section bases must preserve their declared alignment. */
    for(i=0;i<n;i++)for(j=1;j<l.obj[i].ns;j++){Sec *s=&l.obj[i].sec[j];if((s->flags&2u) && (s->addr&(s->align-1u))){fail(&l,"section alignment error");goto done;}}
    for(i=0;i<l.total_symbols;i++) {
        Sym *s=symbol(&l,i,NULL);
        if(s->bind && !strcmp(symname(&l,i),entryname)) {unsigned t,u;Sym *def=s->winner<0?s:symbol(&l,(unsigned)s->winner,NULL);if(resolve(&l,i,&entry,&t,&u))goto done;if(def->type!=XH_ELF_STT_FUNC || !def->shndx || u || (l.target->thumb?!t:(entry&3u)!=0u)){fail(&l,"entry must be a defined aligned target function");goto done;}break;}
    }
    if(i==l.total_symbols){fail(&l,"entry symbol not found: %s",entryname);goto done;}
    /* One output symbol per input symbol except the repeated null sentinel.
       Unused debug symbols are omitted; locals remain before globals. */
    for(i=0;i<n;i++) {
        Obj *o=&l.obj[i];
        for(j=1;j<o->ns;j++)if(o->sec[j].out){if(plus(strsize,(uint32_t)strlen(o->names+o->sec[j].name)+1u,&strsize))goto address_error;}
        for(j=1;j<o->ny;j++){Sym *s=&o->sym[j];if(s->shndx && s->shndx<o->ns && !o->sec[s->shndx].out)continue;nsym++;if(!s->bind)locals++;if(plus(strsize,(uint32_t)strlen(o->names+s->name)+1u,&strsize))goto address_error;}
    }
    if(l.script)for(i=0;i<l.script->ir.symbol_count;i++){if(plus(strsize,(uint32_t)strlen(l.script->ir.symbols[i].name)+1u,&strsize))goto address_error;nsym++;}
    /* Fixed names .common .symtab .strtab share the section/symbol string table. */
    if(plus(strsize,25u,&strsize))goto address_error;
    symtabidx=outsec++;strtabidx=outsec++;
    if(l.script) {
        uint32_t end=headerbytes,min=UINT32_MAX,max=0;
        for(i=0;i<l.script->ir.section_count;i++) {
            Group *g=&l.script->groups[i];uint32_t lend;
            if(aligned(end,4,&g->off) || plus(g->off,g->filesz,&end) || plus(g->lma,g->filesz,&lend))goto address_error;
            if(g->filesz){if(g->lma<min)min=g->lma;if(lend>max)max=lend;}
        }
        stroff=end;dataoff=headerbytes;flatbase=min==UINT32_MAX?0:min;flatlen=max-flatbase;
        if(plus(stroff,strsize,&symoff) || aligned(symoff,4u,&symoff) || plus(symoff,nsym*16u,&shoff) || plus(shoff,outsec*40u,&total))goto address_error;
    } else if(aligned(116u,4u,&dataoff) || plus(dataoff,flashsize,&dataoff) || aligned(dataoff,4u,&dataoff) || plus(dataoff,datasize,&stroff) || plus(stroff,strsize,&symoff) || aligned(symoff,4u,&symoff) || plus(symoff,nsym*16u,&shoff) || plus(shoff,outsec*40u,&total))goto address_error;
    if(total>XH_ELF_LINK_MAX_OUTPUT){fail(&l,"ELF output limit exceeded");goto done;}
    imagebytes=total;
    if(opt->output_format==1u) {
        if(!l.script)flatlen=flashsize;
        if(!l.script && datasize) {
            if(lma<text || plus(lma-text,datasize,&flatlen))goto address_error;
            if(flatlen<flashsize)flatlen=flashsize;
        }
        if(flatlen>XH_ELF_LINK_MAX_OUTPUT){fail(&l,"binary Flash LMA span exceeds output limit");goto done;}
        if(flatlen>imagebytes)imagebytes=flatlen;
    }
    image=(unsigned char *)take(&l,imagebytes);if(!image)goto done;memset(image,0,imagebytes);
    for(i=0;i<n;i++)for(j=1;j<l.obj[i].ns;j++){Sec *s=&l.obj[i].sec[j];if(s->out)s->off=l.script?(opt->output_format==1u?(s->type==8u?0:s->lma-flatbase):l.script->groups[s->group].off+s->addr-l.script->groups[s->group].addr):((s->flags&1u)?dataoff+(s->addr-data):116u+(s->addr-text));}
    /* Pass two: no retained reader pointer survives the corresponding free. */
    for(i=0;i<n;i++) {
        Obj *o=&l.obj[i];XhElfView v;unsigned char *p;int len;
        p=load(&l,inputs[i],&len,&v);if(!p)goto done;
        if(len!=o->filelen || fingerprint(p,(size_t)len)!=o->fingerprint){fail(&l,"ELF input changed between passes");drop(&l,p,(size_t)len+1u);goto done;}
        for(j=1;j<o->ns;j++){Sec *s=&o->sec[j];XhElfSection in;if(!s->out || s->type==8u)continue;xh_elf_section(&v,j,&in);memcpy(image+s->off,in.data,s->size);}
        for(j=1;j<o->ns;j++) {
            XhElfSection r;uint32_t k;xh_elf_section(&v,j,&r);
            if(r.type==XH_ELF_SHT_RELA) {
                for(k=0;k<r.size/12u;k++){XhElfRela a;xh_elf_rela(&v,j,k,&a);if(rv_reloc(&l,o,&v,j,&r,&a,image))goto object_bad;}
                continue;
            }
            if(r.type!=9u)continue;
            for(k=0;k<r.size/8u;k++) {
                XhElfRel rel;Sec *target=&o->sec[r.info];uint32_t value,place;unsigned thumb,weak;int64_t a;unsigned width;
                xh_elf_rel(&v,j,k,&rel);
                /* Unknown relocation types are rejected even in ignored debug sections. */
                if(rel.type!=2u && rel.type!=10u && rel.type!=30u && rel.type!=51u && rel.type!=47u && rel.type!=48u){fail(&l,"unsupported ARM relocation %u",(unsigned)rel.type);goto object_bad;}
                if(!target->out)continue;
                if(target->type==8u){fail(&l,"relocation into NOBITS unsupported");goto object_bad;}
                if(resolve(&l,o->base+rel.symbol,&value,&thumb,&weak))goto object_bad;
                if((rel.type==10u || rel.type==30u || rel.type==51u) && (weak || !thumb)){fail(&l,"branch requires defined Thumb FUNC symbol");goto object_bad;}
                if(plus(target->addr,rel.offset,&place) || xh_arm_elf_rel_decode(rel.type,image+target->off,target->size,rel.offset,&a,&width) || xh_arm_elf_rel_apply(rel.type,image+target->off,target->size,rel.offset,value,place,thumb)){fail(&l,"ARM relocation range/encoding error (type %u)",(unsigned)rel.type);goto object_bad;}
            }
        }
        drop(&l,p,(size_t)len+1u);continue;
object_bad:drop(&l,p,(size_t)len+1u);goto done;
    }
    if(l.script && opt->output_format==1u){total=flatlen;goto write_output;}
    /* ELF header and load segments. Segment alignment one is valid ELF and
       avoids padding the file by the MCU's address-space alignment. */
    memcpy(image,"\177ELF\1\1\1",7);w16(image+16,2);w16(image+18,l.target->machine);w32(image+20,1);w32(image+24,entry);w32(image+28,52);w32(image+32,shoff);w32(image+36,l.target->flags);w16(image+40,52);w16(image+42,32);w16(image+44,l.script?l.script->ir.section_count:2u);w16(image+46,40);w16(image+48,outsec);w16(image+50,strtabidx);
    if(l.script)for(i=0;i<l.script->ir.section_count;i++){Group *g=&l.script->groups[i];phdr(image+52u+i*32u,g->off,g->addr,g->lma,g->filesz,g->size,g->flags);}
    else {phdr(image+52,116,text,text,flashsize,flashsize,5);phdr(image+84,dataoff,data,lma,datasize,memsize,6);}
    {
        uint32_t cursor=1,symcursor=1,commonname,symnameoff,strname;
        for(i=0;i<n;i++)for(j=1;j<l.obj[i].ns;j++) {
            Obj *o=&l.obj[i];Sec *s=&o->sec[j];size_t z;if(!s->out)continue;
            z=strlen(o->names+s->name)+1u;memcpy(image+stroff+cursor,o->names+s->name,z);
            shdr(image+shoff+s->out*40u,cursor,s->type,s->flags,s->addr,s->off,s->size,0,0,s->align,(s->type==14u || s->type==15u)?4u:0u);cursor+=(uint32_t)z;
        }
        commonname=cursor;memcpy(image+stroff+cursor,".common",8);cursor+=8;
        symnameoff=cursor;memcpy(image+stroff+cursor,".symtab",8);cursor+=8;
        strname=cursor;memcpy(image+stroff+cursor,".strtab",8);cursor+=8;
        if(commonidx)shdr(image+shoff+commonidx*40u,commonname,8,3,common_addr,l.script?l.script->groups[l.script->common_group].off:dataoff+datasize,bssend-common_addr,0,0,1,0);
        shdr(image+shoff+symtabidx*40u,symnameoff,2,0,0,symoff,nsym*16u,strtabidx,locals,4,16);
        shdr(image+shoff+strtabidx*40u,strname,3,0,0,stroff,strsize,0,0,1,0);
        for(pass=0;pass<2;pass++)for(i=0;i<n;i++)for(j=1;j<l.obj[i].ny;j++) {
            Obj *o=&l.obj[i];Sym *s=&o->sym[j],*def=s;Obj *owner=o;uint32_t value=0;unsigned thumb,weak;uint16_t sh=0;unsigned char *p;size_t z;
            if((s->bind!=0)!=(pass!=0))continue;
            if(s->shndx && s->shndx<o->ns && !o->sec[s->shndx].out)continue;
            if(s->winner>=0)def=symbol(&l,(unsigned)s->winner,&owner);
            if(!def->shndx && def->bind && l.script && !script_lookup(&l,XH_LD_SYMBOL,o->names+s->name,&value))sh=XH_ELF_SHN_ABS;
            if(def->shndx) {
                if(def->shndx<owner->ns && !owner->sec[def->shndx].out)continue;
                if(resolve(&l,o->base+j,&value,&thumb,&weak))goto done;
                if(def->shndx==XH_ELF_SHN_ABS)sh=XH_ELF_SHN_ABS;
                else if(def->shndx==XH_ELF_SHN_COMMON)sh=(uint16_t)commonidx;
                else sh=owner->sec[def->shndx].out;
            }
            p=image+symoff+symcursor++*16u;z=strlen(o->names+s->name)+1u;memcpy(image+stroff+cursor,o->names+s->name,z);w32(p,cursor);cursor+=(uint32_t)z;
            w32(p+4,value);w32(p+8,def->size);p[12]=(unsigned char)((s->bind<<4)|def->type);p[13]=s->other;w16(p+14,sh);
        }
        if(l.script)for(i=0;i<l.script->ir.symbol_count;i++) {
            unsigned char *p=image+symoff+symcursor++*16u;size_t z=strlen(l.script->ir.symbols[i].name)+1u;
            memcpy(image+stroff+cursor,l.script->ir.symbols[i].name,z);w32(p,cursor);cursor+=(uint32_t)z;w32(p+4,l.script->value[i]);p[12]=16;w16(p+14,XH_ELF_SHN_ABS);
        }
    }
    if(opt->output_format==1u) {
        /* Flash compaction cannot touch the initialized RAM source. Its final
           interval is disjoint from RAM's Flash LMA interval. */
        memmove(image,image+116,flashsize);
        if(datasize)memmove(image+(lma-text),image+dataoff,datasize);
        if(datasize && lma-text>flashsize)memset(image+flashsize,0,lma-text-flashsize);
        total=flatlen;
    }
write_output:
    if(fs_write_file(opt->output_path,image,(int)total)!=(int)total){fail(&l,"ELF output write failed: %s",fs_last_error());goto done;}
    result=0;goto done;
address_error:fail(&l,"ELF address/size/alignment overflow");
done:
    drop(&l,image,imagebytes);
    drop(&l,l.script,sizeof(Script));
    for(i=0;i<n;i++){Obj *o=&l.obj[i];drop(&l,o->sec,o->secbytes);drop(&l,o->sym,o->symbytes);drop(&l,o->names,o->namebytes);}
    if(report){report->peak_requested_bytes=l.peak;report->objects=l.objects;report->sections=l.total_sections;report->symbols=l.total_symbols;report->output_bytes=result?0:total;}
    return result;
}
int xh_elf_link(XhElfTarget target,const char *const *inputs,unsigned n,const XhElfLinkOptions *opt,char *err,size_t errcap) {
    return xh_elf_link_report(target,inputs,n,opt,err,errcap,NULL);
}
int xh_elf_link_arm_report(const char *const *inputs,unsigned n,const XhElfLinkOptions *opt,char *err,size_t errcap,XhElfLinkReport *report) {
    return xh_elf_link_report(XH_ELF_TARGET_ARM,inputs,n,opt,err,errcap,report);
}
int xh_elf_link_arm(const char *const *inputs,unsigned n,const XhElfLinkOptions *opt,char *err,size_t errcap) {
    return xh_elf_link(XH_ELF_TARGET_ARM,inputs,n,opt,err,errcap);
}
