/* Deliberately small make language. Commands run in-process, never via a shell. */
#include "xh_make.h"
#include "xh_driver.h"
#include "xh_package.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#define getcwd _getcwd
#endif
#ifndef XH_MAKE_MEMORY
#define XH_MAKE_MEMORY (64u * 1024u)
#endif
#define TEXT 2048
#define WORDS 128
#define DEPTH 32

typedef struct Mem { struct Mem *next; } Mem;
typedef struct Var { struct Var *next; char *name,*value; int simple,command; } Var;
typedef struct Recipe { struct Recipe *next; char *text; } Recipe;
typedef struct Rule { struct Rule *next; char *target,*deps; Recipe *recipes,*last; } Rule;
typedef struct Node { struct Node *next; char *name; int state,failed,changed; } Node;
typedef struct Make {
    Mem *mem; size_t used; Var *vars; Rule *rules,*last,*current;
    Node *nodes; char *first,*phony; int error,dry,force,silent,keep;
} Make;
typedef struct Auto { const char *target,*first,*all,*stem; } Auto;
static int fail(Make *m,const char *what,const char *detail) {
    fprintf(stderr,"xhccm make: %s%s%s\n",what,detail?": ":"",detail?detail:"");
    m->error=1; return -1;
}
static void *alloc(Make *m,size_t n) {
    Mem *p;
    if(n > XH_MAKE_MEMORY || m->used + n + sizeof(Mem) > XH_MAKE_MEMORY) {
        fail(m,"make memory budget exceeded",0); return 0;
    }
    p=(Mem *)calloc(1,sizeof(Mem)+n);
    if(!p) { fail(m,"out of memory",0); return 0; }
    p->next=m->mem;m->mem=p;m->used+=sizeof(Mem)+n;return p+1;
}
static void release(Make *m,void *ptr,size_t n) {
    Mem **p=&m->mem,*entry;if(!ptr)return;
    entry=((Mem *)ptr)-1;while(*p&&*p!=entry)p=&(*p)->next;
    if(*p){*p=entry->next;m->used-=sizeof(Mem)+n;free(entry);}
}
static char *copy(Make *m,const char *s) {
    size_t n=strlen(s)+1; char *p=(char *)alloc(m,n); if(p)memcpy(p,s,n);return p;
}
static char *trim(char *s) {
    char *e;while(isspace((unsigned char)*s))s++;e=s+strlen(s);
    while(e>s&&isspace((unsigned char)e[-1]))*--e=0;return s;
}
static int put(Make *m,char *out,size_t *n,const char *s) {
    size_t len=strlen(s);if(*n+len>=TEXT)return fail(m,"expanded text exceeds limit",0);
    memcpy(out+*n,s,len+1);*n+=len;return 0;
}
static Var *variable(Make *m,const char *name) {
    Var *v;for(v=m->vars;v;v=v->next)if(!strcmp(v->name,name))return v;return 0;
}
static int expand(Make *m,const char *s,const Auto *a,char *out,int depth) {
    size_t n=0;out[0]=0;
    if(depth>8)return fail(m,"recursive variable expansion",s);
    while(*s) {
        char one[2]={0,0}; const char *value="";
        if(*s!='$') {one[0]=*s++;if(put(m,out,&n,one))return -1;continue;}
        s++;
        if(*s=='$'){one[0]='$';s++;if(put(m,out,&n,one))return -1;continue;}
        if(*s=='('||*s=='{') {
            char name[TEXT],nested[TEXT],close=*s++=='('?')':'}'; size_t k=0;Var *v;
            while(*s&&*s!=close) {if(k+1>=sizeof(name))return fail(m,"variable name too long",0);name[k++]=*s++;}
            if(!*s)return fail(m,"unterminated variable reference",0);s++;name[k]=0;
            if(strchr(name,' ')||strchr(name,'\t')||strchr(name,':')||strchr(name,'$'))return fail(m,"unsupported variable expression",name);
            v=variable(m,name);
            if(v) {if(v->simple)value=v->value;else {if(expand(m,v->value,a,nested,depth+1))return -1;value=nested;}}
            if(put(m,out,&n,value))return -1;
        } else {
            char name[2]={*s,0};Var *v;
            switch(*s) {
            case '@':value=a?a->target:"";break;case '<':value=a?a->first:"";break;
            case '^':value=a?a->all:"";break;case '*':value=a?a->stem:"";break;
            case 0:return fail(m,"trailing dollar",0);
            default:v=variable(m,name);if(v) {char nested[TEXT];if(expand(m,v->value,a,nested,depth+1))return -1;if(put(m,out,&n,nested))return -1;}s++;continue;
            }
            s++;if(put(m,out,&n,value))return -1;
        }
    }return 0;
}
static int setvar(Make *m,char *name,const char *value,int op,int command) {
    Var *v=variable(m,name);char text[TEXT];size_t n=0;
    if(!*name)return fail(m,"empty variable name",0);
    if(v&&((v->command&&!command)||op=='?'))return 0;
    if(op==':'||(op=='+'&&v&&v->simple)) {if(expand(m,value,0,text,0))return -1;value=text;}
    if(op=='+'&&v) {char joined[TEXT];joined[0]=0;if(put(m,joined,&n,v->value)||put(m,joined,&n," ")||put(m,joined,&n,value))return -1;v->value=copy(m,joined);}
    else {
        if(!v){v=(Var *)alloc(m,sizeof(*v));if(!v)return -1;v->name=copy(m,name);v->next=m->vars;m->vars=v;}
        v->value=copy(m,value);v->simple=op==':';
    }
    v->command=command;return v->value&&v->name?0:-1;
}
/* Split shell-like quoting only. No expansion, shell syntax, or subprocess. */
static int words(Make *m,char *s,char **v,int commands) {
    int n=0;char *p=s,*w=s;
    while(*p) {
        int quote=0;while(isspace((unsigned char)*p))p++;if(!*p)break;
        if(n>=WORDS)return fail(m,"too many command/graph words",0);v[n++]=w;
        while(*p&&(quote||!isspace((unsigned char)*p))) {
            char c=*p++;
            if(c=='\\'&&*p) {*w++=*p++;continue;}
            if(c=='\''||c=='"') {if(!quote){quote=c;continue;}if(quote==c){quote=0;continue;}}
            if(commands&&strchr(";&|<>`",c))return fail(m,"unsupported shell operator",0);
            *w++=c;
        }
        if(quote)return fail(m,"unterminated quote",0);
        while(isspace((unsigned char)*p))p++;*w++=0;
    }
    v[n]=0;return n;
}
static int assignment(Make *m,char *s,int command) {
    char *eq=strchr(s,'='),*name,*val;int op='=';
    if(!eq)return 0;
    /* An equals in prerequisites/recipe text is not a variable definition. */
    if(strchr(s,':')&&strchr(s,':')<eq&&strchr(s,':')!=eq-1)return 0;
    if(eq>s&&strchr(":?+",eq[-1]))op=*--eq;
    val=strchr(eq,'=')+1;*eq=0;name=trim(s);
    if(strchr(name,' ')||strchr(name,'\t'))return fail(m,"unsupported assignment",name);
    if(setvar(m,name,trim(val),op,command))return -1;return 1;
}
static char *readline(char *s,int cap,FILE *f) {
    char *r=fgets(s,cap,f);size_t n;if(!r)return r;n=strlen(s);
    if(n>=2&&s[n-2]=='\r'&&s[n-1]=='\n'){s[n-2]='\n';s[n-1]=0;}return r;
}
static int parse(Make *m,const char *path,int optional,int depth) {
    FILE *f;char line[TEXT];int line_no=0;
    if(depth>8)return fail(m,"include nesting/cycle limit",path);
    f=fopen(path,"r");if(!f){if(optional&&errno==ENOENT)return 0;return fail(m,"cannot read makefile",path);}
    while(readline(line,sizeof(line),f)) {
        size_t len=strlen(line);char *s,*colon;int recipe=line[0]=='\t',ar;line_no++;
        if(len==TEXT-1&&line[len-1]!='\n'){fail(m,"makefile line too long",path);break;}
        while(len>=2&&line[len-2]=='\\'&&line[len-1]=='\n') {
            line[len-2]=' ';len--;if(!readline(line+len-1,(int)(sizeof(line)-len+1),f))break;
            len=strlen(line);line_no++;if(len==TEXT-1&&line[len-1]!='\n'){fail(m,"continued line too long",path);break;}
        }
        if(m->error)break;
        if(recipe) {
            Recipe *r;if(!m->current){fail(m,"recipe without a target",path);break;}
            s=line+1;len=strlen(s);while(len&&(s[len-1]=='\n'||s[len-1]=='\r'))s[--len]=0;
            r=(Recipe *)alloc(m,sizeof(*r));if(!r)break;r->text=copy(m,s);if(!r->text)break;
            if(m->current->last)m->current->last->next=r;else m->current->recipes=r;m->current->last=r;continue;
        }
        { char *p=line;int escaped=0;while(*p){if(*p=='#'&&!escaped){*p=0;break;}escaped=*p=='\\'&&!escaped;p++;} }
        s=trim(line);if(!*s)continue;
        ar=assignment(m,s,0);if(ar<0)break;if(ar){m->current=0;continue;}
        if(!strncmp(s,"include ",8)||!strncmp(s,"-include ",9)||!strncmp(s,"sinclude ",9)) {
            int opt=*s!='i',n,i;char expanded[TEXT],*args[WORDS+1];s+=opt?9:8;
            if(expand(m,s,0,expanded,0))break;n=words(m,expanded,args,0);if(n<0)break;
            for(i=0;i<n;i++)if(parse(m,args[i],opt,depth+1))break;
            if(m->error)break;m->current=0;continue;
        }
        colon=strchr(s,':');if(!colon){fail(m,"unsupported makefile statement",s);break;}
        *colon++=0;
        {char target[TEXT],deps[TEXT],*ts[WORDS+1];int n;Rule *r;
            if(expand(m,trim(s),0,target,0)||expand(m,trim(colon),0,deps,0))break;
            if(strchr(deps,';')||strchr(deps,'|')||strchr(deps,':')){fail(m,"unsupported rule syntax",deps);break;}
            if(!strcmp(target,".PHONY")) {char merged[TEXT];size_t z=0;merged[0]=0;if(m->phony&&(put(m,merged,&z,m->phony)||put(m,merged,&z," ")))break;if(put(m,merged,&z,deps))break;m->phony=copy(m,merged);m->current=0;continue;}
            n=words(m,target,ts,0);if(n!=1){fail(m,"exactly one target per rule is supported",0);break;}
            r=(Rule *)alloc(m,sizeof(*r));if(!r)break;r->target=copy(m,ts[0]);r->deps=copy(m,deps);if(!r->target||!r->deps)break;
            if(m->last)m->last->next=r;else m->rules=r;m->last=r;m->current=r;
            if(!m->first&&r->target[0]!='.'&&!strchr(r->target,'%'))m->first=r->target;
        }
    }
    if(ferror(f))fail(m,"error reading makefile",path);fclose(f);(void)line_no;return m->error?-1:0;
}
static int exists(const char *path,struct stat *st){return stat(path,st)==0;}
static int isphony(Make *m,const char *name) {
    const char *p=m->phony?m->phony:"";size_t len=strlen(name);
    while(*p){while(isspace((unsigned char)*p))p++;if(!strncmp(p,name,len)&&(!p[len]||isspace((unsigned char)p[len])))return 1;while(*p&&!isspace((unsigned char)*p))p++;}return 0;
}
static int pattern(const char *pat,const char *target,char *stem) {
    const char *p=strchr(pat,'%');size_t pre,post,n;if(!p)return 0;pre=(size_t)(p-pat);post=strlen(p+1);n=strlen(target);
    if(n<pre+post||strncmp(pat,target,pre)||strcmp(p+1,target+n-post))return 0;
    memcpy(stem,target+pre,n-pre-post);stem[n-pre-post]=0;return 1;
}
static int substitute(Make *m,const char *s,const char *stem,char *out) {
    size_t n=0;out[0]=0;while(*s){char c[2]={*s++,0};if(put(m,out,&n,c[0]=='%'?stem:c))return -1;}return 0;
}
static int mkdir_one(const char *s) {
#ifdef _WIN32
    return _mkdir(s);
#else
    return mkdir(s,0777);
#endif
}
static int mkdir_p(char *s) {
    char *p;struct stat st;
    for(p=s+1;*p;p++)if(*p=='/'||*p=='\\'){char c=*p;*p=0;if(mkdir_one(s)&&errno!=EEXIST){*p=c;return -1;}*p=c;}
    if(mkdir_one(s)&&errno!=EEXIST)return -1;
    return stat(s,&st)||!S_ISDIR(st.st_mode)?-1:0;
}
static int execute(Make *m,const char *text,const Auto *a) {
    char expanded[TEXT],*args[WORDS+2],*s;int n,quiet=0,ignore=0,rc=0,i;
    s=(char *)text;while(*s=='@'||*s=='-'){if(*s=='@')quiet=1;else ignore=1;s++;}
    if(expand(m,s,a,expanded,0))return -1;
    if(m->dry||(!quiet&&!m->silent)) {puts(expanded);fflush(stdout);}
    n=words(m,expanded,args+1,1);if(n<0)return -1;if(!n)return 0;
    if(m->dry)return 0;
    args[0]="xhccm";
    if(!strcmp(args[1],"echo")) {for(i=2;i<=n;i++)printf("%s%s",i>2?" ":"",args[i]);putchar('\n');}
    else if(!strcmp(args[1],"mkdir")) {
        if(n<3||strcmp(args[2],"-p"))return fail(m,"only mkdir -p is supported",0);
        for(i=3;i<=n;i++)if(mkdir_p(args[i])){rc=1;break;}
    } else if(!strcmp(args[1],"rm")) {
        if(n<3||strcmp(args[2],"-f"))return fail(m,"only rm -f is supported",0);
        for(i=3;i<=n;i++)if(remove(args[i])&&errno!=ENOENT){rc=1;break;}
    } else if(!strcmp(args[1],"cp")) {
        FILE *in,*out;char buf[1024];size_t got;
        if(n!=3)return fail(m,"cp requires source and destination",0);
        in=fopen(args[2],"rb");if(!in)rc=1;else {out=fopen(args[3],"wb");if(!out)rc=1;else {while((got=fread(buf,1,sizeof(buf),in))!=0)if(fwrite(buf,1,got,out)!=got){rc=1;break;}if(ferror(in))rc=1;if(fclose(out))rc=1;}fclose(in);}
    } else {
        const char *root=getenv("XHCC_PACKAGE_ROOT");if(!root||!*root)root="packages";
        for(i=2;i<=n;i++) {
            if(!strcmp(args[i],"--package-root")&&i<n){root=args[i+1];break;}
            if(!strncmp(args[i],"--package-root=",15)){root=args[i]+15;break;}
        }
        /* Validate the command itself, even when it resembles a C filename. */
        if(xh_package_has_tool(root,args[1])!=1) {
            fprintf(stderr,"xhccm make: unknown compiler/command: %s\n",args[1]);rc=1;
        } else rc=xh_driver_main(n+1,args);
    }
    if(rc){fprintf(stderr,"xhccm make: recipe failed (%d)%s\n",rc,ignore?" (ignored)":"");if(!ignore)return -1;}
    return 0;
}
typedef struct Frame {char deps[TEXT],all[TEXT],stem[TEXT];char *dv[WORDS+1];} Frame;
static int build(Make *m,const char *name,int depth) {
    Node *node,*depnode;Rule *r,*chosen=0;Recipe *recipe;struct stat st,depst;
    Frame *frame=0;char *deps,*all,*stem,**dv;int dn,i,present,dirty,phony;size_t used=0;
    if(strlen(name)>=TEXT)return fail(m,"target path too long",0);
    if(depth>DEPTH)return fail(m,"dependency nesting limit",name);
    for(node=m->nodes;node;node=node->next)if(!strcmp(node->name,name))break;
    if(node){if(node->state==1)return fail(m,"dependency cycle",name);return node->failed?-1:0;}
    node=(Node *)alloc(m,sizeof(*node));if(!node)return -1;node->name=copy(m,name);if(!node->name)return -1;node->next=m->nodes;m->nodes=node;node->state=1;
    frame=(Frame *)alloc(m,sizeof(*frame));if(!frame)goto bad;
    deps=frame->deps;all=frame->all;stem=frame->stem;dv=frame->dv;
    for(r=m->rules;r;r=r->next)if(!strcmp(r->target,name)){chosen=r;break;}
    if(!chosen)for(r=m->rules;r;r=r->next)if(pattern(r->target,name,stem)){chosen=r;break;}
    if(!chosen&&strlen(name)>2&&!strcmp(name+strlen(name)-2,".o"))
        for(r=m->rules;r;r=r->next)if(!strcmp(r->target,".c.o")) {
            size_t len=strlen(name);chosen=r;memcpy(stem,name,len-2);stem[len-2]=0;break;
        }
    present=exists(name,&st);phony=isphony(m,name);dirty=m->force||!present||phony;
    if(chosen) {
        if(!strcmp(chosen->target,".c.o")){size_t n=0;deps[0]=0;if(put(m,deps,&n,"\"")||put(m,deps,&n,stem)||put(m,deps,&n,".c\""))goto bad;}
        else if(substitute(m,chosen->deps,stem,deps))goto bad;
    }
    else {
        size_t len=strlen(name);deps[0]=0;
        if(len>2&&!strcmp(name+len-2,".o")) {memcpy(stem,name,len-2);stem[len-2]=0;if(len+3>=TEXT){fail(m,"target path too long",name);goto bad;}deps[0]='"';memcpy(deps+1,name,len-1);deps[len]='c';deps[len+1]='"';deps[len+2]=0;}
        else if(!present&&!phony){fail(m,"no rule to make target",name);goto bad;}
    }
    dn=words(m,deps,dv,0);if(dn<0)goto bad;all[0]=0;
    /* An existing file without an explicit rule is a leaf when no implicit
     * source exists. Preserve implicit rebuilds when the source is present. */
    if(!chosen&&present&&dn&&!exists(dv[0],&depst)) {
        node->state=2;release(m,frame,sizeof(*frame));return 0;
    }
    for(i=0;i<dn;i++) {
        int j;if(build(m,dv[i],depth+1)){node->failed=1;if(!m->keep)goto bad;continue;}
        if(!exists(dv[i],&depst)||!present||depst.st_mtime>st.st_mtime||isphony(m,dv[i]))dirty=1;
        for(depnode=m->nodes;depnode;depnode=depnode->next)if(!strcmp(depnode->name,dv[i])){if(depnode->changed)dirty=1;break;}
        for(j=0;j<i;j++)if(!strcmp(dv[j],dv[i]))break;
        if(j==i){if(used&&put(m,all,&used," "))goto bad;if(put(m,all,&used,dv[i]))goto bad;}
    }
    for(node=m->nodes;node;node=node->next)if(!strcmp(node->name,name))break;
    if(node->failed)goto bad;
    if(dirty) {
        Auto a;a.target=name;a.first=dn?dv[0]:"";a.all=all;a.stem=stem;
        if(chosen)for(recipe=chosen->recipes;recipe;recipe=recipe->next)if(execute(m,recipe->text,&a))goto bad;
        if(!chosen&&dn&&execute(m,"$(CC) $(CPPFLAGS) $(CFLAGS) -c \"$<\" -o \"$@\"",&a))goto bad;
        node->changed=1;
    }
    node->state=2;release(m,frame,sizeof(*frame));return 0;
bad:
    for(node=m->nodes;node;node=node->next)if(!strcmp(node->name,name))break;
    if(node){node->state=2;node->failed=1;}release(m,frame,sizeof(*frame));return -1;
}
int xh_make_main(int argc,char **argv) {
    Make m;int i,rc=0,nt=0;char *targets[WORDS],*file="Makefile",cwd[TEXT];Mem *p;
    memset(&m,0,sizeof(m));if(!getcwd(cwd,sizeof(cwd)))return 1;
    setvar(&m,"CC","arm-none-eabi-gcc",'=',0);
    if(argc&&argv[0][0]!='-'&&!strchr(argv[0],'=')&&
       xh_package_has_tool(getenv("XHCC_PACKAGE_ROOT")?getenv("XHCC_PACKAGE_ROOT"):"packages",argv[0])>0) {
        setvar(&m,"CC",argv[0],'=',0);argv++;argc--;
    }
    for(i=0;i<argc;i++) {
        char *arg=argv[i];
        if(!strcmp(arg,"-f")||!strcmp(arg,"-C")) {if(++i==argc){fail(&m,"option needs value",arg);break;}if(arg[1]=='f')file=argv[i];else if(chdir(argv[i])){fail(&m,"cannot change directory",argv[i]);break;}}
        else if(arg[0]=='-'&&arg[1]) {char *c=arg+1;for(;*c;c++){if(*c=='n')m.dry=1;else if(*c=='B')m.force=1;else if(*c=='s')m.silent=1;else if(*c=='k')m.keep=1;else {fail(&m,"unsupported option",arg);break;}}}
        else if(strchr(arg,'=')) {char *s=copy(&m,arg);if(!s||assignment(&m,s,1)<0)break;}
        else if(nt<WORDS)targets[nt++]=arg;else {fail(&m,"too many targets",0);break;}
    }
    if(!m.error&&parse(&m,file,0,0)==0) {
        if(!nt){if(m.first)targets[nt++]=m.first;else fail(&m,"no default target",0);}
        for(i=0;i<nt;i++)if(build(&m,targets[i],0)){rc=1;if(!m.keep)break;}
    }
    if(m.error)rc=1;if(chdir(cwd))rc=1;
    while((p=m.mem)!=0){m.mem=p->next;free(p);}return rc;
}
