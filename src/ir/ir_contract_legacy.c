/* Compatibility adapter only: C token numbering and Type stay on this side. */
#include "xh_ir_legacy_adapter.h"
#define XH_BINARY_MAP(X) \
 X(ADD,PLUS) X(SUB,MINUS) X(MUL,MUL) X(DIV,DIV) X(REM,MOD) \
 X(AND,BAND) X(OR,BOR) X(XOR,BXOR) X(SHL,SHL) X(SHR,SHR) \
 X(EQ,EQ) X(NE,NE) X(LT,LT) X(LE,LE) X(GT,GT) X(GE,GE)
XhIrBinaryOp xh_ir_binary_from_c_token(int token) {
    switch(token) {
#define FROM(a,b) case TOK_##b: return XH_IR_BIN_##a;
    XH_BINARY_MAP(FROM)
#undef FROM
    default:return XH_IR_BIN_INVALID;
    }
}
int xh_ir_binary_to_c_token(XhIrBinaryOp op) {
    switch(op) {
#define TO(a,b) case XH_IR_BIN_##a: return TOK_##b;
    XH_BINARY_MAP(TO)
#undef TO
    default:return -1;
    }
}
IrProgram *xh_ir_core_to_legacy(const XhIrCoreProgram *core,const char *name,XhIrCoreError *error) {
    IrProgram *p;IrFunc *f;uint32_t k;
    if(xh_ir_core_validate(core,error)!=XH_IR_CORE_OK)return 0;
    if(!name || !name[0])return 0;
    p=ir_program_new();f=ir_func_new(name);
    f->ret_type=core->result.signedness==XH_IR_SIGN_UNSIGNED?type_uint():type_int();
    for(k=0;k<core->code_count;k++) {
        const XhIrCoreInst *i=&core->code[k];
        if(i->opcode==XH_IR_CORE_CONST) {
            /* Signed raw i32 bits need sign extension; unsigned retain magnitude. */
            long long value=i->type.signedness==XH_IR_SIGN_SIGNED?
                (long long)(int32_t)(uint32_t)i->bits:(long long)i->bits;
            ir_emit(f,IR_PUSH_INT,value,0,0);
        } else if(i->opcode==XH_IR_CORE_BINARY) {
            int token=xh_ir_binary_to_c_token((XhIrBinaryOp)i->binary_op);
            ir_emit(f,IR_BINARY,0,0,token|(i->type.signedness==XH_IR_SIGN_UNSIGNED?0x100:0));
        } else ir_emit(f,IR_RETURN,0,0,0);
    }
    ir_program_add(p,f);return p;
}
