#include <hash/blake2s.h>
#include <base/printf.h>
#include <util/string.h>

#include "blake2-kat.h"

using namespace Genode;
using namespace Hash;

int main() {
	uint8_t buf[KAT_LENGTH];

	for( size_t i = 0; i < KAT_LENGTH; ++i )
		buf[i] = ( uint8_t )i;

	Hash::Blake2s blake2s;

	for( size_t i = 0; i < KAT_LENGTH; ++i ) {
		uint8_t hash[BLAKE2S_OUTBYTES];

		blake2s.update(buf, i);
		blake2s.digest(hash, sizeof(hash));

		if( 0 != memcmp( hash, blake2s_kat[i], BLAKE2S_OUTBYTES ) ) {
			PERR( "error at #%ld", i);
			return -1;
		}
		blake2s.reset();
	}

	PINF( "ok" );
	return 0;
}