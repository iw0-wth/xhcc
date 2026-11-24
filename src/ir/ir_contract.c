/* Language-neutral core engine: no C AST, tokens, Type graph or allocator. */
#include "xh_ir_contract.h"

static XhIrCoreStatus fail(XhIrCoreError *e, XhIrCoreStatus s, uint32_t i) {
    if (e) { e->status=s; e->instruction=i; } return s;
}
int xh_ir_value_type_valid(const XhIrValueType *t) {
    if (!t) return 0;
    switch (t->category) {
    case XH_IR_VALUE_VOID:
        return !t->byte_width && !t->signedness && !t->address_space && !t->aggregate_bytes;
    case XH_IR_VALUE_INTEGER:
        return t->byte_width && t->byte_width<=16 &&
            (t->signedness==XH_IR_SIGN_SIGNED || t->signedness==XH_IR_SIGN_UNSIGNED) &&
            !t->address_space && !t->aggregate_bytes;
    case XH_IR_VALUE_FLOAT:
        return (t->byte_width==2 || t->byte_width==4 || t->byte_width==8 || t->byte_width==16) &&
            !t->signedness && !t->address_space && !t->aggregate_bytes;
    case XH_IR_VALUE_POINTER:
        return t->byte_width && t->byte_width<=16 && !t->signedness && !t->aggregate_bytes;
    case XH_IR_VALUE_AGGREGATE:
        return !t->byte_width && !t->signedness && !t->address_space && t->aggregate_bytes;
    default: return 0;
    }
}
static int equal(const XhIrValueType *a,const XhIrValueType *b) {
    return a->category==b->category && a->byte_width==b->byte_width &&
        a->signedness==b->signedness && a->address_space==b->address_space &&
        a->aggregate_bytes==b->aggregate_bytes;
}
static int executable(const XhIrValueType *t) {
    return t->category==XH_IR_VALUE_INTEGER && t->byte_width==4;
}
unsigned xh_ir_core_effects(const XhIrCoreInst *i) {
    if (!i) return ~0u;
    if (i->opcode==XH_IR_CORE_RETURN) return XH_IR_EFFECT_TERMINATOR;
    if (i->opcode==XH_IR_CORE_CONST) return XH_IR_EFFECT_PURE;
    if (i->opcode!=XH_IR_CORE_BINARY || i->binary_op<1 || i->binary_op>16) return ~0u;
    return i->binary_op==XH_IR_BIN_DIV || i->binary_op==XH_IR_BIN_REM ||
        i->binary_op==XH_IR_BIN_SHL || i->binary_op==XH_IR_BIN_SHR ?
        XH_IR_EFFECT_MAY_TRAP : XH_IR_EFFECT_PURE;
}
XhIrCoreStatus xh_ir_core_validate(const XhIrCoreProgram *p,XhIrCoreError *e) {
    XhIrValueType stack[XH_IR_CORE_MAX_STACK]; uint32_t n=0,k;
    if (!p || p->version!=XH_IR_CORE_VERSION) return fail(e,XH_IR_CORE_BAD_VERSION,0);
    if (!xh_ir_value_type_valid(&p->result)) return fail(e,XH_IR_CORE_BAD_TYPE,0);
    if (!executable(&p->result)) return fail(e,XH_IR_CORE_UNSUPPORTED_TYPE,0);
    if (!p->code || !p->code_count || p->code_count>XH_IR_CORE_MAX_CODE) return fail(e,XH_IR_CORE_LIMIT,0);
    for (k=0;k<p->code_count;k++) {
        const XhIrCoreInst *i=&p->code[k];
        if (!xh_ir_value_type_valid(&i->type)) return fail(e,XH_IR_CORE_BAD_TYPE,k);
        if (!executable(&i->type)) return fail(e,XH_IR_CORE_UNSUPPORTED_TYPE,k);
        switch (i->opcode) {
        case XH_IR_CORE_CONST:
            if (i->binary_op || i->bits>UINT32_MAX) return fail(e,XH_IR_CORE_BAD_OPERAND,k);
            if (n==XH_IR_CORE_MAX_STACK) return fail(e,XH_IR_CORE_STACK_OVERFLOW,k);
            stack[n++]=i->type; break;
        case XH_IR_CORE_BINARY:
            if (i->binary_op<1 || i->binary_op>16 || i->bits) return fail(e,XH_IR_CORE_BAD_OPERAND,k);
            if (n<2) return fail(e,XH_IR_CORE_STACK_UNDERFLOW,k);
            if (!equal(&stack[n-1],&i->type) || !equal(&stack[n-2],&i->type)) return fail(e,XH_IR_CORE_TYPE_MISMATCH,k);
            --n;
            /* Comparisons produce unsigned i32 boolean 0/1. */
            if (i->binary_op>=XH_IR_BIN_EQ) stack[n-1].signedness=XH_IR_SIGN_UNSIGNED;
            break;
        case XH_IR_CORE_RETURN:
            if (i->binary_op || i->bits || n!=1 || k+1!=p->code_count) return fail(e,XH_IR_CORE_BAD_RETURN,k);
            if (!equal(&stack[0],&i->type) || !equal(&p->result,&i->type)) return fail(e,XH_IR_CORE_TYPE_MISMATCH,k);
            return fail(e,XH_IR_CORE_OK,k);
        default: return fail(e,XH_IR_CORE_BAD_OPCODE,k);
        }
    }
    return fail(e,XH_IR_CORE_BAD_RETURN,p->code_count);
}
static void put32(unsigned char *b,uint32_t v) {
    unsigned k;for(k=0;k<4;k++)b[k]=(unsigned char)(v>>(k*8));
}
static uint32_t get32(const unsigned char *b) {
    return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}
static void puttype(unsigned char *b,const XhIrValueType *t) {
    put32(b,t->category);put32(b+4,t->byte_width);put32(b+8,t->signedness);
    put32(b+12,t->address_space);put32(b+16,t->aggregate_bytes);
}
static void gettype(const unsigned char *b,XhIrValueType *t) {
    t->category=get32(b);t->byte_width=get32(b+4);t->signedness=get32(b+8);
    t->address_space=get32(b+12);t->aggregate_bytes=get32(b+16);
}
/* 40-byte header: magic[8], version, total bytes, result[20], count.
 * 36-byte instruction: opcode, binary-op, type[20], raw bits[8]. */
int xh_ir_core_encode(const XhIrCoreProgram *p,unsigned char *out,uint32_t cap) {
    uint32_t k,bytes;
    if (xh_ir_core_validate(p,0)!=XH_IR_CORE_OK || !out) return -1;
    bytes=40+36*p->code_count;if(cap<bytes)return -1;
    for(k=0;k<8;k++)out[k]=(unsigned char)XH_IR_CORE_MAGIC[k];
    put32(out+8,p->version);put32(out+12,bytes);puttype(out+16,&p->result);put32(out+36,p->code_count);
    for(k=0;k<p->code_count;k++) {
        unsigned char *b=out+40+36*k;const XhIrCoreInst *i=&p->code[k];
        put32(b,i->opcode);put32(b+4,i->binary_op);puttype(b+8,&i->type);
        put32(b+28,(uint32_t)i->bits);put32(b+32,(uint32_t)(i->bits>>32));
    }
    return (int)bytes;
}
XhIrCoreStatus xh_ir_core_decode(const unsigned char *b,uint32_t len,
    XhIrCoreProgram *p,XhIrCoreInst *storage,uint32_t cap,XhIrCoreError *e) {
    uint32_t k,n;XhIrCoreProgram tmp;XhIrCoreStatus s;
    if (!p) return fail(e,XH_IR_CORE_BAD_WIRE,0);
    p->version=0;p->code=0;p->code_count=0;
    p->result.category=p->result.byte_width=p->result.signedness=p->result.address_space=p->result.aggregate_bytes=0;
    if (!b || len<40) return fail(e,XH_IR_CORE_BAD_WIRE,0);
    for(k=0;k<8;k++)if(b[k]!=(unsigned char)XH_IR_CORE_MAGIC[k])return fail(e,XH_IR_CORE_BAD_WIRE,0);
    if(get32(b+8)!=XH_IR_CORE_VERSION)return fail(e,XH_IR_CORE_BAD_VERSION,0);
    n=get32(b+36);
    if (!n || n>XH_IR_CORE_MAX_CODE || n>cap || !storage) return fail(e,XH_IR_CORE_LIMIT,0);
    if (len!=40+36*n || get32(b+12)!=len) return fail(e,XH_IR_CORE_BAD_WIRE,0);
    tmp.version=get32(b+8);gettype(b+16,&tmp.result);tmp.code=storage;tmp.code_count=n;
    for(k=0;k<n;k++) {
        const unsigned char *q=b+40+36*k;XhIrCoreInst *i=&storage[k];
        i->opcode=get32(q);i->binary_op=get32(q+4);gettype(q+8,&i->type);
        i->bits=(uint64_t)get32(q+28)|((uint64_t)get32(q+32)<<32);
    }
    s=xh_ir_core_validate(&tmp,e);if(s==XH_IR_CORE_OK)*p=tmp;return s;
}
