/* RV32 ELF uses the assembler's real section/label tables. Initializers are
 * copied from its bytes; IR supplies only object extent/storage metadata. */
typedef struct { unsigned name,type,flags,off,size,link,info,align,entsize; } TrSec;
static unsigned trname(char *names,unsigned *used,const char *name){unsigned start=*used;do{names[(*used)++]=*name;}while(*name++);return start;}
static int trwrite(const char*path,unsigned char*code,int len,TeSym*s,int n,TeRel*r,int nr,const XhObjectTarget*d){
 RvAsm*a=&xh_rv_asm;IrProgram*p=xh_rv_prog;int i,j,k,custom=0,base,nsec,locals=1;unsigned ns=1,ncap=512,strsz=1,total,shoff;const char**cn;TrSec*sec;int*ms,*rs;unsigned*mo,*cursor;unsigned char*out;char*names;
 if(!p||len<0)goto fail;
 cn=mem_pool_alloc((p->global_count+1)*(int)sizeof(*cn));if(!cn)goto fail;
 for(i=0;i<p->global_count;i++)if(p->globals[i]->section){const char*nm=p->globals[i]->section;if(ark_streq(nm,".text")||ark_streq(nm,".rodata")||ark_streq(nm,".bss"))goto fail;if(ark_streq(nm,".data")||ark_streq(nm,".init_array")||ark_streq(nm,".fini_array"))continue;for(j=0;j<custom;j++)if(ark_streq(cn[j],nm))break;if(j==custom){cn[custom++]=nm;ncap+=(unsigned)ark_strlen(nm)*2+16;}}
 base=11+custom;nsec=base+5+custom;
 sec=mem_pool_alloc(nsec*(int)sizeof(*sec));rs=mem_pool_alloc(base*(int)sizeof(*rs));cursor=mem_pool_alloc(nsec*(int)sizeof(*cursor));ms=mem_pool_alloc((len+1)*(int)sizeof(*ms));mo=mem_pool_alloc((len+1)*(int)sizeof(*mo));names=mem_pool_alloc(ncap);if(!sec||!rs||!cursor||!ms||!mo||!names)goto fail;
 for(i=0;i<nsec;i++){unsigned*v=(unsigned*)&sec[i];for(j=0;j<9;j++)v[j]=0;cursor[i]=0;}for(i=0;i<base;i++)rs[i]=0;names[0]=0;
 {const char*bn[]={"",".text",".data",".bss",".rodata",".symtab",".strtab",".shstrtab",".note.GNU-stack",".init_array",".fini_array"};for(i=1;i<base;i++){sec[i].name=trname(names,&ns,i<11?bn[i]:cn[i-11]);sec[i].type=1;sec[i].align=1;}}
 sec[1].flags=6;sec[1].align=16;sec[1].size=(unsigned)a->sec_pc[RV_SEC_TEXT];sec[2].flags=3;sec[2].align=8;sec[3].flags=3;sec[3].type=8;sec[3].align=8;sec[4].flags=2;sec[4].align=16;sec[4].size=(unsigned)a->sec_pc[RV_SEC_RODATA];
 sec[5].type=2;sec[5].link=6;sec[5].align=4;sec[5].entsize=16;sec[6].type=sec[7].type=3;
 for(i=9;i<base;i++){sec[i].flags=3;sec[i].align=8;if(i<11){sec[i].type=i==9?14:15;sec[i].align=4;sec[i].entsize=4;}}
 k=base;for(i=1;i<base;i++)if(i==1||i==2||i==4||i>=9){const char*nm=names+sec[i].name;unsigned start=ns;names[ns++]='.';names[ns++]='r';names[ns++]='e';names[ns++]='l';names[ns++]='a';trname(names,&ns,nm);rs[i]=k;sec[k].name=start;sec[k].type=4;sec[k].link=5;sec[k].info=(unsigned)i;sec[k].align=4;sec[k].entsize=12;k++;}
 for(i=0;i<=len;i++){ms[i]=-1;mo[i]=0;}
 for(i=0;i<a->sec_pc[RV_SEC_TEXT];i++){if(i>=len)goto fail;ms[i]=1;mo[i]=(unsigned)i;}
 for(i=0;i<a->sec_pc[RV_SEC_RODATA];i++){int pos=a->sec_base[RV_SEC_RODATA]+i;if(pos<0||pos>=len)goto fail;ms[pos]=4;mo[pos]=(unsigned)i;}
 for(i=0;i<p->global_count;i++){
  IrGlobal*g=p->globals[i];RvLabel*l=0;int z=g->type?ark_agg_storage_size(g->type,g->init_aggregate):4,sc;unsigned al=(unsigned)(g->type?type_align(g->type):4),dest;
  if(g->is_extern)continue;if(z<=0)z=4;if(al<4)al=4;if(al>8)al=8;if(g->align_bytes>(int)al)al=(unsigned)g->align_bytes;if(al&(al-1))goto fail;
  for(j=0;j<a->label_count;j++)if(a->labels[j].num<0&&ark_streq(a->labels[j].name,g->name)){l=&a->labels[j];break;}
  if(!l||l->sec!=RV_SEC_DATA)goto fail;sc=g->has_init?2:3;
  if(g->section){if(ark_streq(g->section,".data"))sc=2;else if(ark_streq(g->section,".init_array"))sc=9;else if(ark_streq(g->section,".fini_array"))sc=10;else{for(j=0;j<custom;j++)if(ark_streq(cn[j],g->section))break;sc=11+j;}}
  dest=tealign(sec[sc].size,al);sec[sc].size=dest+(unsigned)z;if(al>sec[sc].align)sec[sc].align=al;
  for(j=0;j<z;j++){int pos=a->sec_base[l->sec]+l->off+j;if(pos<0||pos>=len||ms[pos]!=-1)goto fail;ms[pos]=sc;mo[pos]=dest+(unsigned)j;}
 }
 /* Preserve data not represented by typed globals (e.g. raw assembler input). */
 for(i=0;i<a->sec_pc[RV_SEC_DATA];i++){int pos=a->sec_base[RV_SEC_DATA]+i;if(pos<0||pos>=len)goto fail;if(ms[pos]<0){ms[pos]=2;mo[pos]=sec[2].size++;}}
 for(i=0;i<n;i++)if(s[i].defined){unsigned old=s[i].value;RvLabel*l=0;for(j=0;j<a->label_count;j++)if(a->labels[j].num<0&&ark_streq(a->labels[j].name,s[i].name)){l=&a->labels[j];break;}if(l&&l->sec==RV_SEC_TEXT){s[i].defined=1;s[i].value=(unsigned)l->off;}else if(l&&l->sec==RV_SEC_RODATA){s[i].defined=4;s[i].value=(unsigned)l->off;}else{if(old>=(unsigned)len||ms[old]<0)goto fail;s[i].defined=ms[old];s[i].value=mo[old];}}
 for(i=0;i<nr;i++){unsigned old=r[i].off;int sc;if(old+4>(unsigned)len)goto fail;sc=ms[old];if(sc<1||sc==3||!rs[sc])goto fail;for(j=1;j<4;j++)if(ms[old+j]!=sc||mo[old+j]!=mo[old]+(unsigned)j)goto fail;r[i].off=mo[old];sec[rs[sc]].size+=12;}
 /* Constructor/destructor entries extend the actual init/fini arrays. */
 for(i=0;i<p->func_count;i++){IrFunc*f=p->funcs[i];int sym=-1;for(j=0;j<n;j++)if(ark_streq(s[j].name,f->name)){sym=j;break;}if(sym<0)continue;if(f->is_constructor){sec[9].size=tealign(sec[9].size,4)+4;sec[rs[9]].size+=12;}if(f->is_destructor){sec[10].size=tealign(sec[10].size,4)+4;sec[rs[10]].size+=12;}}
 for(i=0;i<n;i++){if(!s[i].bind)locals++;strsz+=(unsigned)ark_strlen(s[i].name)+1;}{int lo=1,gl=locals;for(i=0;i<n;i++)s[i].index=s[i].bind?gl++:lo++;}
 sec[5].size=(unsigned)(n+1)*16;sec[5].info=(unsigned)locals;sec[6].size=strsz;sec[7].size=ns;
 total=52;for(i=1;i<nsec;i++){total=tealign(total,sec[i].align);sec[i].off=total;cursor[i]=total;if(sec[i].type!=8)total+=sec[i].size;}shoff=tealign(total,4);total=shoff+(unsigned)nsec*40;
 out=mem_pool_alloc((int)total);if(!out)goto fail;for(i=0;i<(int)total;i++)out[i]=0;
 out[0]=127;out[1]='E';out[2]='L';out[3]='F';out[4]=1;out[5]=1;out[6]=1;te16(out+16,1);te16(out+18,d->machine);te32(out+20,1);te32(out+32,shoff);te32(out+36,d->flags);te16(out+40,52);te16(out+46,40);te16(out+48,(unsigned)nsec);te16(out+50,7);
 for(i=0;i<len;i++)if(ms[i]>0&&ms[i]!=3)out[sec[ms[i]].off+mo[i]]=code[i];
 strsz=1;for(i=0;i<n;i++){unsigned char*v=out+sec[5].off+s[i].index*16;int z=ark_strlen(s[i].name)+1;te32(v,strsz);te32(v+4,s[i].value);te32(v+8,s[i].size);v[12]=(unsigned char)((s[i].bind<<4)|s[i].type);te16(v+14,(unsigned)s[i].defined);for(j=0;j<z;j++)out[sec[6].off+strsz+j]=(unsigned char)s[i].name[j];strsz+=(unsigned)z;}
 /* r.off is now section relative; assembler reloc order retains original sites. */
 {int ri=0;for(i=0;i<a->reloc_count;i++){RvReloc*q=&a->relocs[i];int count=q->type==RV_RELOC_AUIPC?2:1;for(j=0;j<count;j++,ri++){int sc=ms[q->off+j*4],si=rs[sc];unsigned type;unsigned char*v=out+cursor[si];if(ri>=nr||xh_object_rules_relocation(d,(unsigned)r[ri].type,&type))goto fail;te32(v,r[ri].off);te32(v+4,((unsigned)s[r[ri].sym].index<<8)|type);te32(v+8,(unsigned)r[ri].add);cursor[si]+=12;}}if(ri!=nr)goto fail;}
 {unsigned at[2]={sec[9].size,sec[10].size};for(i=p->func_count-1;i>=0;i--){IrFunc*f=p->funcs[i];int sym=-1;for(j=0;j<n;j++)if(ark_streq(s[j].name,f->name)){sym=j;break;}if(sym<0)continue;for(j=0;j<2;j++)if(j?f->is_destructor:f->is_constructor){int si=rs[9+j];unsigned char*v=out+cursor[si];at[j]-=4;te32(v,at[j]);te32(v+4,((unsigned)s[sym].index<<8)|1u);cursor[si]+=12;}}}
 for(i=0;i<(int)ns;i++)out[sec[7].off+i]=(unsigned char)names[i];for(i=1;i<nsec;i++){unsigned char*v=out+shoff+i*40;te32(v,sec[i].name);te32(v+4,sec[i].type);te32(v+8,sec[i].flags);te32(v+16,sec[i].off);te32(v+20,sec[i].size);te32(v+24,sec[i].link);te32(v+28,sec[i].info);te32(v+32,sec[i].align);te32(v+36,sec[i].entsize);}
 if(fs_write_file(path,out,(int)total)!=(int)total)return ARK_ERR_IO;return ARK_OK;
 fail:diag_tool(DIAG_SEV_ERROR,path,"RV ELF section layout or relocation is invalid",0,0,0,0,0,0);return ARK_ERR_CODEGEN;
}
