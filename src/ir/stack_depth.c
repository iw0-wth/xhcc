#include "xh_ir_stack.h"
#include <limits.h>

static XhStackStatus fail(XhStackError *e, XhStackStatus s, int i) {
    if (e) { e->status=s; e->instruction=i; } return s;
}
static int label(const IrInst *i) {
    return i->op==IR_LABEL || i->op==IR_LABEL_NAMED;
}
static int branch(const IrInst *i) {
    return i->op==IR_JMP || i->op==IR_JMP_NAMED || i->op==IR_JZ ||
           i->op==IR_JNZ || i->op==IR_CMP_JMP_EQ;
}
static int same_label(const IrInst *a, const IrInst *b) {
    if(a->op!=b->op)return 0;
    return a->op==IR_LABEL ? a->val==b->val : ark_streq(a->name,b->name);
}
static int valid_effect(const IrInst *i, const XhStackEffect *e) {
    if(e->pop<0 || e->push<0 || e->taken_pop<0 || e->taken_push<0 ||
       e->flow<XH_STACK_NEXT || e->flow>XH_STACK_BRANCH)return 0;
    if(i->op==IR_JMP || i->op==IR_JMP_NAMED)return e->flow==XH_STACK_JUMP;
    if(i->op==IR_JZ || i->op==IR_JNZ || i->op==IR_CMP_JMP_EQ)
        return e->flow==XH_STACK_BRANCH;
    if(i->op==IR_RETURN)return e->flow==XH_STACK_STOP;
    return e->flow==XH_STACK_NEXT || e->flow==XH_STACK_STOP;
}
static XhStackStatus join(int node, int count, int depth,
    XhStackWorkspace *w, int *tail) {
    if(node==count)return XH_STACK_OK;
    if(node<0 || node>count)return XH_STACK_BAD_TARGET;
    if(w->depth[node]>=0)
        return w->depth[node]==depth ? XH_STACK_OK : XH_STACK_JOIN_MISMATCH;
    w->depth[node]=depth; w->queue[(*tail)++]=node; return XH_STACK_OK;
}
static XhStackStatus after(int d,int pop,int push,int limit,int *out) {
    if(d<pop)return XH_STACK_UNDERFLOW;
    if(push>limit || d-pop>limit-push)return XH_STACK_LIMIT;
    *out=d-pop+push;return XH_STACK_OK;
}
XhStackStatus xh_ir_stack_validate(const IrFunc *f, XhStackEffectFn fn,
    void *ctx, XhStackWorkspace *w, XhStackError *error) {
    int n,i,j,head=0,tail=0;
    XhStackEffect e; XhStackStatus s;
    if(!f || !fn || !w)return fail(error,XH_STACK_BAD_INPUT,-1);
    n=f->code_count;
    if(n<0 || (n && !f->code) || w->capacity<n || w->max_words<=0 ||
       (n && (!w->depth || !w->queue || !w->target)))
        return fail(error,XH_STACK_BAD_INPUT,-1);
    /* All three scratch arrays must be distinct, caller-owned n-int spans. */
    if(n && (w->depth==w->queue || w->depth==w->target || w->queue==w->target))
        return fail(error,XH_STACK_BAD_INPUT,-1);
    for(i=0;i<n;i++){ w->depth[i]=-1; w->target[i]=-1; }
    for(i=0;i<n;i++) {
        const IrInst *I=&f->code[i];
        if(label(I)) {
            if((I->op==IR_LABEL && (I->val<0 || I->val>INT_MAX)) ||
               (I->op==IR_LABEL_NAMED && (!I->name || !*I->name)))
                return fail(error,XH_STACK_BAD_LABEL,i);
            for(j=0;j<i;j++)if(label(&f->code[j]) && same_label(I,&f->code[j]))
                return fail(error,XH_STACK_DUPLICATE_LABEL,i);
        }
        if(branch(I)) {
            int named=I->op==IR_JMP_NAMED;
            long long id=I->op==IR_CMP_JMP_EQ ? I->aux : I->val;
            if((named && (!I->name || !*I->name)) ||
               (!named && (id<0 || id>INT_MAX)))
                return fail(error,XH_STACK_BAD_TARGET,i);
            for(j=0;j<n;j++)if(named ?
                (f->code[j].op==IR_LABEL_NAMED && f->code[j].name &&
                 ark_streq(I->name,f->code[j].name)) :
                (f->code[j].op==IR_LABEL && f->code[j].val==id)) {
                w->target[i]=j;break;
            }
            if(w->target[i]<0)return fail(error,XH_STACK_BAD_TARGET,i);
        }
        e.pop=e.push=e.taken_pop=e.taken_push=0;e.flow=XH_STACK_NEXT;
        if(fn(ctx,f,i,&e))return fail(error,XH_STACK_UNSUPPORTED,i);
        if(!valid_effect(I,&e))return fail(error,XH_STACK_BAD_EFFECT,i);
        if(I->op==IR_ALLOCA && (e.pop!=1 || e.push!=1 || e.flow!=XH_STACK_NEXT))
            return fail(error,XH_STACK_BAD_ALLOCA,i);
    }
    if(n){w->depth[0]=0;w->queue[tail++]=0;}
    while(head<tail) {
        int node=w->queue[head++], d=w->depth[node], next,taken;
        e.pop=e.push=e.taken_pop=e.taken_push=0;e.flow=XH_STACK_NEXT;
        if(fn(ctx,f,node,&e))return fail(error,XH_STACK_UNSUPPORTED,node);
        if(!valid_effect(&f->code[node],&e))return fail(error,XH_STACK_BAD_EFFECT,node);
        s=after(d,e.pop,e.push,w->max_words,&next);
        if(s)return fail(error,s,node);
        if(e.flow==XH_STACK_STOP)continue;
        if(e.flow==XH_STACK_BRANCH) {
            s=after(d,e.taken_pop,e.taken_push,w->max_words,&taken);
            if(s)return fail(error,s,node);
            s=join(w->target[node],n,taken,w,&tail);
            if(s)return fail(error,s,node);
        }
        s=join(e.flow==XH_STACK_JUMP ? w->target[node] : node+1,n,next,w,&tail);
        if(s)return fail(error,s,node);
    }
    return fail(error,XH_STACK_OK,-1);
}
int xh_ir_stack_alloca_live_words(const IrFunc *f,const XhStackWorkspace *w,int i) {
    if(!f || !w || !w->depth || i<0 || i>=f->code_count || i>=w->capacity ||
       f->code[i].op!=IR_ALLOCA || w->depth[i]<1)return -1;
    return w->depth[i]-1;
}
