/*
 * \brief  SHA256 hash function
 * \author Emery Hemingway
 * \date   2015-04-21
 */

#ifndef _HASH__SHA256_H_
#define _HASH__SHA256_H_

#include <hash/hash.h>
#include <base/stdint.h>

namespace Hash {

	using namespace Genode;

	class Sha256 : public Hash::Function
	{
		private:

			enum {
				SHA256_DIGEST_LENGTH = 32,
				SHA_LBLOCK = 16,

				/*
				 * SHA treats input data as a
				 * contiguous array of 32 bit wide
				 * big-endian values.
				 */
				SHA_CBLOCK = (SHA_LBLOCK*4)
			};

			uint32_t _h[8];
			uint32_t _Nl, _Nh;
			uint32_t _data[SHA_LBLOCK];
			unsigned int _num, _md_len;

			void hash_make_string(uint8_t*);
			void block_data_order(const uint8_t*, size_t);

		public:

			Sha256();

			size_t size() { return SHA256_DIGEST_LENGTH; }
			size_t block_size() { return SHA_CBLOCK; }

			void update(uint8_t const *buf, size_t len);
			void digest(uint8_t *buf, size_t len);
			void reset();
	};

};

#endif