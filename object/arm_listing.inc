/* Complete GNU assembly data definitions for -S. Included before codegen_arm. */
static void xh_listing_ref(AsmBuffer *b,const char *name,long long add){
    asmbuf_puts(b,"    .word ");asmbuf_puts(b,name);
    if(add>0)asmbuf_putc(b,'+');if(add)asmbuf_int(b,add);asmbuf_putc(b,'\n');
}
static void xh_listing_data(AsmBuffer *b,IrProgram *p){
    int i,j;
    for(i=0;i<p->global_count;i++){
        IrGlobal *g=p->globals[i];int sz=g->type?type_size(g->type):4;unsigned char *bytes;
        if(g->is_extern)continue;if(sz<=0)sz=4;
        if(g->is_tls||g->section){diag_tool(DIAG_SEV_ERROR,"xhcc","assembly TLS and custom data sections are not supported",0,0,0,0,0,0);continue;}
        if(!g->is_static){asmbuf_puts(b,".global ");asmbuf_puts(b,g->name);asmbuf_putc(b,'\n');}
        if(g->has_at){asmbuf_puts(b,".set ");asmbuf_puts(b,g->name);asmbuf_puts(b,", ");asmbuf_int(b,g->at_addr);asmbuf_putc(b,'\n');continue;}
        asmbuf_puts(b,g->has_init?".data\n":".bss\n");asmbuf_puts(b,".balign ");asmbuf_int(b,g->align_bytes>8?g->align_bytes:8);asmbuf_putc(b,'\n');
        asmbuf_puts(b,".type ");asmbuf_puts(b,g->name);asmbuf_puts(b,", %object\n.size ");asmbuf_puts(b,g->name);asmbuf_puts(b,", ");asmbuf_int(b,sz);asmbuf_putc(b,'\n');asmbuf_puts(b,g->name);asmbuf_puts(b,":\n");
        if(!g->has_init){asmbuf_puts(b,"    .zero ");asmbuf_int(b,sz);asmbuf_putc(b,'\n');continue;}
        if(g->init_sym){xh_listing_ref(b,g->init_sym,g->init_sym_off);continue;}
        if(g->init_str&&g->type&&g->type->kind==TYPE_PTR){char nm[64];snprintf(nm,sizeof(nm),".L_gstr_%d",i);xh_listing_ref(b,nm,0);continue;}
        bytes=mem_pool_alloc(sz);if(!bytes){diag_tool(DIAG_SEV_ERROR,"xhcc","assembly data exceeds memory",0,0,0,0,0,0);continue;}for(j=0;j<sz;j++)bytes[j]=0;
        if(g->init_aggregate){if(ark_agg_fill_ex(g->type,g->init_aggregate,bytes,sz,arm_agg_addr_hook,g)){diag_tool(DIAG_SEV_ERROR,"xhcc","unsupported assembly initializer",0,0,0,0,0,0);continue;}}
        else if(g->init_str){int len=ark_strlen(g->init_str);for(j=0;j<sz&&j<len;j++)bytes[j]=(unsigned char)g->init_str[j];}
        else if(g->is_float){Type *bt=g->type;int cx=bt&&ARK_IS_CX(bt);if(cx)bt=bt->base;
            if(bt&&bt->kind==TYPE_DOUBLE){union{double d;unsigned char v[8];}u;u.d=g->init_float;for(j=0;j<sz&&j<8;j++)bytes[j]=u.v[j];if(cx){u.d=g->init_float_im;for(j=0;j<8&&j+8<sz;j++)bytes[j+8]=u.v[j];}}
            else{union{float f;unsigned char v[4];}u;u.f=(float)g->init_float;for(j=0;j<sz&&j<4;j++)bytes[j]=u.v[j];if(cx){u.f=(float)g->init_float_im;for(j=0;j<4&&j+4<sz;j++)bytes[j+4]=u.v[j];}}
        }else{unsigned long long v=(unsigned long long)g->init_int;for(j=0;j<sz&&j<8;j++)bytes[j]=(unsigned char)(v>>(8*j));}
        for(j=0;j<sz;){int k,ref=-1;for(k=0;k<g->init_ref_count;k++)if(g->init_refs[k].offset==j){ref=k;break;}
            if(ref>=0){IrSymRef *q=&g->init_refs[ref];char nm[80];const char *name=q->sym;if(q->str){snprintf(nm,sizeof(nm),".L_gstr_%d_%d",i,j);name=nm;}xh_listing_ref(b,name,q->add);j+=4;}
            else{asmbuf_puts(b,"    .byte ");asmbuf_int(b,bytes[j++]);asmbuf_putc(b,'\n');}
        }
    }
    asmbuf_puts(b,".text\n");
}
