/*
 * \brief  Digest encoding
 * \author Emery Hemingway
 * \date   2015-06-02
 */

#ifndef _STORE_HASH__ENCODE_H_
#define _STORE_HASH__ENCODE_H_

#include <base/fixed_stdint.h>
#include <base/stdint.h>
#include <base/exception.h>
#include <util/string.h>

namespace Store_hash {

	enum { HASH_PREFIX_LEN = 32 };

	using namespace Genode;

	static uint8_t const base16[] = {
		'0','1','2','3','4','5','6','7',
		'8','9','a','b','c','d','e','f'
	};

	static uint8_t const base32[] = {
		'0','1','2','3','4','5','6','7',
		'8','9','a','b','c','d','f','g',
		'h','i','j','k','l','m','n','p',
		'q','r','s','v','w','x','y','z'
	};

	/* Get the base32 encoding of the first 160 bits of the digest. */
	void encode(uint8_t *buf, char const *name, size_t len)
	{
		if (len < HASH_PREFIX_LEN+2) {
			*buf = 0;
			return;
		}
		int i = 20;
		int j = 32;
		do {
			uint8_t b7, b6, b5 , b4, b3, b2, b1, b0;

			b7  =  buf[--i]       & 0x1F;
			b6  =  buf[i]   >> 5;
			b6 |= (buf[--i] << 3) & 0x1F;
			b5  = (buf[i]   >> 2) & 0x1F;
			b4  =  buf[i]   >> 7;
			b4 |= (buf[--i] << 1) & 0x1F;
			b3  = (buf[i]   >> 4) & 0x1F;
			b3 |= (buf[--i] << 4) & 0x1F;
			b2  = (buf[i]   >> 1) & 0x1F;
			b1  = (buf[i]   >> 6) & 0x1F;
			b1 |= (buf[--i] >> 2) & 0x1F;
			b0  = (buf[i]   >> 3);

			buf[--j]   = base32[b7];
			buf[--j] = base32[b6];
			buf[--j] = base32[b5];
			buf[--j] = base32[b4];
			buf[--j] = base32[b3];
			buf[--j] = base32[b2];
			buf[--j] = base32[b1];
			buf[--j] = base32[b0];
		} while (i);
		buf[HASH_PREFIX_LEN] = '-';
		strncpy((char *)buf+(HASH_PREFIX_LEN+1), name, len-(HASH_PREFIX_LEN+1));
	}

}

#endif