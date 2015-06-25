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


namespace Builder {

	enum { MAX_NAME_LEN = 128 };

	/*
	 * TODO: downgrade this to a rpc-in arg,
	 * I don't see why the builder would return
	 * the name of the job.
	 */
	typedef Genode::String<MAX_NAME_LEN> Name;

	/****************
	 ** Exceptions **
	 ****************/

	struct Exception  : Genode::Exception { };
	struct Invalid_derivation : Exception { };

	struct Session : public Genode::Session
	{

		/*******************************
		 ** Builder session interface **
		 *******************************/

		static const char *service_name() { return "Builder"; }

		/**
		 * Realize the ouputs of a derivation file
		 *
		 * \throw Invalid_derivation  derivation file was incompatible
		 *                            or failed to parse
		 */
		virtual void realize(Name const  &drv,
		                     Genode::Signal_context_capability sigh) = 0;

		/*********************
		 ** RPC declaration **
		 *********************/

		GENODE_RPC_THROW(Rpc_realize, void, realize,
		                 GENODE_TYPE_LIST(Invalid_derivation),
		                 Name const&, Genode::Signal_context_capability);
		GENODE_RPC_INTERFACE(Rpc_realize);
	};

}

#endif