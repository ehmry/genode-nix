/*
 * \brief  Interface to hash functions
 * \author Emery Hemingway
 * \date   2015-04-20
 */

#ifndef _HASH__HASH_H_
#define _HASH__HASH_H_

#include <base/stdint.h>

namespace Hash {
	using namespace Genode;
	struct Function;
}

struct Hash::Function
{
	virtual ~Function() { }

	/**
	 * The number of bytes returned by the digest method.
	 */
	virtual size_t size() = 0;

	/**
	 * Ideal block size of inputs to this hash function.
	 */
	virtual size_t block_size() = 0;

	/**
	 * Append data to hash message.
	 */
	virtual void update(uint8_t const *buf, size_t len) = 0;

	/**
	 * Calculate message digest.
	 * May be interleaved with calls to update.
	 */
	virtual void digest(uint8_t *buf, size_t len) = 0;

	/**
	 * Reset the internal state of the hash function.
	 */
	virtual void reset() = 0;
};

#endif