/* The BL602 SDK starts this from its app task after C++ constructors run. */
extern int lt_main(void);
extern int lt_bl602_application_main(void);

int main(void) {
	lt_main();
	return lt_bl602_application_main();
}
