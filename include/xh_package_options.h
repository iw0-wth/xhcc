#ifndef XH_PACKAGE_OPTIONS_H
#define XH_PACKAGE_OPTIONS_H
/* Bounded, one-pass argv rewrite. Rules never recursively expand other rules.
 * Each section requires match, mode=flag|value|joined, emit. emit is one
 * canonical option token; flag may emit whitespace-separated tokens. Values
 * retain exact bytes and are never shell-expanded. -- stops rewriting.
 * Storage remains valid until next load/map; API is non-reentrant. */
/* True for canonical separate-value options and loaded value aliases. */
int xh_package_options_takes_value(const char *arg);
int xh_package_options_load(const char *path);
int xh_package_options_map(int argc, char **argv, int *out_argc, char ***out_argv);
#endif
