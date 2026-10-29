/* lwIP's <stdlib.h>: atoi only (netif_find) */
static inline int atoi(const char *s)
{
	int v = 0;
	while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0';
	return v;
}
