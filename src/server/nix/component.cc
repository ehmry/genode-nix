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
#include <base/heap.h>
#include <base/component.h>
#include <base/log.h>

namespace Nix {
	using namespace Genode;

	struct State;

	template <typename SESSION_TYPE> class Service_proxy;

	class Rom_root;
	class File_system_root;
}


/**
 * Allocates and destroys state
 */
struct Nix::State
{
	Genode::Env &env;

	Genode::Attached_rom_dataspace config_rom { env, "config" };

	Genode::Heap heap { env.ram(), env.rm() };

	/*
	 * TODO: Move the VFS to Internal_state, need to
	 * make sure that the VFS is destructable first.
	 */
	Vfs::Dir_file_system vfs
		{ config_rom.xml().sub_node("vfs"),
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

	Genode::Lazy_volatile_object<Internal_state> _state;

	Internal_state &alloc()
	{
		if (!_state.constructed()) nix::handleExceptions("nix server", [&] {
			_state.construct(env, heap, config_rom.xml());
		});

		if (!_state.constructed())
			throw Root::Unavailable();

		return *_state;
	}

	void free() { _state.destruct(); }

	void update_config()
	{
		Genode::log("reloading config and evaluator state");
		free(); config_rom.update();
	}

	Genode::Signal_handler<State> config_handler
		{ env.ep(), *this, &State::update_config };

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

	Genode::Signal_handler<State> yield_handler
		{ env.ep(), *this, &State::yield };

	State(Genode::Env &env) : env(env)
	{
		/* initialize the Nix libraries */
		nix::handleExceptions("nix server", [&] { nix::initNix(vfs); });

		/* register signal handlers */
		config_rom.sigh(config_handler);
		env.parent().yield_sigh(yield_handler);
	}
};


template <typename SESSION_TYPE>
class Nix::Service_proxy : public Genode::Rpc_object<Typed_root<SESSION_TYPE>>
{
	protected:

		virtual void read_nix_arg(char *arg, size_t arg_len,
		                          Root::Session_args const &args) = 0;
		virtual void rewrite_args(char *args, size_t args_len, nix::Path &out) = 0;


	private:

		Parent_service  _backend;
		State          &_state;

		nix::Path _realise(Genode::Xml_node policy, char const *nix_arg)
		{
			using namespace nix;

			nix::string out;

			enum { TMP_BUF_LEN = 256 };
			char tmp_buf[TMP_BUF_LEN] = { '\0' };

			Value root_value;
			Expr *e;

			State::Internal_state &state = _state.alloc();


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
			 ** Collect arguments **
			 ***********************/
			std::map<string, string> arg_map;

			policy.for_each_sub_node("arg", [&arg_map] (Xml_node arg_node) {
				char  name[64] = { '\0' };
				char value[64] = { '\0' };

				arg_node.attribute("name").value(name, sizeof(name));
				arg_node.attribute("value").value(value, sizeof(value));
				arg_map[name] = value;
			});

			Bindings &args(*evalAutoArgs(state.eval_state, arg_map));


			/********************
			 ** Find attribute **
			 ********************/

			Value *v;
			try {
				policy.attribute("attr").value(tmp_buf, sizeof(tmp_buf));
				v = findAlongAttrPath(
					state.eval_state, tmp_buf, args, root_value);
			} catch (Xml_node::Nonexistent_attribute) {
				v = findAlongAttrPath(
					state.eval_state, "", args, root_value);
			}


			/*************
			 ** realise **
			 *************/

			PathSet context;
			Value func;
			if (args.empty())
				func = *v;
			else
				state.eval_state.autoCallFunction(args, *v, func);

			Value arg_value;
			mkString(arg_value, nix_arg);

			Value result;
			state.eval_state.callFunction(func, arg_value, result, noPos);

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

	public:

		/**
		 * Constructor
		 */
		Service_proxy(char const *service_name, State &state)
		: _backend(service_name), _state(state)
		{
			state.env.parent().announce(state.env.ep().manage(*this));
		}

		/********************
		 ** Root interface **
		 ********************/

		Session_capability session(Root::Session_args const &args,
		                           Affinity           const &affinity)
		{
			enum {
				   ARGS_MAX_LEN = 160,
				NIX_ARG_MAX_LEN = 128
			};

			char new_args[ARGS_MAX_LEN];
			char nix_arg[NIX_ARG_MAX_LEN];
			nix::Path out;

			read_nix_arg(nix_arg, NIX_ARG_MAX_LEN, args);
			char const *arg = nix_arg;

			Session_label const label = label_from_args(args.string());
			try {
				try {
					Session_policy policy(label, _state.config_rom.xml());
					nix::handleExceptions("nix", [&] {
						out = _realise(policy, arg);
					});
				} catch (Session_policy::No_policy_defined) {
					nix::handleExceptions("nix", [&] {
						out = _realise(Xml_node("<default-policy/>"), arg);
					});
				}
			} catch (...) {
				Genode::error("caught unhandled exception while evaluating '", arg, "'");
				throw Root::Unavailable();
			}

			if (out == "") {
				Genode::error("no evaluation for '", arg, "'");
				throw Root::Unavailable();
			}

			if (out.length() >= Vfs::MAX_PATH_LEN) {
				Genode::error("'", arg, "' did not resolve to a store object");
				throw Root::Unavailable();
			}

			/*
			 * Set the label on the request to "store" to differentiate
			 * this request with requests during evaluation.
			 */
			strncpy(new_args, args.string(), ARGS_MAX_LEN);
			rewrite_args(new_args, sizeof(new_args), out);

			try { return _backend.session(new_args, affinity); }
			catch (Service::Invalid_args)   { throw Root::Invalid_args();  }
			catch (Service::Quota_exceeded) { throw Root::Quota_exceeded(); }
			catch (...) { }
			throw Root::Unavailable();
		}

		void upgrade(Session_capability        cap,
		             Root::Upgrade_args const &args) override {
			_backend.upgrade(cap, args.string()); }

		void close(Session_capability cap) override {
			_backend.close(cap); }
};


class Nix::Rom_root : public Service_proxy<Rom_session>
{
	protected:

		void read_nix_arg(char *arg, size_t arg_len,
		                  Root::Session_args const &args) override
		{
			Genode::Session_label const label = label_from_args(args.string());
			Genode::strncpy(arg, label.last_element().string(), arg_len);
		}

		void rewrite_args(char *args, size_t args_len, nix::Path &out) override
		{
			using namespace Genode;

			// XXX: slash hack
			while (out.front() == '/')
				out.erase(0,1);

			Session_label const new_label = prefixed_label(
				Session_label("store"), Session_label(out.c_str()));

			Arg_string::set_arg_string(
				args, args_len, "label", new_label.string());
		}

	public:

		Rom_root(State &state)
		: Service_proxy<Rom_session>("ROM", state) { }
};


class Nix::File_system_root : public Service_proxy<File_system::Session>
{
	protected:

		void read_nix_arg(char *arg, size_t arg_len,
		                  Root::Session_args const &args) override
		{
			Genode::Arg_string::find_arg(args.string(), "root").string(
				arg, arg_len, "");

			while (*arg == '/')
				strncpy(arg, arg+1, arg_len-1);
		}

		void rewrite_args(char *args, size_t args_len, nix::Path &out) override
		{
			Arg_string::set_arg_string(args, args_len, "label", "store");
			Arg_string::set_arg_string(args, args_len, "root", out.c_str());
			Arg_string::set_arg(args, args_len, "writeable", false);
		}

	public:

		File_system_root(State &state)
		: Service_proxy<File_system::Session>("File_system", state)
		{ }
};


	/*
	 * XXX: Nix uses this stack for the evaluation,
	 * so the threat of a blown stack depends on
	 * the complexity of the evaulation.
	 */
Genode::size_t Component::stack_size() { return 32*1024*sizeof(long); }

void Component::construct(Genode::Env &env)
{
	using namespace Nix;

	static            State    state { env };
	static         Rom_root rom_root { state };
	static File_system_root  fs_root { state };
}
