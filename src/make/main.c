#include <string.h>
#include "xh_driver.h"
#include "xh_make.h"
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "make"))
        return xh_make_main(argc - 2, argv + 2);
    return xh_driver_main(argc, argv);
}
