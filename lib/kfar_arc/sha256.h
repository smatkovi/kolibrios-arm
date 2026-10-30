/* kfar_arc for KolibriOS for ARM: sha256.c, aes.c (sha256.inc,
 * aes.inc): what 7zaes.c uses */
#ifndef KFAR_ARC_SHA256_H
#define KFAR_ARC_SHA256_H

void sha256_init(void);
void sha256_update(const void *esi, u32 edx);
void sha256_final(u8 *edi);

/* aes.inc's AES data: the number of rounds, the round keys */
typedef struct aes_data {
	u32 nr;
	u8 KeyExpanded[16 * 15];
} aes_data;
/* aes_setkey: in: esi -> key, edx = key size in dwords, edi -> AES data */
void aes_setkey(const u8 *esi, u32 edx, aes_data *edi);
/* aes_decode: in: esi -> in, ebx -> out (16 bytes), edi -> AES data */
void aes_decode(const u8 *esi, u8 *ebx, const aes_data *edi);

#endif
