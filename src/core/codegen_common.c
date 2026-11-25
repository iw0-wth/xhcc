/* Ark C Compiler - codegen common utilities
 * AsmBuffer: a lightweight dynamic byte buffer used by all backends
 * to accumulate emitted assembly or generated C source.
 */
#include "codegen.h"
#include "memory.h"

#define ASM_INIT_CAP 4096

void asmbuf_init(AsmBuffer *b) {
    b->cap=ASM_INIT_CAP;b->len=0;b->fixed=0;b->failed=0;
    b->buf=(char*)mem_pool_alloc(b->cap);b->buf[0]=0;
}
void asmbuf_init_fixed(AsmBuffer*b,char*storage,int capacity){
    b->buf=storage;b->cap=capacity>0?capacity:0;b->len=0;b->fixed=1;
    b->failed=(!storage||capacity<1)?1u:0u;
    if(storage&&capacity>0)storage[0]=0;
}
int asmbuf_failed(const AsmBuffer*b){return !b||b->failed;}
int asmbuf_require(AsmBuffer*b,int desired){
    int nc;char*next;
    if(!b||b->failed)return -1;
    if(desired<0||desired==2147483647||b->len<0||b->cap<0){b->failed=1;return -1;}
    if(desired<b->cap)return 0;
    if(b->fixed){b->failed=1;return -1;}
    nc=b->cap>0?b->cap:ASM_INIT_CAP;
    while(nc<=desired){if(nc>1073741823){nc=desired+1;break;}nc*=2;}
    next=(char*)mem_realloc(b->buf,b->cap,nc);
    if(!next){b->failed=1;return -1;}b->buf=next;b->cap=nc;return 0;
}
/* Compatibility: total denotes requested text bytes, NUL is additional. */
void asmbuf_reserve(AsmBuffer*b,int total){(void)asmbuf_require(b,total);}
static int asmbuf_grow(AsmBuffer*b,int extra){
    if(!b||b->failed)return -1;
    if(extra<0||b->len>2147483646-extra){b->failed=1;return -1;}
    return asmbuf_require(b,b->len+extra);
}
void asmbuf_putc(AsmBuffer*b,char c){
    if(asmbuf_grow(b,1))return;b->buf[b->len++]=c;b->buf[b->len]=0;
}
void asmbuf_puts(AsmBuffer*b,const char*s){
    int n,i;if(!b||b->failed)return;if(!s){b->failed=1;return;}
    n=ark_strlen(s);if(asmbuf_grow(b,n))return;
    for(i=0;i<n;i++)b->buf[b->len++]=s[i];b->buf[b->len]=0;
}

void asmbuf_int(AsmBuffer *b, long long v) {
    char tmp[24];
    int neg = 0;
    int n = 0;
    int i;
    unsigned long long x;
    if (v < 0) { neg = 1; x = (unsigned long long)(-v); }
    else x = (unsigned long long)v;
    if (x == 0) tmp[n++] = '0';
    while (x) {
        tmp[n++] = (char)('0' + (int)(x % 10));
        x /= 10;
    }
    if (neg) asmbuf_putc(b, '-');
    for (i = n - 1; i >= 0; i--) asmbuf_putc(b, tmp[i]);
}

void asmbuf_printf_int(AsmBuffer *b, const char *prefix, int v, const char *suffix) {
    if (prefix) asmbuf_puts(b, prefix);
    asmbuf_int(b, v);
    if (suffix) asmbuf_puts(b, suffix);
}
