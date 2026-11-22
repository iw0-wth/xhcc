/* Bounded, in-process C99 command driver. */
#ifndef XH_DRIVER_H
#define XH_DRIVER_H

#define XH_DRIVER_MAX_ARGS 512
#define XH_DRIVER_ARG_BYTES (32 * 1024)
#define XH_DRIVER_RESPONSE_BYTES (64 * 1024)
#define XH_DRIVER_RESPONSE_DEPTH 8
#define XH_DRIVER_PATH_BYTES 512
#define XH_DRIVER_MAX_SOURCES 64

/* argv[0] is the executable. Optional argv[1]: arm-none-eabi-gcc or armcc.
 * Response paths, including nested @files, are relative to the process cwd.
 * No shell, subprocess, heap allocation, environment expansion or linking.
 * Fixed storage makes this entry point non-reentrant. */
int xh_driver_main(int argc, char **argv);

#endif
