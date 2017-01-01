/*
 * \brief  ROM verification server
 * \author Emery Hemingway
 * \date   2016-12-30
 */

/*
 * Copyright (C) 2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

/* Crypto++ includes */
#include <sha.h>

/* Genode includes */
#include <os/session_policy.h>
#include <rom_session/connection.h>
#include <base/attached_rom_dataspace.h>
#include <base/heap.h>
#include <base/service.h>
#include <base/session_label.h>
#include <base/component.h>
#include <base/log.h>

static char const alph[0x10] = {
	'0','1','2','3','4','5','6','7',
	'8','9','a','b','c','d','e','f'
};

namespace Rom_hash {
	using namespace Genode;

	struct Session;
	struct Main;

	typedef Session_state::Args Args;
}


struct Rom_hash::Session :
	Genode::Parent::Server,
	Genode::Connection<Rom_session>
{
	Parent::Client parent_client;

	Id_space<Parent::Client>::Element client_id;
	Id_space<Parent::Server>::Element server_id;

	Session(Id_space<Parent::Client> &client_space,
	        Id_space<Parent::Server> &server_space,
	        Parent::Server::Id server_id,
	        Genode::Env &env, Args const &args,
	        Session_policy const &policy);
};

Rom_hash::Session::Session(Id_space<Parent::Client> &client_space,
                           Id_space<Parent::Server> &server_space,
                           Parent::Server::Id server_id,
                           Genode::Env &env, Args const &args,
                           Session_policy const &policy)
:
	Connection<Rom_session>(env, session(env.parent(), args.string())),
	client_id(parent_client, client_space),
	server_id(*this, server_space, server_id)
{
	/************
	 ** verify **
	 ************/

	CryptoPP::SHA256 hash;
	unsigned const digest_size = hash.DigestSize();
	unsigned const encoded_size = digest_size*2+1;

	char text[encoded_size];
	uint8_t digest[digest_size];

	try {
		policy.attribute("sha256").value(text, encoded_size);
	} catch (...) {
		error("no 'sha256' digest found on ",policy);
		throw;
	}

	/* read the connection dataspace */
	Rom_session_client rom(cap());
	Attached_dataspace ds(env.rm(), rom.dataspace());

	hash.CalculateDigest(digest, ds.local_addr<const byte>(), ds.size());

	/* compare with hexadecimal */
	for (unsigned i = 0, j = 0; i < digest_size; ++i)
		if ((alph[digest[i] >> 4] != text[j++]) ||
		    (alph[digest[i]&0x0F] != text[j++]))
		{
			error("verification failed for '",label_from_args(args.string()),"'");
			throw ~0;
		}
}


struct Rom_hash::Main
{
	Id_space<Parent::Server> server_id_space;

	Genode::Env &env;

	Attached_rom_dataspace config_rom { env, "config" };

	Attached_rom_dataspace session_requests { env, "session_requests" };

	Sliced_heap alloc { env.ram(), env.rm() };

	bool config_stale = false;

	void handle_config() {
		config_stale = true; }

	void handle_session_request(Xml_node request);

	void handle_session_requests()
	{
		if (config_stale) {
			config_rom.update();
			config_stale = false;
		}

		session_requests.update();

		Xml_node const requests = session_requests.xml();

		requests.for_each_sub_node([&] (Xml_node request) {
			handle_session_request(request);
		});
	}

	Signal_handler<Main> config_handler {
		env.ep(), *this, &Main::handle_config };

	Signal_handler<Main> session_request_handler {
		env.ep(), *this, &Main::handle_session_requests };

	Main(Genode::Env &env) : env(env)
	{
		config_rom.sigh(config_handler);
		session_requests.sigh(session_request_handler);
	}
};


void Rom_hash::Main::handle_session_request(Xml_node request)
{
	if (!request.has_attribute("id"))
		return;

	Id_space<Parent::Server>::Id const server_id {
		request.attribute_value("id", 0UL) };

	if (request.has_type("create")) {
		if (!request.has_sub_node("args"))
			return;

		typedef Session_state::Args Args;
		Args const args = request.sub_node("args").decoded_content<Args>();

		/* fetch and serve it again */
		try {
			Session_label const label = label_from_args(args.string());
			Session_policy const policy(label, config_rom.xml());

			Session *session = new (alloc)
				Session(env.id_space(), server_id_space, server_id, env, args, policy);
			if (session)
				env.parent().deliver_session_cap(server_id, session->cap());
		} catch (...) {
			env.parent().session_response(server_id, Parent::INVALID_ARGS);
		}
	}

	if (request.has_type("upgrade")) {
		server_id_space.apply<Session>(server_id, [&] (Session &session) {
			size_t ram_quota = request.attribute_value("ram_quota", 0UL);

			char buf[64];
			Genode::snprintf(buf, sizeof(buf), "ram_quota=%ld", ram_quota);

			// XXX handle Root::Invalid_args
			env.upgrade(session.client_id.id(), buf);
			env.parent().session_response(server_id, Parent::SESSION_OK);
		});
	}

	if (request.has_type("close")) {
		server_id_space.apply<Session>(server_id, [&] (Session &session) {
			env.close(session.client_id.id());
			destroy(alloc, &session);
			env.parent().session_response(server_id, Parent::SESSION_CLOSED);
		});
	}

}

/***************
 ** Component **
 ***************/

void Component::construct(Genode::Env &env)
{
	static Rom_hash::Main inst(env);
	env.parent().announce("ROM");
}
