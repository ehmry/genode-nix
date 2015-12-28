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

class Builder::Connection : public Genode::Connection<Session>, public Session_client
{
	private:

		Session_capability _create_session(char const *label)
		{
			if (label && *label)
				return session("ram_quota=8K, label=\"%s\"", label);
			else
				return session("ram_quota=8K");
		}

	public:

		Connection(char const *label = 0)
		:
			Genode::Connection<Session>(_create_session(label)),
			Session_client(cap())
		{}
};

#endif