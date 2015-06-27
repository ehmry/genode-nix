/*
 * \brief  Builder session interface
 * \author Emery Hemingway
 * \date   2015-05-27
 */

#ifndef _INCLUDE__BUILDER_SESSION__BUILDER_SESSION_H_
#define _INCLUDE__BUILDER_SESSION__BUILDER_SESSION_H_

#include <session/session.h>
#include <base/signal.h>
#include <base/exception.h>
#include <base/rpc_args.h>


namespace Builder {

	enum { MAX_NAME_LEN = 128 };

	typedef Genode::Rpc_in_buffer<MAX_NAME_LEN> Name;

	/****************
	 ** Exceptions **
	 ****************/

	struct Exception  : Genode::Exception { };
	struct Invalid_derivation : Exception { };
	struct Missing_dependency : Exception { };

	struct Session : public Genode::Session
	{

		/*******************************
		 ** Builder session interface **
		 *******************************/

		static const char *service_name() { return "Builder"; }

		/**
		 * Convience function for clients to test
		 * the validity of store objects.
		 */
		virtual bool valid(Name const &name) = 0;

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
		virtual void realize(Name const  &drv,
		                     Genode::Signal_context_capability sigh) = 0;

		/*********************
		 ** RPC declaration **
		 *********************/

		GENODE_RPC(Rpc_valid, bool, valid, Name const&);
		GENODE_RPC_THROW(Rpc_realize, void, realize,
		                 GENODE_TYPE_LIST(Invalid_derivation,
		                                  Missing_dependency),
		                 Name const&, Genode::Signal_context_capability);
		GENODE_RPC_INTERFACE(Rpc_valid, Rpc_realize);
	};

}

#endif