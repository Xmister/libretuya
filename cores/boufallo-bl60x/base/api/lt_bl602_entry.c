/* The BL602 SDK starts this from its app task after C++ constructors run. */
extern int lt_main(void);
int main(void) { return lt_main(); }
