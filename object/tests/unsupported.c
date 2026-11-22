__attribute__((section(".custom"))) int custom = 3;
int read_custom(void) { return custom; }
