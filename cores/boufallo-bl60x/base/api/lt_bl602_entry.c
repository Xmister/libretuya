/* The BL602 SDK calls the application entry from its app task after C++
 * constructors and references it as main(). Other families enter LibreTiny
 * through their own startup functions instead. */
extern int lt_main(void);
extern int lt_bl602_application_main(void);

int main(void) {
	lt_main();
	return lt_bl602_application_main();
}
