#ifndef XH_PACKAGE_H
#define XH_PACKAGE_H
#include "compiler.h"
int xh_package_apply(const char *root, const char *tool, ArkOptions *opts);
int xh_package_list(const char *root);
int xh_write_dependencies(const ArkOptions *opts);
#endif
