/* tests/wifi's crypto test (crypto.c) as a program of its own */
#include <stdio.h>
void crypto_tests(void);
extern int crypto_failed, crypto_passed;
int main(void)
{
	crypto_tests();
	printf("%d passed, %d failed\n", crypto_passed, crypto_failed);
	return crypto_failed != 0;
}
