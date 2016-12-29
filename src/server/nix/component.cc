/*
 * \brief  Serve Nix expressions
 * \author Emery Hemingway
 * \date   2015-10-13
 *
 * This component can barely be called a server.
 * Incoming session requestes are rewritten with
 * the result of a Nix evaulation and forwarded
 * by the parent to a ROM or File_system service.
 */

/*
 * Copyright (C) 2015-2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

/* Nix includes */
#include <attr-path.hh>
#include <shared.hh>
#include <common-opts.hh>
#include <eval.hh>
#include <get-drvs.hh>
#include <store.hh>
#include <util.hh>

/* Genode includes */
#include <vfs/file_system_factory.h>
#include <vfs/dir_file_system.h>
#include <os/session_policy.h>
#include <base/service.h>
#include <base/rpc_server.h>
#include <root/root.h>
#include <base/attached_rom_dataspace.h>
#include <util/reconstructible.h>
#include <base/heap.h>
#include <base/component.h>
#include <base/log.h>

namespace Nix {
	using namespace Genode;

	struct Main;

	typedef Genode::String<64> Service_name;
}


struct Nix::Main
{
	struct Session : Parent::Server
	{
		Parent::Client parent_client;

		Id_space<Parent::Client>::Element client_id;
		Id_space<Parent::Server>::Element server_id;

		Session(Id_space<Parent::Client> &client_space,
		        Id_space<Parent::Server> &server_space,
		        Parent::Server::Id server_id)
		:
			client_id(parent_client, client_space),
			server_id(*this, server_space, server_id) { }
	};

	Id_space<Parent::Server> server_id_space;

	Genode::Env &env;

	Attached_rom_dataspace config_rom       { env, "config" };
	Attached_rom_dataspace session_requests { env, "session_requests" };

	Heap heap { env.ram(), env.rm() };

	/*
	 * TODO: Move the VFS to Internal_state, need to
	 * make sure that the VFS is destructable first.
	 */
	Vfs::Dir_file_system vfs
		{ env, heap,
		  config_rom.xml().sub_node("vfs"),
		  Vfs::global_file_system_factory()
		};

	struct Internal_state
	{
		nix::Store      store;

		nix::EvalState  eval_state;

		Internal_state(Genode::Env &env,
		               Genode::Allocator &allocator,
		               Genode::Xml_node config)
		:
			store(env, allocator),
			eval_state(env, store, config.sub_node("nix"))
		{ }
	};

	Genode::Constructible<Internal_state>
		state { env, heap, config_rom.xml() };

	Internal_state &alloc_state()
	{
		if (!state.constructed()) nix::handleExceptions("nix server", [&] {
			state.construct(env, heap, config_rom.xml()); });

		if (!state.constructed())
			throw Root::Unavailable();

		return *state;
	}

	void free() { state.destruct(); }

	nix::Path realise(Genode::Xml_node const policy,
	                  Genode::Service::Name const &service,
	                  Genode::Session_label const &label,
	                  Session_state::Args const &session_args);

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
			handle_session_request(request); });
	}

	/**
	 * This lazy kind of work can get expensive
	 * so destroy the state when the parent asks
	 * for resources.
	 */
	void yield()
	{
		Genode::size_t const before = env.ram().avail();
		free();
		Genode::size_t const after = env.ram().avail();
		env.parent().yield_response();
		Genode::log("yielded ", (before - after) >> 10, "KB");
	}

	Signal_handler<Main> config_handler {
		env.ep(), *this, &Main::handle_config };

	Signal_handler<Main> session_request_handler {
		env.ep(), *this, &Main::handle_session_requests };

	Signal_handler<Main> yield_handler
		{ env.ep(), *this, &Main::yield };

	Main(Genode::Env &env) : env(env)
	{
		/* initialize the Nix libraries */
		nix::handleExceptions("nix server", [&] { nix::initNix(vfs); });

		config_rom.sigh(config_handler);
		session_requests.sigh(session_request_handler);
		env.parent().yield_sigh(yield_handler);

		try {
			Xml_node announce = config_rom.xml().sub_node("announce");
			announce.for_each_sub_node("service", [&] (Xml_node &node) {
				env.parent().announce(node.attribute_value(
					"name", Genode::Service::Name()).string()); });
		} catch (...) {
			error("failed to parse and announce services");
			throw;
		}
	}
};


/**
 * Apply an argument to a function and realise the nix output
 */
nix::Path Nix::Main::realise(Genode::Xml_node const policy,
                             Genode::Service::Name const &service,
 	                         Genode::Session_label const &label,
                             Session_state::Args const &session_args)
{
	using namespace nix;

	nix::string out;

	enum { TMP_BUF_LEN = 256 };
	char tmp_buf[TMP_BUF_LEN] = { '\0' };

	Value root_value;
	Expr *e;

	Internal_state &state = alloc_state();


	/****************
	 ** Parse file **
	 ****************/

	/* XXX: use the string methods from Xml_node */
	try {
		policy.attribute("file").value(tmp_buf, sizeof(tmp_buf));
		e = state.eval_state.parseExprFromFile(nix::Path(tmp_buf));

	} catch (Xml_node::Nonexistent_attribute) {
		e = state.eval_state.parseExprFromFile("/default.nix");
	}
	state.eval_state.eval(e, root_value);

	/***********************
	 ** Session arguments **
	 ***********************/
	std::map<string, string> arg_map;

	{
		string label_str;
		label_str += "#"; /* stripped away, i don't know why */
		label_str += label.string();
		arg_map["label"] = label_str;

		/*
		 * XXX: All session arguments should be passed
		 * but Arg_string does not support iteration.
		 */

		enum { ARG_MAX_LEN = 128 };
		char arg[ARG_MAX_LEN];

		Arg_string::find_arg(session_args.string(), "root").string(
			arg, sizeof(arg), "");
		if (arg[0])
			arg_map["root"] = arg;
	}

	Bindings &args(*evalAutoArgs(state.eval_state, arg_map));


	/********************
	 ** Find attribute **
	 ********************/

	Value *entry;
	try {
		policy.attribute("attr").value(tmp_buf, sizeof(tmp_buf));
		entry = findAlongAttrPath(
			state.eval_state, tmp_buf, args, root_value);
	} catch (Xml_node::Nonexistent_attribute) {
		entry = findAlongAttrPath(
			state.eval_state, "", args, root_value);
	}


	/*************
	 ** realise **
	 *************/

	PathSet context;
	Value service_arg, func, result;

	mkString(service_arg, service.string());

	state.eval_state.callFunction(*entry, service_arg, func, noPos);
	state.eval_state.autoCallFunction(args, func, result);

	DrvInfo drv_info(state.eval_state);
	if (getDerivation(state.eval_state, result, drv_info, false)) {
		PathSet drv_set{ drv_info.queryDrvPath() };
		state.store.buildPaths(drv_set, nix::bmNormal);

		out = drv_info.queryOutPath();
	} else
		out = state.eval_state.coerceToString(noPos, result, context);

	while (out.front() == '/') out.erase(0,1);

	/* XXX: and if out is not a top level store element? */
	Nix_store::Name out_name =
		state.eval_state.store().store_session().dereference(out.c_str());
	return out_name.string();
}


void Nix::Main::handle_session_request(Genode::Xml_node request)
{
	using namespace Genode;

	if (!request.has_attribute("id"))
		return;

	Parent::Server::Id const server_id { request.attribute_value("id", 0UL) };

	if (request.has_type("create")) {

		if (!request.has_sub_node("args"))
			return;

		Genode::Service::Name const service =
			request.attribute_value("service", Service::Name());

		Session_state::Args const args =
			request.sub_node("args").decoded_content<Session_state::Args>();

		Session_label const label = label_from_args(args.string());
		nix::Path out;

		try {
			try {
				Session_policy policy(label, config_rom.xml());
				nix::handleExceptions("nix", [&] {
					out = realise(policy, service, label, args);
				});
			} catch (Session_policy::No_policy_defined) {
				nix::handleExceptions("nix", [&] {
					out = realise(Xml_node(
						"<default-policy/>"), service, label, args);
				});
			}
		} catch (...) {
			Genode::error("caught unhandled exception while evaluating '",service,":",label,"'");
			env.parent().session_response(server_id, Parent::INVALID_ARGS);
			return;
		}

		if (out == "") {
			Genode::error("no evaluation for '",service,":",label,"'");
			env.parent().session_response(server_id, Parent::INVALID_ARGS);
			return;
		}

		if (out.length() >= Vfs::MAX_PATH_LEN) {
			Genode::error("'",service,":",label,"' did not resolve to a store object");
			env.parent().session_response(server_id, Parent::INVALID_ARGS);
			return;
		}

		enum { ARGS_MAX_LEN = 256 };
		char new_args[ARGS_MAX_LEN];

		strncpy(new_args, args.string(), ARGS_MAX_LEN);

		// XXX: slash hack
		while (out.front() == '/')
			out.erase(0,1);

		Session_label const new_label = prefixed_label(
			Session_label("store"),
			Session_label(out.c_str()));

		Arg_string::set_arg_string(
			new_args, ARGS_MAX_LEN, "label", new_label.string());

		/* allocate session meta-data */
		Session *session = nullptr;
		try {
			session = new (heap)
				Session(env.id_space(), server_id_space, server_id);

			Affinity aff;
			Session_capability cap =
				env.session(service.string(), session->client_id.id(),
				            new_args, aff);

			env.parent().deliver_session_cap(server_id, cap);
			return;
		}

		catch (Parent::Service_denied) {
			warning("'", new_label, "' was denied"); }

		catch (Service::Unavailable) {
			warning("'", new_label, "' is unavailable"); }

		catch (Service::Invalid_args)   {
			warning("'", new_label, "' received invalid args"); }

		catch (Service::Quota_exceeded) {
			warning("'", new_label, "' quota donation was insufficient"); }

		if (session)
				destroy(heap, session);

		env.parent().session_response(server_id, Parent::INVALID_ARGS);
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
			destroy(heap, &session);
			env.parent().session_response(server_id, Parent::SESSION_CLOSED);
		});
	}

};



/***************
 ** Component **
 ***************/


/*
 * XXX: Nix uses this stack for the evaluation,
 * so the threat of a blown stack depends on
 * the complexity of the evaulation.
 */
Genode::size_t Component::stack_size() { return 32*1024*sizeof(long); }

void Component::construct(Genode::Env &env) {
	static Nix::Main inst(env); }
