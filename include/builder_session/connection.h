/*
 * \brief  Connection to Builder service
 * \author Emery Hemingway
 * \date   2015-05-27
 */

#ifndef _INCLUDE__BUILDER_SESSION__CONNECTION_H_
#define _INCLUDE__BUILDER_SESSION__CONNECTION_H_

#include <builder_session/client.h>
#include <base/connection.h>


namespace Builder { struct Connection; }

struct Builder::Connection : Genode::Connection<Session>, Session_client
{
	Connection()
	:
		Genode::Connection<Session>(session("ram_quota=512K")),
		Session_client(cap())
	{}
};

#endif