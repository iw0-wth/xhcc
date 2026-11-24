/* Original bounded script parser. No GNU implementation code is used. */
#include "xh_ld_script.h"
#include <string.h>
#include <stdio.h>
#include <limits.h>

typedef struct { const char *s; size_t n,p,start; int tok,failed; uint32_t num; char word[XH_LD_MESSAGE]; XhLdScript *ir; char *err; size_t cap; } Parser;
enum { NAME=256, NUMBER, STRING, EQ, NE, LE, GE, AND, OR };
static int fail(Parser *p,const char *s) { if(!p->failed && p->cap) snprintf(p->err,p->cap,"ld script byte %lu: %s",(unsigned long)p->start,s); p->failed=1; return -1; }
static int alpha(int c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='.'||c=='$'; }
static int digit(int c) { return c>='0'&&c<='9'; }
static void next(Parser *p) {
    size_t a,k; int c; uint64_t v; unsigned base,d;
    if(p->failed) {p->tok=0;return;}
    for(;;) {
        while(p->p<p->n && (p->s[p->p]==' '||p->s[p->p]=='\t'||p->s[p->p]=='\r'||p->s[p->p]=='\n')) p->p++;
        if(p->p+1<p->n && p->s[p->p]=='/' && p->s[p->p+1]=='*') {
            p->p+=2; while(p->p+1<p->n && !(p->s[p->p]=='*'&&p->s[p->p+1]=='/')) p->p++;
            if(p->p+1>=p->n) {fail(p,"unterminated comment");p->tok=0;return;} p->p+=2;
        } else break;
    }
    p->start=p->p; if(p->p==p->n) {p->tok=0;return;} c=(unsigned char)p->s[p->p++];
    if(!c) {fail(p,"embedded NUL");p->tok=0;return;}
    if(c=='"') {
        k=0;
        while(p->p<p->n && p->s[p->p]!='"') {
            c=(unsigned char)p->s[p->p++];
            if(c=='\\') {
                if(p->p==p->n) {fail(p,"unterminated string escape");return;}
                c=(unsigned char)p->s[p->p++];
                if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';
                else if(c!='"'&&c!='\\') {fail(p,"unsupported string escape");return;}
            } else if(c<32||c>126) {fail(p,"invalid string character");return;}
            if(k+1>=sizeof(p->word)) {fail(p,"assertion message too long");return;}p->word[k++]=(char)c;
        }
        if(p->p==p->n) {fail(p,"unterminated string");return;}
        p->p++;p->word[k]=0;p->tok=STRING;return;
    }
    if(p->p<p->n) {
        int t=p->s[p->p],op=0;
        if(t=='=')op=c=='='?EQ:c=='!'?NE:c=='<'?LE:c=='>'?GE:0;
        else if(c=='&'&&t=='&')op=AND;else if(c=='|'&&t=='|')op=OR;
        if(op) {p->p++;p->tok=op;return;}
    }
    if(alpha(c)) {
        a=p->p-1; while(p->p<p->n && (alpha((unsigned char)p->s[p->p])||digit(p->s[p->p]))) p->p++;
        k=p->p-a; if(k>=XH_LD_NAME) {fail(p,"name too long");p->tok=0;return;}
        memcpy(p->word,p->s+a,k);p->word[k]=0;p->tok=NAME;return;
    }
    if(digit(c)) {
        base=c=='0'?8u:10u; v=(unsigned)(c-'0');
        if(c=='0' && p->p<p->n && (p->s[p->p]=='x'||p->s[p->p]=='X')) {base=16;v=0;p->p++; if(p->p==p->n) {fail(p,"invalid hex number");return;} }
        a=p->p;
        while(p->p<p->n) { c=(unsigned char)p->s[p->p]; d=digit(c)?(unsigned)(c-'0'):(c>='a'&&c<='f')?(unsigned)(c-'a'+10):(c>='A'&&c<='F')?(unsigned)(c-'A'+10):99; if(d>=base) break;v=v*base+d; if(v>UINT32_MAX) {fail(p,"number overflow");return;}p->p++; }
        if(base==16 && p->p==a) {fail(p,"invalid hex number");return;}
        if(p->p<p->n && (p->s[p->p]=='K'||p->s[p->p]=='k'||p->s[p->p]=='M'||p->s[p->p]=='m')) {c=p->s[p->p++];v*=c=='K'||c=='k'?1024u:1048576u;}
        if(v>UINT32_MAX) {fail(p,"number overflow");return;}p->num=(uint32_t)v;p->tok=NUMBER;return;
    }
    p->tok=c;
}
static int take(Parser *p,int tok) { if(p->tok!=tok) return fail(p,"unexpected token or unsupported syntax"); next(p);return p->failed?-1:0; }
static int is(Parser *p,const char *s) {return p->tok==NAME&&!strcmp(p->word,s);}
static int name(Parser *p,char *s) { if(p->tok!=NAME) return fail(p,"expected name"); strcpy(s,p->word);next(p);return 0; }
static int node(Parser *p,int kind,int l,int r,uint32_t v,const char *s) { XhLdExpr *e; unsigned i=p->ir->expr_count;if(i==XH_LD_MAX_EXPRS) return fail(p,"expression node limit");p->ir->expr_count++;e=&p->ir->exprs[i];e->kind=kind;e->left=l;e->right=r;e->value=v;if(s) strcpy(e->name,s);return (int)i; }
static int expr(Parser *p,int depth,int min);
static int primary(Parser *p,int depth) {
    char s[XH_LD_NAME];int k,n;
    if(depth>16) return fail(p,"expression nesting limit");
    if(p->tok==NUMBER) {uint32_t v=p->num;next(p);return node(p,XH_LD_VALUE,-1,-1,v,0);}
    if(p->tok=='(') {next(p);n=expr(p,depth+1,0);if(take(p,')'))return -1;return n;}
    if(p->tok=='+'||p->tok=='-'||p->tok=='!') {k=p->tok;next(p);n=primary(p,depth+1);return k=='+'?n:node(p,k=='!'?XH_LD_NOT:XH_LD_NEG,n,-1,0,0);}
    if(p->tok!=NAME) return fail(p,"expected expression");
    strcpy(s,p->word);next(p);
    if(!strcmp(s,".")) return node(p,XH_LD_DOT,-1,-1,0,0);
    k=!strcmp(s,"ORIGIN")?XH_LD_ORIGIN:!strcmp(s,"LENGTH")?XH_LD_LENGTH:!strcmp(s,"ADDR")?XH_LD_ADDR:!strcmp(s,"LOADADDR")?XH_LD_LOADADDR:!strcmp(s,"SIZEOF")?XH_LD_SIZEOF:!strcmp(s,"ALIGN")?XH_LD_ALIGN:0;
    if(k) {
        if(take(p,'('))return -1;
        if(k==XH_LD_ALIGN) {n=expr(p,depth+1,0);if(take(p,')'))return -1;return node(p,k,n,-1,0,0);}
        if(name(p,s)||take(p,')'))return -1;return node(p,k,-1,-1,0,s);
    }
    if(p->tok=='(') return fail(p,"unsupported expression function");
    return node(p,XH_LD_SYMBOL,-1,-1,0,s);
}
static int expr(Parser *p,int depth,int min) {
    int l=primary(p,depth),r,op,prec,kind;
    while(!p->failed) {
        op=p->tok;prec=op==OR?1:op==AND?2:op==EQ||op==NE?3:op=='<'||op=='>'||op==LE||op==GE?4:op=='+'||op=='-'?5:op=='*'||op=='/'?6:0;
        if(!prec||prec<min)break;
        kind=op==OR?XH_LD_OR:op==AND?XH_LD_AND:op==EQ?XH_LD_EQ:op==NE?XH_LD_NE:op=='<'?XH_LD_LT:op=='>'?XH_LD_GT:op==LE?XH_LD_LE:op==GE?XH_LD_GE:op=='+'?XH_LD_ADD:op=='-'?XH_LD_SUB:op=='*'?XH_LD_MUL:XH_LD_DIV;
        next(p);r=expr(p,depth+1,prec+1);l=node(p,kind,l,r,0,0);
    }
    return l;
}
static int eval(const XhLdScript *s,int id,uint32_t dot,XhLdLookup lookup,void *user,uint32_t *out,unsigned depth) {
    const XhLdExpr *e;uint32_t a=0,b=0;uint64_t v;unsigned i;
    if(id<0||(unsigned)id>=s->expr_count||depth>128)return -1;e=&s->exprs[id];
    switch(e->kind) {
    case XH_LD_VALUE:*out=e->value;return 0;
    case XH_LD_DOT:*out=dot;return 0;
    case XH_LD_ORIGIN:case XH_LD_LENGTH:
        for(i=0;i<s->region_count;i++)if(!strcmp(e->name,s->regions[i].name)){*out=e->kind==XH_LD_ORIGIN?s->regions[i].origin:s->regions[i].length;return 0;}return -1;
    case XH_LD_SYMBOL:case XH_LD_ADDR:case XH_LD_LOADADDR:case XH_LD_SIZEOF:return lookup?lookup(user,e->kind,e->name,out):-1;
    default:break;
    }
    if(eval(s,e->left,dot,lookup,user,&a,depth+1))return -1;
    if(e->kind==XH_LD_NOT) {*out=!a;return 0;}
    if((e->kind==XH_LD_AND&&!a)||(e->kind==XH_LD_OR&&a)) {*out=e->kind==XH_LD_OR;return 0;}
    if(e->kind==XH_LD_NEG) {if(a)return -1;*out=0;return 0;}
    if(e->kind==XH_LD_ALIGN) {if(!a||(a&(a-1)))return -1;v=((uint64_t)dot+a-1)&~((uint64_t)a-1);}
    else {
        if(eval(s,e->right,dot,lookup,user,&b,depth+1))return -1;
        switch(e->kind) {case XH_LD_ADD:v=(uint64_t)a+b;break;case XH_LD_SUB:if(a<b)return -1;v=a-b;break;case XH_LD_MUL:v=(uint64_t)a*b;break;case XH_LD_DIV:if(!b)return -1;v=a/b;break;
        case XH_LD_EQ:v=a==b;break;case XH_LD_NE:v=a!=b;break;
        case XH_LD_LT:v=a<b;break;case XH_LD_LE:v=a<=b;break;
        case XH_LD_GT:v=a>b;break;case XH_LD_GE:v=a>=b;break;
        case XH_LD_AND:v=!!b;break;case XH_LD_OR:v=!!b;break;default:return -1;}
    }
    if(v>UINT32_MAX)return -1;*out=(uint32_t)v;return 0;
}
int xh_ld_script_eval(const XhLdScript *s,int id,uint32_t dot,XhLdLookup lookup,void *user,uint32_t *out,char *err,size_t cap) {
    if(!s||!out||eval(s,id,dot,lookup,user,out,0)) {if(cap&&err)snprintf(err,cap,"ld script: unresolved or invalid/overflowing expression");return -1;}return 0;
}
static int region(Parser *p,const char *s) {unsigned i;for(i=0;i<p->ir->region_count;i++)if(!strcmp(s,p->ir->regions[i].name))return (int)i;return fail(p,"unknown memory region");}
static int event(Parser *p,int kind,int index,int sec) {XhLdEvent *e;if(p->ir->event_count==XH_LD_MAX_EVENTS)return fail(p,"event limit");e=&p->ir->events[p->ir->event_count++];e->kind=kind;e->index=index;e->section=sec;return 0;}
static int assignment(Parser *p,int sec) {
    char s[XH_LD_NAME];int id;unsigned i;XhLdSymbol *sym;
    if(name(p,s)||take(p,'='))return -1;id=expr(p,0,0);if(take(p,';'))return -1;
    if(!strcmp(s,"."))return event(p,XH_LD_SET_DOT,id,sec);
    if(p->ir->symbol_count==XH_LD_MAX_SYMBOLS)return fail(p,"symbol limit");
    for(i=0;i<p->ir->symbol_count;i++)if(!strcmp(s,p->ir->symbols[i].name))return fail(p,"duplicate symbol assignment");
    i=p->ir->symbol_count++;sym=&p->ir->symbols[i];strcpy(sym->name,s);sym->expr=id;sym->section=sec;sym->event=p->ir->event_count;return event(p,XH_LD_DEFINE,(int)i,sec);
}
static int assertion(Parser *p,int sec) {
    XhLdAssertion *a;unsigned i;int id;
    if(p->ir->assertion_count==XH_LD_MAX_ASSERTIONS)return fail(p,"assertion limit");
    next(p);if(take(p,'('))return -1;id=expr(p,0,0);if(take(p,','))return -1;
    if(p->tok!=STRING)return fail(p,"ASSERT requires quoted message");
    i=p->ir->assertion_count++;a=&p->ir->assertions[i];a->expr=id;a->section=sec;a->event=p->ir->event_count;strcpy(a->message,p->word);
    next(p);if(take(p,')'))return -1;if(p->tok==';')next(p);return event(p,XH_LD_ASSERT,(int)i,sec);
}
static int memory(Parser *p) {
    XhLdRegion *r;char s[XH_LD_NAME];unsigned i,j;int id;uint32_t v;
    next(p);if(take(p,'{'))return -1;
    while(p->tok!='}'&&!p->failed) {
        if(p->ir->region_count==XH_LD_MAX_REGIONS)return fail(p,"region limit");
        if(name(p,s))return -1;for(i=0;i<p->ir->region_count;i++)if(!strcmp(s,p->ir->regions[i].name))return fail(p,"duplicate region");
        r=&p->ir->regions[p->ir->region_count];strcpy(r->name,s);
        if(p->tok=='(') {next(p);if(p->tok!=NAME)return fail(p,"expected region attributes");for(j=0;p->word[j];j++){int c=p->word[j];if(c!='r'&&c!='w'&&c!='x')return fail(p,"unsupported region attribute");r->attrs|=c=='r'?1u:c=='w'?2u:4u;}next(p);if(take(p,')'))return -1;}
        if(take(p,':')||!is(p,"ORIGIN"))return fail(p,"expected ORIGIN");next(p);if(take(p,'='))return -1;
        id=expr(p,0,0);r->origin_expr=id;if(eval(p->ir,id,0,0,0,&v,0))return fail(p,"invalid region origin expression");r->origin=v;
        if(take(p,',')||!is(p,"LENGTH"))return fail(p,"expected LENGTH");next(p);if(take(p,'='))return -1;
        id=expr(p,0,0);r->length_expr=id;if(eval(p->ir,id,0,0,0,&v,0)||!v||(uint64_t)r->origin+v>UINT64_C(0x100000000))return fail(p,"invalid region length or range");r->length=v;p->ir->region_count++;
        if(p->tok==';')next(p);
    }return take(p,'}');
}
static int patterns(Parser *p,int sec,int keep) {
    XhLdPattern *pat;unsigned i;char s[XH_LD_NAME];
    if(take(p,'*')||take(p,'('))return -1;
    if(p->tok==')')return fail(p,"empty input pattern");
    while(p->tok!=')'&&!p->failed) {
        if(name(p,s))return -1;
        if(strcmp(s,"COMMON")&&s[0]!='.')return fail(p,"unsupported input pattern (only section names or COMMON)");
        if(p->tok=='*') {if(!strcmp(s,"COMMON"))return fail(p,"COMMON wildcard is unsupported");i=(unsigned)strlen(s);if(i+1>=sizeof(s))return fail(p,"pattern too long");s[i]='*';s[i+1]=0;next(p);}
        if(p->ir->pattern_count==XH_LD_MAX_PATTERNS)return fail(p,"pattern limit");
        i=p->ir->pattern_count++;pat=&p->ir->patterns[i];strcpy(pat->name,s);pat->common=!strcmp(s,"COMMON");pat->keep=(unsigned)keep;
        if(event(p,XH_LD_PATTERN,(int)i,sec))return -1;
    }return take(p,')');
}
static int sections(Parser *p) {
    XhLdSection *s;char n[XH_LD_NAME];int sec;unsigned i;
    next(p);if(take(p,'{'))return -1;
    while(p->tok!='}'&&!p->failed) {
        if(p->tok==';') {next(p);continue;}
        if(is(p,"ASSERT")) {if(assertion(p,-1))return -1;continue;}
        /* A name followed by '=' is an assignment, including location counter. */
        {Parser saved=*p; if(name(p,n))return -1;if(p->tok=='='){*p=saved;if(assignment(p,-1))return -1;continue;}*p=saved;}
        if(name(p,n))return -1;if(n[0]!='.'||!strcmp(n,"."))return fail(p,"unsupported output section");
        if(p->ir->section_count==XH_LD_MAX_SECTIONS)return fail(p,"section limit");
        for(i=0;i<p->ir->section_count;i++)if(!strcmp(n,p->ir->sections[i].name))return fail(p,"duplicate output section");
        sec=(int)p->ir->section_count++;s=&p->ir->sections[sec];strcpy(s->name,n);s->region=s->load_region=-1;s->address_expr=s->align_expr=-1;
        if(p->tok==NAME&&is(p,"ALIGN"))s->address_expr=expr(p,0,0);
        else if(p->tok!= ':' && p->tok!='(')s->address_expr=expr(p,0,0);
        if(p->tok=='(') {next(p);if(!is(p,"NOLOAD"))return fail(p,"unsupported section type");s->noload=1;next(p);if(take(p,')'))return -1;}
        if(take(p,':'))return -1;
        if(is(p,"ALIGN"))s->align_expr=expr(p,0,0);
        if(take(p,'{'))return -1;s->first_event=p->ir->event_count;
        while(p->tok!='}'&&!p->failed) {
            if(p->tok=='*') {if(patterns(p,sec,0))return -1;}
            else if(is(p,"KEEP")) {next(p);if(take(p,'(')||patterns(p,sec,1)||take(p,')'))return -1;}
            else if(is(p,"ASSERT")) {if(assertion(p,sec))return -1;}
            else if(p->tok==NAME) {if(assignment(p,sec))return -1;}
            else return fail(p,"unsupported section statement");
        }
        s->event_count=p->ir->event_count-s->first_event;
        if(take(p,'}')||take(p,'>')||name(p,n))return -1;s->region=region(p,n);if(s->region<0)return -1;s->load_region=s->region;
        if(is(p,"AT")) {next(p);if(take(p,'>')||name(p,n))return -1;s->load_region=region(p,n);if(s->load_region<0)return -1;}
        if(p->tok==';')next(p);
    }return take(p,'}');
}
int xh_ld_script_parse(const char *text,size_t len,XhLdScript *out,char *err,size_t cap) {
    Parser p;int seen_memory=0,seen_sections=0;
    if(err&&cap)err[0]=0;
    if(!out||!text||len>65536||(!err&&cap)) {if(err&&cap)snprintf(err,cap,"ld script: invalid input or text limit");return -1;}
    memset(out,0,sizeof(*out));memset(&p,0,sizeof(p));p.s=text;p.n=len;p.ir=out;p.err=err;p.cap=cap;next(&p);
    while(p.tok&&!p.failed) {
        if(p.tok==';')next(&p);
        else if(is(&p,"ENTRY")) {if(out->entry[0])return fail(&p,"duplicate ENTRY");next(&p);if(take(&p,'(')||name(&p,out->entry)||take(&p,')'))return -1;}
        else if(is(&p,"MEMORY")) {if(seen_memory++)return fail(&p,"duplicate MEMORY");if(memory(&p))return -1;}
        else if(is(&p,"SECTIONS")) {if(seen_sections++)return fail(&p,"duplicate SECTIONS");if(sections(&p))return -1;}
        else if(is(&p,"ASSERT")) {if(assertion(&p,-1))return -1;}
        else if(p.tok==NAME) {if(assignment(&p,-1))return -1;}
        else return fail(&p,"unsupported top-level statement");
    }
    if(p.failed)return -1;if(!seen_memory||!seen_sections||!out->section_count)return fail(&p,"MEMORY and nonempty SECTIONS required");return 0;
}
int xh_ld_script_match(const XhLdPattern *p,const char *name,int common) {
    size_t n;if(!p||!name)return 0;if(p->common)return !!common;if(common)return 0;n=strlen(p->name);if(n&&p->name[n-1]=='*')return !strncmp(p->name,name,n-1);return !strcmp(p->name,name);
}
