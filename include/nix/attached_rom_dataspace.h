/*
 * \brief  Convenience utility to open a ROM session to Nix store
 * \author Emery Hemingway
 * \date   2016-05-29
 */

/*
 * Copyright (C) 2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _INCLUDE__NIX__ATTACHED_ROM_DATASPACE_H_
#define _INCLUDE__NIX__ATTACHED_ROM_DATASPACE_H_

#include <base/attached_rom_dataspace.h>
#include <rom_session/connection.h>
#include <util/label.h>

namespace Nix {
	struct Rom_connection;
	struct Attached_rom_dataspace;
}

struct Nix::Rom_connection : public Genode::Rom_connection
{
	Rom_connection(Genode::Env &env, char const *name)
	: Genode::Rom_connection(env, Genode::Label(name, "store").string()) {}
};


struct Nix::Attached_rom_dataspace : Genode::Attached_rom_dataspace
{
	Attached_rom_dataspace(Genode::Env &env, char const *name)
	: Genode::Attached_rom_dataspace(env, Genode::Label(name, "store").string()) {}
};

#endif /* _INCLUDE__NIX__ATTACHED_ROM_DATASPACE_H_ */
