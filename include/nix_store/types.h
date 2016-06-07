#ifndef _INCLUDE__NIX_STORE__NAME_H_
#define _INCLUDE__NIX_STORE__NAME_H_

/* Genode includes */
#include <os/path.h>
#include <util/string.h>

namespace Nix_store {

	enum { MAX_NAME_LEN = 256 };

	typedef Genode::String<MAX_NAME_LEN> Name;

	typedef Genode::Path<MAX_NAME_LEN+1> Path;

	struct Invalid_derivation { };

}

#endif