/* Standalone shell. Shared applications reuse xh_linker_main without stubs. */
#include "xh_linker_cli.h"
#include "platform.h"
#include "diag.h"
#include <stdarg.h>
#include <stdio.h>
void ark_log(const char *format,...) {va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);}
void diag_tool(DiagSeverity severity,const char *where,const char *message,const char *a,const char *b,const char *c,const char *d,const char *e,const char *f) {
 (void)severity;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;fprintf(stderr,"%s: %s\n",where,message);
}
int main(int argc,char **argv) {return xh_linker_main(argc,argv);}
