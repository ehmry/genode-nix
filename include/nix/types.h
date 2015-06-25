/*
 * \brief  Common type declarations for Nix
 * \author Emery Hemingway
 * \date   2015-05-10
 */

#ifndef _NIX__TYPES_H_
#define _NIX__TYPES_H_

#include <util/string.h>
#include <base/stdint.h>

namespace Nix {

	enum { MAX_NAME_LEN = 128 };

	enum Path_type { TEXT, SOURCE, OUTPUT };

	enum Hash_type { SHA256, BLAKE2S };

	typedef Genode::String<MAX_NAME_LEN> Name;
};

#endif /* _NICHTS__TYPES_H_ */
