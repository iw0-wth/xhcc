int external_data = 5;
extern int callback(int);
int gnu_add(int a, int b) { return a + b; }
int gnu_entry(void) { return callback(4); }
int override_me(void) { return 20; }
