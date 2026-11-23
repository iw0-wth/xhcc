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
#include <dirent.h>
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
typedef struct Var { struct Var *next; char *name,*value; int simple,command,is_default,local,overridden; } Var;
typedef struct Recipe { struct Recipe *next; char *text; } Recipe;
typedef struct Binding {struct Binding *next;char *name,*value;int op,forced;} Binding;
typedef struct Rule { struct Rule *next; char *target,*deps; Recipe *recipes,*last; Binding *bindings,*last_binding;} Rule;
typedef struct Node { struct Node *next; char *name; int state,failed,changed; } Node;
typedef struct Make {
    Mem *mem; size_t used; Var *vars; Rule *rules,*last,*current;
    Node *nodes; char *first,*phony; int error,dry,force,silent,keep,recipe_conflict,multi_rule;
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
static int expand(Make *m,const char *s,const Auto *a,char *out,int depth);
/* Find separators only at the outer level of nested make references. */
static char *separator(char *s,char wanted) {
    char close[16];int level=0;
    for(;*s;s++) {
        if(*s=='$'&&(s[1]=='('||s[1]=='{')) {if(level==16)return 0;close[level++]=s[1]=='('?')':'}';s++;}
        else if(level&&*s==close[level-1])level--;
        else if(!level&&*s==wanted)return s;
    }return 0;
}
static int matchword(const char *pat,const char *word,const char **stem,size_t *length) {
    const char *percent=strchr(pat,'%');size_t pre,post,n=strlen(word);
    if(!percent){*stem=word;*length=0;return !strcmp(pat,word);}
    pre=(size_t)(percent-pat);post=strlen(percent+1);
    if(n<pre+post||strncmp(pat,word,pre)||strcmp(percent+1,word+n-post))return 0;
    *stem=word+pre;*length=n-pre-post;return 1;
}
static int replacement(Make *m,char *out,size_t *used,const char *to,const char *stem,size_t length) {
    const char *p=strchr(to,'%');char piece[TEXT];
    if(!p)return put(m,out,used,to);
    memcpy(piece,to,(size_t)(p-to));piece[p-to]=0;
    if(put(m,out,used,piece))return -1;
    memcpy(piece,stem,length);piece[length]=0;
    return put(m,out,used,piece)||put(m,out,used,p+1)?-1:0;
}
static int wordlist(Make *m,char *s,char **list) {
    int count=0;while(*s){while(isspace((unsigned char)*s))s++;if(!*s)break;
        if(count==WORDS)return fail(m,"too many function words",0);list[count++]=s;
        while(*s&&!isspace((unsigned char)*s))s++;if(*s)*s++=0;}return count;
}
static int comparewords(const void *a,const void *b) {return strcmp(*(const char *const *)a,*(const char *const *)b);}
static int transform(Make *m,int kind,char values[3][TEXT],char *out) {
    char *items[WORDS],*patterns[WORDS];int count,np=0,i,j;size_t used=0;out[0]=0;
    count=wordlist(m,values[kind==10?2:kind==9?0:1],items);if(count<0)return -1;
    if(kind==7||kind==8){np=wordlist(m,values[0],patterns);if(np<0)return -1;}
    if(kind==9)qsort(items,(size_t)count,sizeof(*items),comparewords);
    for(i=0;i<count;i++) {
        const char *stem="";size_t length=0;int match=0;
        if(kind==9&&i&&!strcmp(items[i],items[i-1]))continue;
        if(kind==7||kind==8){for(j=0;j<np;j++)if(matchword(patterns[j],items[i],&stem,&length)){match=1;break;}if((kind==7&&!match)||(kind==8&&match))continue;}
        if(used&&put(m,out,&used," "))return -1;
        if(kind==10&&matchword(values[0],items[i],&stem,&length)) {if(replacement(m,out,&used,values[1],stem,length))return -1;}
        else if(put(m,out,&used,items[i]))return -1;
    }return 0;
}
static int globmatch(const char *pat,const char *name) {
    const char *star=0,*retry=0;
    while(*name){if(*pat=='?'||*pat==*name){pat++;name++;}
        else if(*pat=='*'){star=pat++;retry=name;}
        else if(star){pat=star+1;name=++retry;}else return 0;}
    while(*pat=='*')pat++;return !*pat;
}
typedef struct Matches {char storage[TEXT];char *paths[WORDS];size_t used;int count;} Matches;
static int addmatch(Make *m,Matches *matches,const char *path) {
    int i;size_t n=strlen(path)+1;
    for(i=0;i<matches->count;i++)if(!strcmp(matches->paths[i],path))return 0;
    if(matches->count==WORDS)return fail(m,"too many wildcard paths",0);
    if(matches->used+n>TEXT)return fail(m,"wildcard paths exceed text limit",0);
    matches->paths[matches->count++]=matches->storage+matches->used;
    memcpy(matches->storage+matches->used,path,n);matches->used+=n;return 0;
}
static int wildcard(Make *m,char *patterns,char *out) {
    Matches *matches=(Matches *)alloc(m,sizeof(*matches));char *list[WORDS];int count,i,rc=-1;size_t used=0;
    if(!matches)return -1;out[0]=0;count=wordlist(m,patterns,list);if(count<0)goto done;
    for(i=0;i<count;i++) {
        char *pat=list[i],*slash=strrchr(pat,'/'),*base=slash?slash+1:pat;
        char dir[TEXT],path[TEXT];DIR *d;struct dirent *entry;struct stat st;
        size_t prefix=slash?(size_t)(slash-pat+1):0;
        if(strpbrk(pat,"[]\\")){fail(m,"unsupported wildcard pattern",pat);goto done;}
        if(slash){memcpy(dir,pat,prefix);dir[prefix]=0;if(strpbrk(dir,"*?")){fail(m,"wildcards in directory components unsupported",pat);goto done;}}
        else strcpy(dir,".");
        if(!strpbrk(base,"*?")){if(!stat(pat,&st)&&addmatch(m,matches,pat))goto done;continue;}
        d=opendir(dir);if(!d){if(errno==ENOENT||errno==ENOTDIR)continue;fail(m,"cannot enumerate wildcard directory",dir);goto done;}
        for(;;) {
            size_t len;errno=0;entry=readdir(d);
            if(!entry){if(errno)fail(m,"error enumerating wildcard directory",dir);break;}
            if(entry->d_name[0]=='.'&&base[0]!='.')continue;
            if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
            if(!globmatch(base,entry->d_name))continue;len=strlen(entry->d_name);
            if(prefix+len>=TEXT){fail(m,"wildcard path exceeds text limit",0);break;}
            memcpy(path,pat,prefix);memcpy(path+prefix,entry->d_name,len+1);
            if(strpbrk(path," \t\r\n")){fail(m,"wildcard path with whitespace unsupported",path);break;}
            if(addmatch(m,matches,path))break;
        }
        closedir(d);if(m->error)goto done;
    }
    qsort(matches->paths,(size_t)matches->count,sizeof(char *),comparewords);
    for(i=0;i<matches->count;i++){if(i&&put(m,out,&used," "))goto done;if(put(m,out,&used,matches->paths[i]))goto done;}
    rc=0;
done:release(m,matches,sizeof(*matches));return rc;
}
static int function_inner(Make *m,char *expr,const Auto *a,char *out,int depth,char values[3][TEXT]) {
    char *rest=expr,*raw[3],*p;int argc=1,i,kind;size_t used=0;
    while(*rest&&!isspace((unsigned char)*rest))rest++;
    if(!*rest)return 0;*rest++=0;while(isspace((unsigned char)*rest))rest++;
    if(!strcmp(expr,"strip"))kind=1;else if(!strcmp(expr,"addprefix"))kind=2;
    else if(!strcmp(expr,"addsuffix"))kind=3;else if(!strcmp(expr,"notdir"))kind=4;
    else if(!strcmp(expr,"basename"))kind=5;else if(!strcmp(expr,"subst"))kind=6;
    else if(!strcmp(expr,"filter"))kind=7;else if(!strcmp(expr,"filter-out"))kind=8;
    else if(!strcmp(expr,"sort"))kind=9;else if(!strcmp(expr,"patsubst"))kind=10;
    else if(!strcmp(expr,"wildcard"))kind=11;else if(!strcmp(expr,"origin"))kind=12;
    else if(!strcmp(expr,"error"))kind=13;else if(!strcmp(expr,"warning"))kind=14;
    else if(!strcmp(expr,"dir"))kind=15;else if(!strcmp(expr,"suffix"))kind=16;
    else if(!strcmp(expr,"word"))kind=17;else if(!strcmp(expr,"words"))kind=18;
    else if(!strcmp(expr,"if"))kind=19;else if(!strcmp(expr,"foreach"))kind=20;
    else if(!strcmp(expr,"findstring"))kind=21;else if(!strcmp(expr,"join"))kind=22;
    else if(!strcmp(expr,"value"))kind=23;else if(!strcmp(expr,"flavor"))kind=24;
    else return fail(m,"unsupported make function",expr);
    raw[0]=rest;
    if(kind==2||kind==3||kind==6||kind==7||kind==8||kind==10||kind==17||(kind>=19&&kind<=22)) {
        int required=kind==6||kind==10||kind==20?3:2;
        while(argc<required) {p=separator(raw[argc-1],',');if(!p)return fail(m,"missing function argument",expr);*p=0;raw[argc++]=p+1;}
    }
    if(kind==23||kind==24) {
        Var *v;out[0]=0;
        if(expand(m,raw[0],a,values[0],depth+1))return -1;
        v=variable(m,trim(values[0]));
        return put(m,out,&used,kind==23?(v?v->value:""):!v?"undefined":v->simple?"simple":"recursive")?-1:1;
    }
    if(kind==19) {
        char *other=separator(raw[1],',');if(other)*other++=0;
        if(expand(m,trim(raw[0]),a,values[0],depth+1))return -1;
        out[0]=0;return expand(m,*trim(values[0])?raw[1]:other?other:"",a,out,depth+1)?-1:1;
    }
    if(kind==20) {
        char *items[WORDS],*name;Var local;int count,rc=1;
        if(expand(m,raw[0],a,values[0],depth+1)||expand(m,raw[1],a,values[1],depth+1))return -1;
        name=trim(values[0]);if(!*name||strpbrk(name," \t\r\n"))return fail(m,"foreach expects one variable name",name);
        count=wordlist(m,values[1],items);if(count<0)return -1;
        memset(&local,0,sizeof(local));local.name=name;local.simple=1;local.local=1;local.next=m->vars;m->vars=&local;out[0]=0;
        for(i=0;i<count;i++) {
            local.value=items[i];
            if(expand(m,raw[2],a,values[2],depth+1)||(i&&put(m,out,&used," "))||put(m,out,&used,values[2])){rc=-1;break;}
        }
        m->vars=local.next;return rc;
    }
    for(i=0;i<argc;i++)if(expand(m,raw[i],a,values[i],depth+1))return -1;
    out[0]=0;
    if(kind==21)return put(m,out,&used,strstr(values[1],values[0])?values[0]:"")?-1:1;
    if(kind==22) {
        char *left[WORDS],*right[WORDS];int nl=wordlist(m,values[0],left),nr=wordlist(m,values[1],right),count;
        if(nl<0||nr<0)return -1;count=nl>nr?nl:nr;
        for(i=0;i<count;i++){if(i&&put(m,out,&used," "))return -1;if(i<nl&&put(m,out,&used,left[i]))return -1;if(i<nr&&put(m,out,&used,right[i]))return -1;}return 1;
    }
    if(kind==11)return wildcard(m,values[0],out)?-1:1;
    if(kind==12){char *name=trim(values[0]);Var *v=variable(m,name);const char *origin=!v?"undefined":v->local?"automatic":v->overridden?"override":v->command?"command line":v->is_default?"default":"file";
        if(a&&*name&&!name[1]&&strchr("@<^*",*name))origin="automatic";
        return put(m,out,&used,origin)?-1:1;}
    if(kind==13)return fail(m,"error",values[0]);
    if(kind==14){fprintf(stderr,"xhccm make: warning: %s\n",values[0]);return 1;}
    if(kind>=15) {
        char *items[WORDS];int count=wordlist(m,values[kind==17?1:0],items);
        if(count<0)return -1;
        if(kind==17){char *end,*index=trim(values[0]);unsigned long word;
            if(!isdigit((unsigned char)*index))return fail(m,"word index must be positive integer",index);
            errno=0;word=strtoul(index,&end,10);if(errno||*end||!word)return fail(m,"word index must be positive integer",index);
            if(word<=(unsigned long)count&&put(m,out,&used,items[word-1]))return -1;
        } else if(kind==18){char number[24];snprintf(number,sizeof(number),"%d",count);if(put(m,out,&used,number))return -1;}
        else for(i=0;i<count;i++) {
            char *base=strrchr(items[i],'/'),*part;
            if(kind==15){if(base){base[1]=0;part=items[i];}else part="./";}
            else {char *dot=strrchr(items[i],'.');if(!dot||(base&&dot<base))continue;part=dot;}
            if(used&&put(m,out,&used," "))return -1;if(put(m,out,&used,part))return -1;
        }return 1;
    }
    if(kind>=7)return transform(m,kind,values,out)?-1:1;
    if(kind==6) {
        size_t from=strlen(values[0]);p=values[2];
        while(*p){char c[2]={*p,0};if(from&&!strncmp(p,values[0],from)){if(put(m,out,&used,values[1]))return -1;p+=from;}else{if(put(m,out,&used,c))return -1;p++;}}
        if(!from&&put(m,out,&used,values[1]))return -1;
    } else {
        p=values[kind==2||kind==3?1:0];
        while(*p) {
            char *word,*end,*base,*dot;while(isspace((unsigned char)*p))p++;if(!*p)break;
            word=p;while(*p&&!isspace((unsigned char)*p))p++;end=p;if(*p)*p++=0;
            if(kind==4){base=strrchr(word,'/');if(base)word=base+1;}
            if(kind==5){base=strrchr(word,'/');dot=strrchr(word,'.');if(dot&&(!base||dot>base))*dot=0;}
            if(used&&put(m,out,&used," "))return -1;
            if(kind==2&&put(m,out,&used,values[0]))return -1;
            if(put(m,out,&used,word))return -1;
            if(kind==3&&put(m,out,&used,values[0]))return -1;
            (void)end;
        }
    }return 1;
}
static int function(Make *m,char *expr,const Auto *a,char *out,int depth) {
    char (*values)[TEXT];int rc;
    if(!strpbrk(expr," \t\r\n"))return 0;
    values=(char (*)[TEXT])alloc(m,3*TEXT);if(!values)return -1;
    rc=function_inner(m,expr,a,out,depth,values);release(m,values,3*TEXT);return rc;
}
static int substitution_ref(Make *m,char *expr,const Auto *a,char *out,int depth) {
    char *colon=separator(expr,':'),*equal;char (*values)[TEXT];Var *v;int rc=-1;
    if(!colon)return 0;*colon++=0;equal=separator(colon,'=');
    if(!equal)return fail(m,"substitution reference missing equals",0);*equal++=0;
    values=(char (*)[TEXT])alloc(m,3*TEXT);if(!values)return -1;
    if(expand(m,expr,a,values[2],depth+1))goto done;v=variable(m,values[2]);
    if(!v)values[2][0]=0;
    else if(v->simple){if(strlen(v->value)>=TEXT){fail(m,"variable text exceeds limit",0);goto done;}strcpy(values[2],v->value);}
    else if(expand(m,v->value,a,values[2],depth+1))goto done;
    if(expand(m,colon,a,values[0],depth+1)||expand(m,equal,a,values[1],depth+1))goto done;
    if(!strchr(values[0],'%')) {
        size_t n=strlen(values[0]),r=strlen(values[1]);
        if(n+1>=TEXT||r+1>=TEXT){fail(m,"substitution text exceeds limit",0);goto done;}
        memmove(values[0]+1,values[0],n+1);values[0][0]='%';
        memmove(values[1]+1,values[1],r+1);values[1][0]='%';
    }
    rc=transform(m,10,values,out)?-1:1;
done:release(m,values,3*TEXT);return rc;
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
            char name[TEXT],nested[TEXT],close[16];size_t k=0;int level=1,fr;Var *v;
            close[0]=*s++=='('?')':'}';
            while(*s&&level) {
                if(*s==close[level-1]){level--;if(!level){s++;break;}}
                else if(*s=='$'&&(s[1]=='('||s[1]=='{')) {
                    if(level==16)return fail(m,"variable nesting limit",0);
                    if(k+2>=sizeof(name))return fail(m,"variable expression too long",0);
                    name[k++]=*s++;close[level++]=*s=='('?')':'}';
                }
                if(k+1>=sizeof(name))return fail(m,"variable expression too long",0);name[k++]=*s++;
            }
            if(level)return fail(m,"unterminated variable reference",0);name[k]=0;
            if(a&&strlen(name)==2&&(name[0]=='@'||name[0]=='<')&&(name[1]=='D'||name[1]=='F')) {
                const char *path=name[0]=='@'?a->target:a->first,*slash=strrchr(path,'/');
                if(name[1]=='F')value=slash?slash+1:path;
                else if(!slash)value=".";
                else {size_t len=(size_t)(slash-path);if(!len)len=1;memcpy(nested,path,len);nested[len]=0;value=nested;}
                if(put(m,out,&n,value))return -1;continue;
            }
            fr=function(m,name,a,nested,depth);if(fr<0)return -1;
            if(fr){if(put(m,out,&n,nested))return -1;continue;}
            fr=substitution_ref(m,name,a,nested,depth);if(fr<0)return -1;
            if(fr){if(put(m,out,&n,nested))return -1;continue;}
            if(strchr(name,':'))return fail(m,"unsupported variable expression",name);
            if(strchr(name,'$')){if(expand(m,name,a,nested,depth+1))return -1;v=variable(m,nested);}else v=variable(m,name);
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
    int forced=command==2;if(forced)command=0;
    Var *v=variable(m,name);char text[TEXT];size_t n=0;
    if(!*name)return fail(m,"empty variable name",0);
    if(v&&(op=='?'||(!forced&&(v->overridden||(v->command&&!command)))))return 0;
    if(op==':'||(op=='+'&&v&&v->simple)) {if(expand(m,value,0,text,0))return -1;value=text;}
    if(op=='+'&&v) {char joined[TEXT];joined[0]=0;if(put(m,joined,&n,v->value)||(*v->value&&put(m,joined,&n," "))||put(m,joined,&n,value))return -1;v->value=copy(m,joined);}
    else {
        if(!v){v=(Var *)alloc(m,sizeof(*v));if(!v)return -1;v->name=copy(m,name);v->next=m->vars;m->vars=v;}
        v->value=copy(m,value);v->simple=op==':';
    }
    v->command=command;v->is_default=0;v->overridden=forced;return v->value&&v->name?0:-1;
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
static int undefine(Make *m,char *name,int forced) {
    char expanded[TEXT];Var **link=&m->vars,*v;
    if(expand(m,trim(name),0,expanded,0))return -1;
    name=trim(expanded);if(!*name||strpbrk(name," \t\r\n"))return fail(m,"undefine expects one variable name",name);
    while(*link&&strcmp((*link)->name,name))link=&(*link)->next;v=*link;
    if(!v||(!forced&&(v->command||v->overridden)))return 0;
    *link=v->next;release(m,v->name,strlen(v->name)+1);release(m,v->value,strlen(v->value)+1);release(m,v,sizeof(*v));return 0;
}
static char *readline(char *s,int cap,FILE *f) {
    char *r=fgets(s,cap,f);size_t n;if(!r)return r;n=strlen(s);
    if(n>=2&&s[n-2]=='\r'&&s[n-1]=='\n'){s[n-2]='\n';s[n-1]=0;}return r;
}
typedef struct Condition {int parent,match,had_else;} Condition;
static int condition(Make *m,char *s,Condition *stack,int *count,int *active) {
    char *arg=s;int kind=0,match=0;
    {const char *names[]={"ifeq","ifneq","ifdef","ifndef","else","endif"};int j;
     for(j=0;j<6;j++){size_t len=strlen(names[j]);if(!strncmp(s,names[j],len)&&(!s[len]||isspace((unsigned char)s[len])))break;}
     if(j==6)return 0;}
    while(*arg&&!isspace((unsigned char)*arg))arg++;
    if(*arg)*arg++=0;arg=trim(arg);
    if(!strcmp(s,"ifeq"))kind=1;else if(!strcmp(s,"ifneq"))kind=2;
    else if(!strcmp(s,"ifdef"))kind=3;else if(!strcmp(s,"ifndef"))kind=4;
    else if(!strcmp(s,"else"))kind=5;else if(!strcmp(s,"endif"))kind=6;else return 0;
    if(kind>=5) {
        Condition *c;if(!*count)return fail(m,"unmatched conditional",s);
        if(*arg)return fail(m,"unsupported conditional suffix",arg);
        c=&stack[*count-1];
        if(kind==5){if(c->had_else)return fail(m,"duplicate else",0);c->had_else=1;*active=c->parent&&!c->match;}
        else {*active=c->parent;(*count)--;}return 1;
    }
    if(*count==8)return fail(m,"conditional nesting limit",0);
    if(*active) {
        char expanded[TEXT];
        if(kind<=2) {
            char *comma,*end;char lhs[TEXT],rhs[TEXT];size_t len=strlen(arg);
            if(len<3||*arg!='('||arg[len-1]!=')')return fail(m,"conditional expects (left,right)",s);
            arg[len-1]=0;arg++;comma=separator(arg,',');if(!comma)return fail(m,"conditional missing comma",s);*comma++=0;
            end=trim(arg);comma=trim(comma);
            if(expand(m,end,0,lhs,0)||expand(m,comma,0,rhs,0))return -1;
            match=!strcmp(lhs,rhs);if(kind==2)match=!match;
        } else {
            Var *v;if(expand(m,arg,0,expanded,0))return -1;
            if(!*expanded||strpbrk(expanded," \t"))return fail(m,"conditional expects variable name",arg);
            v=variable(m,expanded);match=v&&*v->value;if(kind==4)match=!match;
        }
    }
    stack[*count].parent=*active;stack[*count].match=match;stack[*count].had_else=0;(*count)++;
    *active=*active&&match;return 1;
}
typedef struct MergeDeps {char old[TEXT],added[TEXT],result[TEXT];char *names[WORDS];int normal[WORDS],count;} MergeDeps;
static int merge_dependencies(Make *m,Rule *rule,const char *text) {
    MergeDeps *d=(MergeDeps *)alloc(m,sizeof(*d));char *sets[2];int k,i,j,rc=-1;size_t used=0;
    if(!d)return -1;strcpy(d->old,rule->deps?rule->deps:"");strcpy(d->added,text);sets[0]=d->old;sets[1]=d->added;
    for(k=0;k<2;k++) {
        char *part=sets[k],*bar=separator(part,'|'),*args[WORDS+1];int count,phase;
        if(bar){*bar++=0;if(separator(bar,'|')){fail(m,"multiple order-only separators",0);goto done;}}
        for(phase=0;phase<2;phase++) {
            if(phase&&!bar)break;count=words(m,phase?bar:part,args,0);if(count<0)goto done;
            for(i=0;i<count;i++) {
                for(j=0;j<d->count;j++)if(!strcmp(d->names[j],args[i]))break;
                if(j==d->count){if(j==WORDS){fail(m,"too many merged prerequisites",0);goto done;}d->names[d->count++]=args[i];}
                if(!phase)d->normal[j]=1;
            }
        }
    }
    for(k=1;k>=0;k--) {
        int any=0;for(i=0;i<d->count;i++)if(d->normal[i]==k)any=1;
        if(!k&&any&&put(m,d->result,&used," |"))goto done;
        for(i=0;i<d->count;i++)if(d->normal[i]==k) {
            if(used&&put(m,d->result,&used," "))goto done;
            /* Canonical quoting keeps prerequisite spaces intact. */
            if(put(m,d->result,&used,"\""))goto done;
            {const char *p=d->names[i];for(;*p;p++){char c[2]={*p,0};if((*p=='"'||*p=='\\')&&put(m,d->result,&used,"\\"))goto done;if(put(m,d->result,&used,c))goto done;}}
            if(put(m,d->result,&used,"\""))goto done;
        }
    }
    {char *merged=copy(m,d->result);if(!merged)goto done;if(rule->deps)release(m,rule->deps,strlen(rule->deps)+1);rule->deps=merged;}
    rc=0;
done:release(m,d,sizeof(*d));return rc;
}
static Rule *explicit_rule(Make *m,const char *target) {
    Rule *r;for(r=m->rules;r;r=r->next)if(!strcmp(r->target,target))return r;
    r=(Rule *)alloc(m,sizeof(*r));if(!r)return 0;r->target=copy(m,target);r->deps=copy(m,"");if(!r->target||!r->deps)return 0;
    if(m->last)m->last->next=r;else m->rules=r;m->last=r;return r;
}
static int target_assignment(Make *m,char *target,char *text) {
    char *eq=separator(text,'='),*name,*value,*names[WORDS+1];int forced=0,op='=',n;Rule *r;Binding *b;
    if(!eq)return 0;
    if(!strncmp(text,"override",8)&&isspace((unsigned char)text[8])){forced=1;text=trim(text+8);eq=separator(text,'=');}
    if(!eq)return fail(m,"target assignment needs equals",0);
    if(eq>text&&strchr(":?+",eq[-1]))op=*--eq;
    value=strchr(eq,'=')+1;*eq=0;name=trim(text);value=trim(value);
    if(!*name||strpbrk(name," \t\r\n"))return fail(m,"invalid target variable name",name);
    n=words(m,target,names,0);if(n!=1)return fail(m,"target variables require one target",0);
    {char *percent=strchr(names[0],'%');if(percent&&strchr(percent+1,'%'))return fail(m,"multiple percent signs in variable pattern",names[0]);}
    r=explicit_rule(m,names[0]);if(!r)return -1;
    b=(Binding *)alloc(m,sizeof(*b));if(!b)return -1;b->name=copy(m,name);b->op=op;b->forced=forced;
    if(op==':'){char expanded[TEXT];if(expand(m,value,0,expanded,0))return -1;b->value=copy(m,expanded);}else b->value=copy(m,value);
    if(!b->name||!b->value)return -1;
    if(r->last_binding)r->last_binding->next=b;else r->bindings=b;r->last_binding=b;
    m->current=0;m->multi_rule=0;m->recipe_conflict=0;return 1;
}
static int parse(Make *m,const char *path,int optional,int depth) {
    FILE *f;char line[TEXT];int line_no=0,ncond=0,active=1,group_count=0;Condition conditions[8];Rule *group[WORDS];
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
            Recipe *r;int j;if(!active)continue;
            if(m->recipe_conflict){fail(m,"conflicting recipes for target",m->current?m->current->target:path);break;}
            if(!m->current){fail(m,"recipe without a target",path);break;}
            s=line+1;len=strlen(s);while(len&&(s[len-1]=='\n'||s[len-1]=='\r'))s[--len]=0;
            r=(Recipe *)alloc(m,sizeof(*r));if(!r)break;r->text=copy(m,s);if(!r->text)break;
            if(m->current->last)m->current->last->next=r;
            for(j=0;j<group_count;j++){if(!group[j]->recipes)group[j]->recipes=r;group[j]->last=r;}
            continue;
        }
        { char *p=line;int escaped=0;while(*p){if(*p=='#'&&!escaped){*p=0;break;}escaped=*p=='\\'&&!escaped;p++;} }
        s=trim(line);if(!*s)continue;
        ar=condition(m,s,conditions,&ncond,&active);if(ar<0)break;if(ar)continue;
        if(!active)continue;
        if(*s=='$'){char expanded[TEXT];if(expand(m,s,0,expanded,0))break;if(!*trim(expanded))continue;fail(m,"unsupported expanded makefile statement",expanded);break;}
        if(!strncmp(s,"override",8)&&(!s[8]||isspace((unsigned char)s[8]))) {
            s=trim(s+8);
            if(!strncmp(s,"undefine",8)&&(!s[8]||isspace((unsigned char)s[8]))) {if(undefine(m,s+8,1))break;}
            else {ar=assignment(m,s,2);if(ar<0)break;if(!ar){fail(m,"override expects assignment or undefine",s);break;}}
            m->current=0;continue;
        }
        if(!strncmp(s,"undefine",8)&&(!s[8]||isspace((unsigned char)s[8]))) {if(undefine(m,s+8,0))break;m->current=0;continue;}
        ar=assignment(m,s,0);if(ar<0)break;if(ar){m->current=0;continue;}
        if(!strncmp(s,"include ",8)||!strncmp(s,"-include ",9)||!strncmp(s,"sinclude ",9)) {
            int opt=*s!='i',n,i;char expanded[TEXT],*args[WORDS+1];s+=opt?9:8;
            if(expand(m,s,0,expanded,0))break;n=words(m,expanded,args,0);if(n<0)break;
            for(i=0;i<n;i++)if(parse(m,args[i],opt,depth+1))break;
            if(m->error)break;m->current=0;continue;
        }
        colon=strchr(s,':');if(!colon){fail(m,"unsupported makefile statement",s);break;}
        *colon++=0;
        {char target[TEXT],deps[TEXT],*ts[WORDS+1];int n,i;Rule *r;
            if(expand(m,trim(s),0,target,0))break;
            ar=target_assignment(m,target,trim(colon));if(ar<0)break;if(ar)continue;
            if(expand(m,trim(colon),0,deps,0))break;
            if(strchr(deps,';')||strchr(deps,':')){fail(m,"unsupported rule syntax",deps);break;}
            if(!strcmp(target,".PHONY")) {char merged[TEXT];size_t z=0;merged[0]=0;if(m->phony&&(put(m,merged,&z,m->phony)||put(m,merged,&z," ")))break;if(put(m,merged,&z,deps))break;m->phony=copy(m,merged);m->current=0;continue;}
            if(strchr(target,'&')){fail(m,"grouped targets unsupported",0);break;}
            n=words(m,target,ts,0);if(n<=0){fail(m,"rule needs a target",0);break;}
            m->multi_rule=n>1;m->recipe_conflict=0;m->current=0;group_count=0;
            for(i=0;i<n;i++) {
                if(n>1&&strchr(ts[i],'%')){fail(m,"multiple pattern targets unsupported",0);break;}
                r=0;if(!strchr(ts[i],'%'))for(r=m->rules;r;r=r->next)if(!strcmp(r->target,ts[i]))break;
                if(!r){r=(Rule *)alloc(m,sizeof(*r));if(!r)break;r->target=copy(m,ts[i]);if(!r->target)break;
                    if(m->last)m->last->next=r;else m->rules=r;m->last=r;}
                if(merge_dependencies(m,r,deps))break;
                {int j;for(j=0;j<group_count;j++)if(group[j]==r)break;
                 if(j==group_count)group[group_count++]=r;}
                if(!m->current)m->current=r;
                if(r->recipes)m->recipe_conflict=1;
                if(!m->first&&r->target[0]!='.'&&!strchr(r->target,'%'))m->first=r->target;
            }
            if(m->error)break;
        }
    }
    if(ncond&&!m->error)fail(m,"unterminated conditional",path);
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
typedef struct Frame {char deps[TEXT],all[TEXT],stem[TEXT];char *dv[WORDS+1];Rule combined;} Frame;
static int build(Make *m,const char *name,int depth);
static int build_impl(Make *m,const char *name,int depth) {
    Node *node,*depnode;Rule *r,*chosen=0;Recipe *recipe;struct stat st,depst;
    Frame *frame=0;char *deps,*all,*stem,**dv;int dn,i,present,dirty,phony,normal;size_t used=0;
    if(strlen(name)>=TEXT)return fail(m,"target path too long",0);
    if(depth>DEPTH)return fail(m,"dependency nesting limit",name);
    for(node=m->nodes;node;node=node->next)if(!strcmp(node->name,name))break;
    if(node){if(node->state==1)return fail(m,"dependency cycle",name);return node->failed?-1:0;}
    node=(Node *)alloc(m,sizeof(*node));if(!node)return -1;node->name=copy(m,name);if(!node->name)return -1;node->next=m->nodes;m->nodes=node;node->state=1;
    frame=(Frame *)alloc(m,sizeof(*frame));if(!frame)goto bad;
    deps=frame->deps;all=frame->all;stem=frame->stem;dv=frame->dv;
    for(r=m->rules;r;r=r->next)if(!strcmp(r->target,name)){chosen=r;break;}
    if(!chosen||!chosen->recipes)for(r=m->rules;r;r=r->next)if(r->recipes&&pattern(r->target,name,stem)) {
        if(chosen) {
            Rule *combined=&frame->combined;char *expanded=(char *)alloc(m,TEXT);
            if(!expanded)goto bad;if(substitute(m,r->deps,stem,expanded)){release(m,expanded,TEXT);goto bad;}
            if(merge_dependencies(m,combined,expanded)||merge_dependencies(m,combined,chosen->deps)){release(m,expanded,TEXT);goto bad;}
            release(m,expanded,TEXT);combined->target=(char *)name;combined->recipes=r->recipes;chosen=combined;
        }else chosen=r;break;
    }
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
    /* Separate order-only dependencies before tokenization; accept a | with
     * or without adjacent whitespace. Quoted path separators remain literal. */
    {char *p=deps,*bar=0;int quote=0;
     for(;*p;p++){if(*p=='\\'&&p[1]){p++;continue;}if(*p=='\''||*p=='"'){if(!quote)quote=*p;else if(quote==*p)quote=0;continue;}
       if(*p=='|'&&!quote){if(bar){fail(m,"multiple order-only separators",0);goto bad;}bar=p;}}
     if(bar){int more,j;char *ov[WORDS+1];*bar=0;dn=words(m,deps,dv,0);if(dn<0)goto bad;normal=dn;
       more=words(m,bar+1,ov,0);if(more<0)goto bad;if(dn+more>WORDS){fail(m,"too many prerequisites",0);goto bad;}for(j=0;j<more;j++)dv[dn+j]=ov[j];dn+=more;
     }else {dn=words(m,deps,dv,0);normal=dn;}}
    if(dn<0)goto bad;all[0]=0;
    /* An existing file without an explicit rule is a leaf when no implicit
     * source exists. Preserve implicit rebuilds when the source is present. */
    if(!chosen&&present&&dn&&!exists(dv[0],&depst)) {
        node->state=2;release(m,frame,sizeof(*frame));return 0;
    }
    for(i=0;i<dn;i++) {
        int j;if(build(m,dv[i],depth+1)){node->failed=1;if(!m->keep)goto bad;continue;}
        if(i>=normal)continue;
        if(!exists(dv[i],&depst)||!present||depst.st_mtime>st.st_mtime||isphony(m,dv[i]))dirty=1;
        for(depnode=m->nodes;depnode;depnode=depnode->next)if(!strcmp(depnode->name,dv[i])){if(depnode->changed)dirty=1;break;}
        for(j=0;j<i;j++)if(!strcmp(dv[j],dv[i]))break;
        if(j==i){if(used&&put(m,all,&used," "))goto bad;if(put(m,all,&used,dv[i]))goto bad;}
    }
    for(node=m->nodes;node;node=node->next)if(!strcmp(node->name,name))break;
    if(node->failed)goto bad;
    if(dirty) {
        Auto a;a.target=name;a.first=normal?dv[0]:"";a.all=all;a.stem=stem;
        if(chosen)for(recipe=chosen->recipes;recipe;recipe=recipe->next)if(execute(m,recipe->text,&a))goto bad;
        if(!chosen&&dn&&execute(m,"$(CC) $(CPPFLAGS) $(CFLAGS) -c \"$<\" -o \"$@\"",&a))goto bad;
        node->changed=1;
    }
    node->state=2;if(frame->combined.deps)release(m,frame->combined.deps,strlen(frame->combined.deps)+1);release(m,frame,sizeof(*frame));return 0;
bad:
    for(node=m->nodes;node;node=node->next)if(!strcmp(node->name,name))break;
    if(node){node->state=2;node->failed=1;}if(frame&&frame->combined.deps)release(m,frame->combined.deps,strlen(frame->combined.deps)+1);release(m,frame,sizeof(*frame));return -1;
}
static int apply_bindings(Make *m,Binding *bindings) {
    Binding *b;
    for(b=bindings;b;b=b->next) {
        Var *existing=variable(m,b->name),*local;
        if(existing&&(b->op=='?'||(!b->forced&&(existing->command||existing->overridden))))continue;
        local=(Var *)alloc(m,sizeof(*local));if(!local)return -1;
        if(existing){local->simple=existing->simple;local->value=copy(m,existing->value);}else local->value=copy(m,"");
        local->name=copy(m,b->name);if(!local->name||!local->value)return -1;
        local->next=m->vars;m->vars=local;
        if(setvar(m,b->name,b->value,(b->op==':'||b->op=='?')?'=':b->op,b->forced?2:0))return -1;
        if(b->op==':')local->simple=1;
    }return 0;
}
typedef struct PatternBindings {Rule *rules[WORDS];size_t stems[WORDS];int count;} PatternBindings;
static int build(Make *m,const char *name,int depth) {
    Rule *rule,*explicit=0;Var *saved=m->vars,*v;PatternBindings *matches=0;int rc=-1,i,j;
    for(rule=m->rules;rule;rule=rule->next)if(rule->bindings) {
        const char *stem;size_t length;
        if(!strchr(rule->target,'%')){if(!strcmp(rule->target,name))explicit=rule;continue;}
        if(!matchword(rule->target,name,&stem,&length))continue;
        if(!matches){matches=(PatternBindings *)alloc(m,sizeof(*matches));if(!matches)goto done;}
        if(matches->count==WORDS){fail(m,"too many matching variable patterns",name);goto done;}
        i=matches->count++;while(i&&matches->stems[i-1]<length){matches->rules[i]=matches->rules[i-1];matches->stems[i]=matches->stems[i-1];i--;}
        matches->rules[i]=rule;matches->stems[i]=length;
    }
    if(matches)for(j=0;j<matches->count;j++)if(apply_bindings(m,matches->rules[j]->bindings))goto done;
    if(explicit&&apply_bindings(m,explicit->bindings))goto done;
    rc=build_impl(m,name,depth);
done:
    while(m->vars!=saved){v=m->vars;m->vars=v->next;release(m,v->name,strlen(v->name)+1);release(m,v->value,strlen(v->value)+1);release(m,v,sizeof(*v));}
    release(m,matches,sizeof(*matches));return rc;
}
int xh_make_main(int argc,char **argv) {
    Make m;int i,rc=0,nt=0;char *targets[WORDS],*file="Makefile",cwd[TEXT];Mem *p;
    memset(&m,0,sizeof(m));if(!getcwd(cwd,sizeof(cwd)))return 1;
    setvar(&m,"CC","arm-none-eabi-gcc",'=',0);
    if(variable(&m,"CC"))variable(&m,"CC")->is_default=1;
    if(argc&&argv[0][0]!='-'&&!strchr(argv[0],'=')&&
       xh_package_has_tool(getenv("XHCC_PACKAGE_ROOT")?getenv("XHCC_PACKAGE_ROOT"):"packages",argv[0])>0) {
        setvar(&m,"CC",argv[0],'=',0);if(variable(&m,"CC"))variable(&m,"CC")->is_default=1;argv++;argc--;
    }
    for(i=0;i<argc;i++) {
        char *arg=argv[i];
        if(!strcmp(arg,"--always-make"))m.force=1;
        else if(!strcmp(arg,"--dry-run"))m.dry=1;
        else if(!strcmp(arg,"--silent"))m.silent=1;
        else if(!strcmp(arg,"--keep-going"))m.keep=1;
        else if(!strncmp(arg,"--file=",7)){if(!arg[7]){fail(&m,"option needs value",arg);break;}file=arg+7;}
        else if(!strncmp(arg,"--directory=",12)){if(!arg[12]||chdir(arg+12)){fail(&m,"cannot change directory",arg+12);break;}}
        else if(!strcmp(arg,"-f")||!strcmp(arg,"-C")||!strcmp(arg,"--file")||!strcmp(arg,"--directory")) {int isfile=!strcmp(arg,"-f")||!strcmp(arg,"--file");if(++i==argc){fail(&m,"option needs value",arg);break;}if(isfile)file=argv[i];else if(chdir(argv[i])){fail(&m,"cannot change directory",argv[i]);break;}}
        else if(!strncmp(arg,"-j",2)) {const char *jobs=arg+2;if(!*jobs&&i+1<argc)jobs=argv[++i];if(strcmp(jobs,"1")){fail(&m,"only serial -j1 is supported",arg);break;}}
        else if(arg[0]=='-'&&arg[1]) {char *c=arg+1;for(;*c;c++){if(*c=='n')m.dry=1;else if(*c=='B')m.force=1;else if(*c=='s')m.silent=1;else if(*c=='k')m.keep=1;else {fail(&m,"unsupported option",arg);break;}}}
        else if(strchr(arg,'=')) {char *s=copy(&m,arg);if(!s||assignment(&m,s,1)<0)break;}
        else if(nt<WORDS)targets[nt++]=arg;else {fail(&m,"too many targets",0);break;}
    }
    if(!m.error&&parse(&m,file,0,0)==0) {
        if(!nt){
            Var *goal=variable(&m,".DEFAULT_GOAL");char expanded[TEXT],*goals[WORDS+1];int count=0;
            if(goal&&*goal->value){if(!expand(&m,goal->value,0,expanded,0)){count=words(&m,expanded,goals,0);if(count!=1)fail(&m,".DEFAULT_GOAL must name one target",0);else targets[nt++]=copy(&m,goals[0]);}}
            else if(m.first)targets[nt++]=m.first;else fail(&m,"no default target",0);
        }
        if(!m.error)for(i=0;i<nt;i++)if(build(&m,targets[i],0)){rc=1;if(!m.keep)break;}
    }
    if(m.error)rc=1;if(chdir(cwd))rc=1;
    while((p=m.mem)!=0){m.mem=p->next;free(p);}return rc;
}
