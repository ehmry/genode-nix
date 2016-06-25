/*
 * \brief  Interface over store inputs and outputs
 * \author Emery Hemingway
 * \date   2015-03-15
 */

/*
 * Copyright (C) 2015-2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _INCLUDE__NIX_STORE_SESSION__NIX_STORE_SESSION_H_
#define _INCLUDE__NIX_STORE_SESSION__NIX_STORE_SESSION_H_

/* Genode includes */
#include <file_system_session/capability.h>
#include <session/session.h>
#include <base/service.h>
#include <root/root.h>
#include <base/rpc_args.h>

/* Nix includes */
#include <nix_store/types.h>

namespace Nix_store {

	struct Session;

	struct Missing_dependency { };

}

struct Nix_store::Session : public Genode::Session
{

	static const char *service_name() { return "Nix_store"; }


	/****************************
	 ** Nix_store interface **
	 ****************************/

	/**
	 * Convience method to dereference input-addressed
	 * paths to output-addressed paths.
	 *
	 * Returns an empty string on failure
	 */
	virtual Name dereference(Name const &name) = 0;

	/**
	 * Realize the ouputs of a derivation file.
	 * Ensuring that all dependencies are present
	 * is the responsibility of the client.
	 *
	 * \throw Invalid_derivation  derivation file was incompatible
	 *                            or failed to parse
	 * \throw Missing_dependency  a derivation dependency is not
	 *                            present in the store
	 */
	virtual void realize(Name const &drv,
	                     Genode::Signal_context_capability sigh) = 0;


	/*********************
	 ** RPC declaration **
	 *********************/

	GENODE_RPC(Rpc_dereference, Name, dereference, Name const&);

	GENODE_RPC_THROW(Rpc_realize, void, realize,
	                 GENODE_TYPE_LIST(Invalid_derivation, Missing_dependency),
	                 Name const&, Genode::Signal_context_capability);

	GENODE_RPC_INTERFACE(Rpc_dereference, Rpc_realize);

};

#endif