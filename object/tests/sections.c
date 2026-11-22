int __attribute__((section(".custom"), aligned(32))) custom = 13;
int __attribute__((section(".custom"))) *custom_ptr = &custom;
int state;
static void __attribute__((constructor)) setup(void) { state = *custom_ptr + 7; }
static void __attribute__((destructor)) teardown(void) { state += 3; }
int section_entry(void) { return state + custom; }
