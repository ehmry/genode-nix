/*
 * \brief  Connection to Nix_store service
 * \author Emery Hemingway
 * \date   2015-05-27
 */

/*
 * Copyright (C) 2015-2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _INCLUDE__NIX_STORE__CONNECTION_H_
#define _INCLUDE__NIX_STORE__CONNECTION_H_

#include <nix_store_session/nix_store_session.h>
#include <base/connection.h>

namespace Nix_store { struct Connection; }

struct Nix_store::Connection : public Genode::Connection<Session>, public Genode::Rpc_client<Session>
{
	Connection(Genode::Env &env, char const *label = "")
	:
		Genode::Connection<Session>(
			env, session(env.parent(), "ram_quota=8K, label=\"%s\"", label)),
		Genode::Rpc_client<Session>(cap())
	{}

	Name dereference(Name const &name) { return call<Rpc_dereference>(name); }

	void realize(Name const  &drv, Genode::Signal_context_capability sigh) {
		call<Rpc_realize>(drv, sigh); }
};

#endif