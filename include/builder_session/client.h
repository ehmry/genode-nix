/*
 * \brief  Client-side Builder session interface
 * \author Emery Hemingway
 * \date   2015-05-27
 */

#ifndef _INCLUDE__BUILDER_SESSION__CLIENT_H_
#define _INCLUDE__BUILDER_SESSION__CLIENT_H_

#include <builder_session/capability.h>
#include <packet_stream_tx/client.h>
#include <base/rpc_client.h>

namespace Builder { struct Session_client; }

struct Builder::Session_client : Genode::Rpc_client<Session>
{
	explicit Session_client(Session_capability session)
	: Genode::Rpc_client<Session>(session) { }

	void realize(Name const  &drv, Genode::Signal_context_capability sigh) {
		call<Rpc_realize>(drv, sigh); }
};

#endif