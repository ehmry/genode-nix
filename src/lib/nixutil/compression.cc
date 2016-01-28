#include "compression.hh"
#include "types.hh"

#include <cstdio>

namespace nix {

std::string decompressXZ(const std::string & in)
{
    throw Error(format("%1% not implemented, still need to port liblzma") % __func__);
}

}
