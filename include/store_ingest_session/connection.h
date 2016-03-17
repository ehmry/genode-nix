/*
 * \brief  Client-side ingest session interface
 * \author Emery Hemingway
 * \date   2016-03-16
 */

/*
 * Copyright (C) 2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _INCLUDE__STORE_INGEST_SESSION__FS_CONNECTION_H_
#define _INCLUDE__STORE_INGEST_SESSION__FS_CONNECTION_H_

#include <file_system_session/connection.h>
#include <store_ingest_session/store_ingest_session.h>
#include <base/connection.h>
#include <base/rpc_client.h>

namespace Store_ingest {
	struct Session_client;
	struct Connection;
}


struct Store_ingest::Session_client : Genode::Rpc_client<Session>
{
	Session_client(Genode::Capability<Session> cap)
	: Genode::Rpc_client<Session>(cap) { }

	/****************************
	 ** Store ingest interface **
	 ****************************/

	void expect(Name const &name) override {
		call<Rpc_expect>(name); }

	File_system::Session_capability
	file_system_session(Session_args const &args) override {
		return call<Rpc_file_system_session>(args); }

	void revoke_session() override {
		call<Rpc_revoke_session>(); }

	Name ingest(Name const &name) override {
		return call<Rpc_ingest>(name); }
};


struct Store_ingest::Connection :
	Genode::Connection<Session>, Session_client
{
	/* there are two tx buffers, frontend and backend */
	enum { DEFAULT_TX_BUF_SIZE = File_system::DEFAULT_TX_BUF_SIZE*2 };

	Connection(Genode::size_t tx_buf_size = DEFAULT_TX_BUF_SIZE)
	:
		Genode::Connection<Session>(
			session("ram_quota=%zd, tx_buf_size=%zd",
			        8*4096 + tx_buf_size, tx_buf_size)),
		Session_client(cap())
	{ }
};

#endif
