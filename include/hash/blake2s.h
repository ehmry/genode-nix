/*
 * \brief  BLAKE2s hash function
 * \author Emery Hemingway
 * \date   2015-04-20
 */

#ifndef _HASH__BLAKE2S_H_
#define _HASH__BLAKE2S_H_

#include <hash/hash.h>
#include <base/stdint.h>

#if defined(_MSC_VER)
#define ALIGN(x) __declspec(align(x))
#else
#define ALIGN(x) __attribute__((aligned(x)))
#endif

  ALIGN( 64 ) typedef struct __blake2s_state
  {
    Genode::uint32_t h[8];
    Genode::uint32_t t[2];
    Genode::uint32_t f[2];
    Genode::uint8_t  buf[2 * 64];
    Genode::size_t   buflen;
    Genode::uint8_t  last_node;
  } blake2s_state ;

namespace Hash {

	class Blake2s : public Hash::Function
	{
		private:

			enum {
				BLAKE2S_BLOCKBYTES = 64,
				BLAKE2S_OUTBYTES   = 32,
				BLAKE2S_KEYBYTES   = 32,
				BLAKE2S_SALTBYTES  = 8,
				BLAKE2S_PERSONALBYTES = 8
			};

			blake2s_state S;

		public:

			Blake2s();

			size_t size() { return BLAKE2S_OUTBYTES; }
			size_t block_size() { return BLAKE2S_BLOCKBYTES; };

			void update(uint8_t const *in, size_t inlen);
			void digest(uint8_t *out, size_t outlen);

			virtual void reset();
	};
};

#endif