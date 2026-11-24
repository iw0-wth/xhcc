/* Independent ELF wire reader. All reads are byte-wise (unaligned-safe).
 * No allocator, libc dependency, global scratch array, or input ownership. */
#include "xh_elf_read.h"

static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static int span(size_t len, uint32_t off, uint32_t size) {
    return size <= UINT32_MAX-off && (size_t)off <= len &&
           (size_t)size <= len-(size_t)off;
}
static void section_raw(const XhElfView *v, unsigned i, XhElfSection *s) {
    const unsigned char *p=v->data+(size_t)v->shoff+(size_t)i*40u;
    s->name_index=rd32(p); s->type=rd32(p+4); s->flags=rd32(p+8);
    s->addr=rd32(p+12); s->offset=rd32(p+16); s->size=rd32(p+20);
    s->link=rd32(p+24); s->info=rd32(p+28); s->align=rd32(p+32);
    s->entsize=rd32(p+36); s->name=0;
    s->data=(s->type==XH_ELF_SHT_NOBITS || s->type==XH_ELF_SHT_NULL)?0:v->data+s->offset;
}
static int string_at(const XhElfView *v, const XhElfSection *s,
                     uint32_t index, const char **out) {
    uint32_t i;
    if(index>=s->size) return XH_ELF_ERR_INDEX;
    for(i=index;i<s->size;i++) {
        if(!v->data[(size_t)s->offset+i]) {
            *out=(const char *)(v->data+(size_t)s->offset+index);
            return 0;
        }
    }
    return XH_ELF_ERR_FORMAT;
}
int xh_elf_section(const XhElfView *v, unsigned i, XhElfSection *s) {
    XhElfSection names;
    if(!v || !v->data || !s) return XH_ELF_ERR_ARGUMENT;
    if(i>=v->shnum) return XH_ELF_ERR_INDEX;
    section_raw(v,i,s);
    if(v->shstrndx) {
        section_raw(v,v->shstrndx,&names);
        return string_at(v,&names,s->name_index,&s->name);
    }
    if(s->name_index) return XH_ELF_ERR_INDEX;
    s->name="";
    return 0;
}
int xh_elf_symbol(const XhElfView *v, unsigned tab, uint32_t i, XhElfSymbol *s) {
    XhElfSection t, names;
    const unsigned char *p;
    if(!v || !v->data || !s) return XH_ELF_ERR_ARGUMENT;
    if(tab>=v->shnum) return XH_ELF_ERR_INDEX;
    section_raw(v,tab,&t);
    if(t.type!=XH_ELF_SHT_SYMTAB || t.entsize!=16) return XH_ELF_ERR_FORMAT;
    if(i>=t.size/16u) return XH_ELF_ERR_INDEX;
    p=v->data+(size_t)t.offset+(size_t)i*16u;
    s->name_index=rd32(p); s->value=rd32(p+4); s->size=rd32(p+8);
    s->bind=(uint8_t)(p[12]>>4); s->type=(uint8_t)(p[12]&15);
    s->other=p[13]; s->shndx=rd16(p+14);
    section_raw(v,t.link,&names);
    return string_at(v,&names,s->name_index,&s->name);
}
int xh_elf_rel(const XhElfView *v, unsigned tab, uint32_t i, XhElfRel *r) {
    XhElfSection t;
    const unsigned char *p;
    uint32_t info;
    if(!v || !v->data || !r) return XH_ELF_ERR_ARGUMENT;
    if(tab>=v->shnum) return XH_ELF_ERR_INDEX;
    section_raw(v,tab,&t);
    if(t.type!=XH_ELF_SHT_REL || t.entsize!=8) return XH_ELF_ERR_FORMAT;
    if(i>=t.size/8u) return XH_ELF_ERR_INDEX;
    p=v->data+(size_t)t.offset+(size_t)i*8u;
    r->offset=rd32(p); info=rd32(p+4);
    r->symbol=info>>8; r->type=(uint8_t)info;
    return 0;
}
int xh_elf_open(const unsigned char *data, size_t len, XhElfView *out) {
    XhElfView v;
    XhElfSection s, t, names;
    unsigned i;
    uint32_t j;
    const char *name;
    if(!out) return XH_ELF_ERR_ARGUMENT;
    out->data=0; out->len=0; out->shoff=0; out->flags=0;
    out->shnum=0; out->shstrndx=0;
    if(!data) return XH_ELF_ERR_ARGUMENT;
    if(len<52) return XH_ELF_ERR_BOUNDS;
    if(data[0]!=127 || data[1]!='E' || data[2]!='L' || data[3]!='F' ||
       data[4]!=1 || data[5]!=1 || data[6]!=1 || rd16(data+16)!=1 ||
       rd16(data+18)!=40 || rd32(data+20)!=1 || rd16(data+40)!=52 ||
       rd16(data+46)!=40) return XH_ELF_ERR_FORMAT;
    /* Relocatable input has no loadable program headers or entry address. */
    if(rd32(data+24) || rd32(data+28) || rd16(data+42) || rd16(data+44))
        return XH_ELF_ERR_UNSUPPORTED;
    v.data=data; v.len=len; v.shoff=rd32(data+32); v.flags=rd32(data+36);
    v.shnum=rd16(data+48); v.shstrndx=rd16(data+50);
    if((v.flags & ~0x200u)!=0x05000000u) return XH_ELF_ERR_UNSUPPORTED;
    if(!v.shnum || v.shnum>XH_ELF_MAX_SECTIONS ||
       v.shstrndx==0xffffu) return XH_ELF_ERR_UNSUPPORTED;
    if(v.shstrndx>=v.shnum) return XH_ELF_ERR_INDEX;
    if(v.shoff<52 || !span(len,v.shoff,(uint32_t)v.shnum*40u))
        return XH_ELF_ERR_BOUNDS;
    /* Section zero must be the all-zero sentinel, not an extended-index carrier. */
    for(i=0;i<40;i++) if(data[(size_t)v.shoff+i]) return XH_ELF_ERR_FORMAT;
    /* Validate every file span before following any links or string pointers. */
    for(i=1;i<v.shnum;i++) {
        /* Decode manually first: section_raw forms a data pointer only after range validation. */
        const unsigned char *p=data+(size_t)v.shoff+(size_t)i*40u;
        uint32_t type=rd32(p+4), off=rd32(p+16), size=rd32(p+20);
        if(size>UINT32_MAX-off) return XH_ELF_ERR_BOUNDS;
        if(type!=XH_ELF_SHT_NOBITS && !span(len,off,size)) return XH_ELF_ERR_BOUNDS;
        section_raw(&v,i,&s);
        if(s.size>UINT32_MAX-s.addr) return XH_ELF_ERR_BOUNDS;
        if(s.type==XH_ELF_SHT_NULL) return XH_ELF_ERR_FORMAT;
        if(s.align>XH_ELF_MAX_ALIGN || (s.align && (s.align&(s.align-1))))
            return XH_ELF_ERR_FORMAT;
        if(s.type==17 || (s.flags&(0x200u|0x400u)) || s.type==18 ||
           s.type==4 || s.type==11) return XH_ELF_ERR_UNSUPPORTED;
        if(s.type==XH_ELF_SHT_STRTAB && (!s.size || data[s.offset] ||
           data[(size_t)s.offset+s.size-1])) return XH_ELF_ERR_FORMAT;
    }
    if(v.shstrndx) {
        section_raw(&v,v.shstrndx,&names);
        if(names.type!=XH_ELF_SHT_STRTAB) return XH_ELF_ERR_FORMAT;
    }
    for(i=0;i<v.shnum;i++) {
        section_raw(&v,i,&s);
        if(v.shstrndx) {
            int rc=string_at(&v,&names,s.name_index,&name);
            if(rc) return rc;
        } else if(s.name_index) return XH_ELF_ERR_INDEX;
        if(s.type==XH_ELF_SHT_SYMTAB) {
            if(s.entsize!=16 || s.size%16 || !s.size) return XH_ELF_ERR_FORMAT;
            if(s.size/16u>XH_ELF_MAX_RECORDS) return XH_ELF_ERR_UNSUPPORTED;
            if(!s.link || s.link>=v.shnum) return XH_ELF_ERR_INDEX;
            section_raw(&v,s.link,&t);
            if(t.type!=XH_ELF_SHT_STRTAB || !s.info || s.info>s.size/16u)
                return XH_ELF_ERR_FORMAT;
            for(j=0;j<s.size/16u;j++) {
                XhElfSymbol sym;
                int rc=xh_elf_symbol(&v,i,j,&sym);
                if(rc) return rc;
                if((j<s.info)!=(sym.bind==XH_ELF_STB_LOCAL)) return XH_ELF_ERR_FORMAT;
                if(sym.type==6) return XH_ELF_ERR_UNSUPPORTED; /* STT_TLS */
                if(sym.shndx>=v.shnum && sym.shndx!=XH_ELF_SHN_ABS &&
                   sym.shndx!=XH_ELF_SHN_COMMON) return XH_ELF_ERR_INDEX;
                if(!j && (sym.name_index || sym.value || sym.size || sym.bind ||
                   sym.type || sym.other || sym.shndx)) return XH_ELF_ERR_FORMAT;
            }
        }
    }
    for(i=1;i<v.shnum;i++) {
        section_raw(&v,i,&s);
        if(s.type!=XH_ELF_SHT_REL) continue;
        if(s.entsize!=8 || s.size%8) return XH_ELF_ERR_FORMAT;
        if(s.size/8u>XH_ELF_MAX_RECORDS) return XH_ELF_ERR_UNSUPPORTED;
        if(!s.link || s.link>=v.shnum || !s.info || s.info>=v.shnum)
            return XH_ELF_ERR_INDEX;
        section_raw(&v,s.link,&t);
        if(t.type!=XH_ELF_SHT_SYMTAB) return XH_ELF_ERR_FORMAT;
        for(j=0;j<s.size/8u;j++) {
            XhElfRel r;
            XhElfSection target;
            int rc=xh_elf_rel(&v,i,j,&r);
            if(rc) return rc;
            if(r.symbol>=t.size/16u) return XH_ELF_ERR_INDEX;
            section_raw(&v,s.info,&target);
            if(r.offset>=target.size) return XH_ELF_ERR_BOUNDS;
        }
    }
    *out=v;
    return 0;
}
int xh_elf_validate_thumb(const XhElfView *v) {
    unsigned i;
    uint32_t j;
    if(!v || !v->data) return XH_ELF_ERR_ARGUMENT;
    for(i=1;i<v->shnum;i++) {
        XhElfSection t;
        section_raw(v,i,&t);
        if(t.type!=XH_ELF_SHT_SYMTAB) continue;
        for(j=0;j<t.size/16u;j++) {
            XhElfSymbol s;
            int rc=xh_elf_symbol(v,i,j,&s);
            if(rc) return rc;
            if(s.type==XH_ELF_STT_FUNC && s.shndx && s.shndx<v->shnum &&
               !(s.value&1u)) return XH_ELF_ERR_THUMB;
        }
    }
    return 0;
}
