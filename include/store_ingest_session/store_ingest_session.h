/*
 * \brief  Interface between store_ingest and builder
 * \author Emery Hemingway
 * \date   2015-03-15
 */

#ifndef _INCLUDE__INGEST_SESSION__INGEST_SESSION_H_
#define _INCLUDE__INGEST_SESSION__INGEST_SESSION_H_

/* Genode includes */
#include <file_system_session/capability.h>
#include <session/session.h>
#include <base/service.h>
#include <root/root.h>
#include <base/rpc_args.h>

namespace Store_ingest {

	enum { MAX_NAME_LEN = 128 };

	typedef Genode::String<MAX_NAME_LEN> Name;

	struct Session;

	typedef Genode::Root::Session_args Session_args;
}

struct Store_ingest::Session : public Genode::Session
{

	static const char *service_name() { return "Store_ingest"; }


	/****************************
	 ** Store ingest interface **
	 ****************************/

	/**
	 * Declare an expected file system object
	 */
	virtual void expect(Name const &name) = 0;

	/**
	 * Return a capability to a virtualized file system session
	 *
	 * \throw Genode::Service::Invalid_args
	 * \throw Genode::Service::Unavailable
	 * \throw Genode::Service::Quota_exceeded
	 */
	virtual File_system::Session_capability
		file_system_session(Session_args const&) = 0;

	/**
	 * Revoke the file system session
	 */
	virtual void revoke_session() = 0;

	/**
	 * Finalize the content addressable file system object
	 */
	virtual Name ingest(Name const &name) = 0;


	/*********************
	 ** RPC declaration **
	 *********************/

	GENODE_RPC(Rpc_expect, void, expect, Name const&);
	GENODE_RPC_THROW(Rpc_file_system_session,
	                 File_system::Session_capability, file_system_session,
	                 GENODE_TYPE_LIST(Genode::Service::Invalid_args,
                                      Genode::Service::Unavailable,
                                      Genode::Service::Quota_exceeded),
	                 Session_args const&);
	GENODE_RPC(Rpc_revoke_session, void, revoke_session);
	GENODE_RPC(Rpc_ingest, Name, ingest, Name const&);
	GENODE_RPC_INTERFACE(Rpc_expect,
	                     Rpc_file_system_session,
	                     Rpc_revoke_session,
	                     Rpc_ingest);

};

#endif