#include "xh_const_int.h"
#include "sema.h"
#include "symtab.h"

static int ci_integer(Type *t) {
    int n;
    if (!t || !type_is_integer(t)) return 0;
    n = type_size(t);
    return n == 1 || n == 2 || n == 4 || n == 8;
}
static unsigned long long ci_mask(Type *t) {
    int n = type_size(t) * 8;
    return n >= 64 ? ~0ULL : (1ULL << n) - 1ULL;
}
long long xh_const_int_signed(XhConstInt v) {
    unsigned long long m = ci_mask(v.type), s = (m >> 1) + 1ULL;
    v.bits &= m;
    if (v.type->is_unsigned && v.bits <= 9223372036854775807ULL) return (long long)v.bits;
    return v.bits & s ? -1LL - (long long)((~v.bits) & m) : (long long)v.bits;
}
static Type *ci_promote(Type *t) {
    return type_size(t) < 4 || t->kind == TYPE_ENUM || t->kind == TYPE_BOOL ? type_int() : t;
}
static void ci_convert(XhConstInt *v, Type *t) {
    if (t->kind == TYPE_BOOL) v->bits = v->bits != 0;
    else if (!v->type->is_unsigned) v->bits = (unsigned long long)xh_const_int_signed(*v);
    v->type = t; v->bits &= ci_mask(t);
}
/* Infer both conditional branches without executing their arithmetic. */
static Type *ci_type(AstNode *n) {
    Type *a, *b; int op;
    if (!n) return 0;
    if (n->kind == AST_NUM || n->kind == AST_SIZEOF || n->kind == AST_CAST)
        return ci_integer(n->type) ? n->type : 0;
    if (n->kind == AST_VAR) {
        Symbol *s = sym_find(n->u.name);
        return s && s->kind == SYM_ENUM_CONST ? type_int() : 0;
    }
    if (n->kind == AST_UNARY) {
        a = ci_type(n->u.unary.operand); op = n->u.unary.op;
        if (!a) return 0;
        return op == TOK_NOT ? type_int() :
            op == TOK_PLUS || op == TOK_MINUS || op == TOK_BNOT ? ci_promote(a) : 0;
    }
    if (n->kind == AST_TERNARY) {
        a = ci_type(n->u.ternary.true_expr); b = ci_type(n->u.ternary.false_expr);
        return a && b ? usual_arith_conv(a,b) : 0;
    }
    if (n->kind != AST_BINARY) return 0;
    a = ci_type(n->u.binary.left); b = ci_type(n->u.binary.right);
    if (!a || !b) return 0;
    op = n->u.binary.op;
    switch (op) {
    case TOK_EQ: case TOK_NE: case TOK_LT: case TOK_LE: case TOK_GT: case TOK_GE:
    case TOK_AND: case TOK_OR: return type_int();
    case TOK_SHL: case TOK_SHR: return ci_promote(a);
    case TOK_PLUS: case TOK_MINUS: case TOK_MUL: case TOK_DIV: case TOK_MOD:
    case TOK_BAND: case TOK_BOR: case TOK_BXOR: return usual_arith_conv(a,b);
    default: return 0;
    }
}
int xh_const_int_eval(AstNode *n, XhConstInt *out) {
    XhConstInt a,b; Type *t; unsigned long long mask,sign; long long x,y,lo,hi,r;
    int op,rc,width;
    if (!n || !out) return 0;
    if (n->kind == AST_NUM || n->kind == AST_SIZEOF) {
        t = ci_type(n);
        if (!t || (n->kind == AST_SIZEOF && (n->sizeof_operand || n->sizeof_is_vla))) return 0;
        out->type=t; out->bits=(unsigned long long)n->u.num_val & ci_mask(t); return 1;
    }
    if (n->kind == AST_VAR) {
        Symbol *s = sym_find(n->u.name);
        if (!s || s->kind != SYM_ENUM_CONST) return 0;
        out->type=type_int(); out->bits=(unsigned long long)s->enum_value & ci_mask(out->type); return 1;
    }
    if (n->kind == AST_CAST) {
        if (!ci_integer(n->type)) return 0;
        rc=xh_const_int_eval(n->u.unary.operand,&a); if (rc!=1) return rc;
        ci_convert(&a,n->type); *out=a; return 1;
    }
    if (n->kind == AST_TERNARY) {
        rc=xh_const_int_eval(n->u.ternary.cond,&a); if (rc!=1) return rc;
        t=ci_type(n); if (!t) return 0;
        rc=xh_const_int_eval(a.bits ? n->u.ternary.true_expr : n->u.ternary.false_expr,&b);
        if (rc!=1) return rc;
        ci_convert(&b,t); *out=b; return 1;
    }
    if (n->kind == AST_UNARY) {
        op=n->u.unary.op;
        if (op!=TOK_PLUS && op!=TOK_MINUS && op!=TOK_BNOT && op!=TOK_NOT) return 0;
        rc=xh_const_int_eval(n->u.unary.operand,&a); if (rc!=1) return rc;
        ci_convert(&a,ci_promote(a.type)); mask=ci_mask(a.type);
        if (op==TOK_NOT) { a.bits=!a.bits; a.type=type_int(); }
        else if (op==TOK_BNOT) a.bits=~a.bits & mask;
        else if (op==TOK_MINUS) {
            if (!a.type->is_unsigned && a.bits==(mask>>1)+1ULL) return -1;
            a.bits=(0ULL-a.bits)&mask;
        }
        *out=a; return 1;
    }
    if (n->kind!=AST_BINARY || !ci_type(n)) return 0;
    op=n->u.binary.op;
    rc=xh_const_int_eval(n->u.binary.left,&a); if (rc!=1) return rc;
    if ((op==TOK_AND && !a.bits) || (op==TOK_OR && a.bits)) {
        out->bits=op==TOK_OR; out->type=type_int(); return 1;
    }
    rc=xh_const_int_eval(n->u.binary.right,&b); if (rc!=1) return rc;
    if (op==TOK_AND || op==TOK_OR) { out->bits=b.bits!=0; out->type=type_int(); return 1; }
    if (op==TOK_SHL || op==TOK_SHR) {
        ci_convert(&a,ci_promote(a.type)); ci_convert(&b,ci_promote(b.type));
        width=type_size(a.type)*8;
        if ((!b.type->is_unsigned && xh_const_int_signed(b)<0) || b.bits>=(unsigned)width) return -1;
        mask=ci_mask(a.type);
        if (op==TOK_SHL) {
            if (!a.type->is_unsigned && (xh_const_int_signed(a)<0 || a.bits>((mask>>1)>>b.bits))) return -1;
            a.bits=(a.bits<<b.bits)&mask;
        } else if (!a.type->is_unsigned && xh_const_int_signed(a)<0) {
            /* Defined target policy: arithmetic signed right shift. */
            if (b.bits) a.bits=(a.bits>>b.bits) | (mask ^ (mask>>b.bits));
        } else a.bits>>=b.bits;
        *out=a; return 1;
    }
    t=usual_arith_conv(a.type,b.type); ci_convert(&a,t); ci_convert(&b,t);
    mask=ci_mask(t); sign=(mask>>1)+1ULL;
    if (op==TOK_EQ || op==TOK_NE || op==TOK_LT || op==TOK_LE || op==TOK_GT || op==TOK_GE) {
        x=xh_const_int_signed(a); y=xh_const_int_signed(b);
        switch(op) {
        case TOK_EQ:r=a.bits==b.bits;break; case TOK_NE:r=a.bits!=b.bits;break;
        case TOK_LT:r=t->is_unsigned?a.bits<b.bits:x<y;break;
        case TOK_LE:r=t->is_unsigned?a.bits<=b.bits:x<=y;break;
        case TOK_GT:r=t->is_unsigned?a.bits>b.bits:x>y;break;
        default:r=t->is_unsigned?a.bits>=b.bits:x>=y;break;
        }
        out->bits=(unsigned long long)r; out->type=type_int(); return 1;
    }
    if (op==TOK_BAND || op==TOK_BOR || op==TOK_BXOR) {
        a.bits=op==TOK_BAND ? a.bits&b.bits : op==TOK_BOR ? a.bits|b.bits : a.bits^b.bits;
    } else if (t->is_unsigned) {
        switch(op) {
        case TOK_PLUS:a.bits+=b.bits;break; case TOK_MINUS:a.bits-=b.bits;break;
        case TOK_MUL:a.bits*=b.bits;break;
        case TOK_DIV:if(!b.bits)return -1;a.bits/=b.bits;break;
        case TOK_MOD:if(!b.bits)return -1;a.bits%=b.bits;break;
        default:return 0;
        }
    } else {
        x=xh_const_int_signed(a); y=xh_const_int_signed(b); hi=(long long)(sign-1ULL); lo=-hi-1LL;
        switch(op) {
        case TOK_PLUS:if((y>0&&x>hi-y)||(y<0&&x<lo-y))return -1;r=x+y;break;
        case TOK_MINUS:if((y<0&&x>hi+y)||(y>0&&x<lo+y))return -1;r=x-y;break;
        case TOK_MUL:
            if(x>0?(y>0?x>hi/y:y<lo/x):(x<0&&(y>0?x<lo/y:y<0&&x<hi/y)))return -1;
            r=x*y;break;
        case TOK_DIV:case TOK_MOD:
            if(!y||(x==lo&&y==-1))return -1;
            r=op==TOK_DIV?x/y:x%y;break;
        default:return 0;
        }
        a.bits=(unsigned long long)r;
    }
    a.bits&=mask; *out=a; return 1;
}
