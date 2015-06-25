#include "vectors.h"

#include <hash/sha256.h>
#include <base/printf.h>
#include <util/string.h>

using namespace Genode;

void to_hex(char *out, int out_len, uint8_t *in, int in_len)
{
	char hex[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

	int j = 0;
	int k = 0;
	while (j < in_len) {
		out[k++] = hex[in[j] >> 4];
		out[k++] = hex[in[j++] & 0x0F];
	};
	out[out_len-1] = '\0';
}

int main() {
	Hash::Sha256 sha256;
	uint8_t md[sha256.size()];
	char str[sizeof(md)*2+1];


	for (size_t i = 0; i < sizeof(vectors)/sizeof(vector); ++i) {
		vector &v = vectors[i];

		sha256.update((uint8_t *)v.m, strlen(v.m));
		sha256.digest(md, sizeof(md));

		to_hex(str, sizeof(str), md, sizeof(md));

		if (memcmp(md, v.d, sizeof(md)))
			PERR("%s - %s", str, v.m);
		else
			PINF("%s - %s", str, v.m);
		sha256.reset();
	}

	memset(md, 0, sizeof(md));

	sha256.update(md, sizeof(md));
	sha256.digest(md, sizeof(md));

	to_hex(str, sizeof(str), md, sizeof(md));
	PLOG("%s - 256 zero bits", str);

	memset(md, 0, sizeof(md));
	for (int i = 0; i < 100000; ++i) {
		sha256.reset();
		sha256.update(md, sizeof(md));
		sha256.digest(md, sizeof(md));
	}
	to_hex(str, sizeof(str), md, sizeof(md));
	PLOG("%s - iterated 100000 times", str);

	return 0;
}
