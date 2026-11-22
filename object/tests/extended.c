extern int external_data;
static int hidden(int x) { return x + 2; }
__attribute__((weak)) int override_me(void) { return 1; }
struct Pair { int n; int *p; } pair = { 7, &external_data };
int *arr[2] = { &external_data, &pair.n };
char *words[2] = { "ab", "cd" };
float soft_add(float a, float b) { return a + b; }
int soft_entry(void) { return (int)soft_add(2.5f, 4.5f); }
int aggregate_entry(void) { return pair.n + *pair.p + *arr[0] + *arr[1] + words[1][0] + hidden(1) + override_me(); }
