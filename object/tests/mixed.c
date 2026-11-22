extern int gnu_add(int, int);
extern int external_data;
static int local = 9;
int value = 17;
int zero;
int *ptr = &external_data;
char *message = "ELF";
int callback(int x) { return x + local; }
int (*fp)(int) = callback;
int xh_entry(void) { return gnu_add(value, external_data) + *ptr + fp(3) + message[1] + zero; }
