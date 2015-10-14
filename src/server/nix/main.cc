/*
 * \brief  Serve Nix expressions as ROM dataspaces
 * \author Emery Hemingway
 * \date   2015-10-13
 *
 * This component can barely be called a server.
 * Incoming session requestes are rewritten with
 * the result of a Nix evaulation and forwarded
 * to a ROM session provided by the parent.
 */

/*
 * Copyright (C) 2015 Genode Labs GmbH
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
#include <builder_session/builder_session.h>
#include <vfs/file_system_factory.h>
#include <vfs/dir_file_system.h>
#include <vfs/rom_file_system.h>
#include <vfs/fs_file_system.h>
#include <os/session_policy.h>
#include <os/config.h>
#include <base/service.h>
#include <rom_session/capability.h>
#include <cap_session/connection.h>
#include <base/rpc_server.h>
#include <root/root.h>
#include <base/sleep.h>

namespace Nix {
	using namespace Genode;

	struct State;
	class  Rom_root;
}

/**
 * Nix evaluation state and service connections
 *
 * This struct allows the evaluator to be destroyed at a yield
 * request or re-instantiated with configuration changes.
 */
struct Nix::State
{
	Vfs::Dir_file_system vfs;
	nix::Vfs_root        vfs_root;
	nix::Store           store;
	nix::EvalState       eval_state;

	State()
	:
		vfs(config()->xml_node().sub_node("nix").sub_node("vfs"),
		    Vfs::global_file_system_factory()),
		vfs_root(vfs),
		store(vfs_root),
		eval_state(vfs_root, store, config()->xml_node().sub_node("nix"))
	{ }

};


class Nix::Rom_root : public Genode::Rpc_object<Genode::Typed_root<Genode::Rom_session>>
{
	private:

		Parent_service  _backend;
		Lock           &_lock;
		State          *_state;

		nix::Path _realise(Genode::Xml_node policy, char const *rom_filename)
		{
			Lock::Guard guard(_lock);

			if (_state == nullptr)
				_state = new (env()->heap()) State;

			using namespace nix;

			enum { TMP_BUF_LEN = 256 };
			char tmp_buf[TMP_BUF_LEN] = { '\0' };

			Value root_value;
			Expr *e;


			/****************
			 ** Parse file **
			 ****************/

			/* XXX: use the string methods from Xml_node */
			try {
				policy.attribute("file").value(tmp_buf, sizeof(tmp_buf));
				e = _state->eval_state.parseExprFromFile(nix::Path(tmp_buf));

			} catch (Xml_node::Nonexistent_attribute) {
				e = _state->eval_state.parseExprFromFile("/default.nix");
			}
			_state->eval_state.eval(e, root_value);


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

			/* enclose value in quotes as a convienence*/
			policy.for_each_sub_node("argstr", [&arg_map] (Xml_node arg_node) {
				char  name[64] = { '\0' };
				char value[64] = { '"' };

				arg_node.attribute("name").value(name, sizeof(name));
				arg_node.attribute("value").value(value+1, sizeof(value)-1);
				arg_map[name] = value;
			});

			Bindings &args(*evalAutoArgs(_state->eval_state, arg_map));


			/********************
			 ** Find attribute **
			 ********************/

			Value *v;
			try {
				policy.attribute("attr").value(tmp_buf, sizeof(tmp_buf));
				v = findAlongAttrPath(
					_state->eval_state, tmp_buf, args, root_value);
			} catch (Xml_node::Nonexistent_attribute) {
				v = findAlongAttrPath(
					_state->eval_state, "", args, root_value);
			}


			/*************
			 ** realise **
			 *************/

			PathSet context;
			Value func;
			if (args.empty())
				func = *v;
			else
				_state->eval_state.autoCallFunction(args, *v, func);

			Value filename_value;
			mkString(filename_value, rom_filename);

			Value result;
			_state->eval_state.callFunction(func, filename_value, result, noPos);

			DrvInfo drv_info(_state->eval_state);
			if (getDerivation(_state->eval_state, result, drv_info, false)) {
				PathSet drv_set{ drv_info.queryDrvPath() };
				_state->store.buildPaths(drv_set, nix::bmNormal);

				return drv_info.queryOutPath();
			}

			switch (result.type) {
			case tPath:
				return result.path;
			case tString:
				return nix::Path(result.string.s);
			default:
				PERR("evaluation result is not a string or path");
			}
			return "";
		}

	public:

		/**
		 * Constructor
		 */
		Rom_root(Lock &lock) : _backend("ROM"), _lock(lock), _state(nullptr) { }

		/**
		 * Free the Nix state and inform the parent
		 *
		 * Rom_root will be locked by the main thread
		 * before calling this method.
		 */
		void free_state()
		{
			if (_state == nullptr) return;

			PDBG("destroying evaluation state");
			destroy(env()->heap(), _state);
			_state = nullptr;
		}


		/********************
		 ** Root interface **
		 ********************/

		Session_capability session(Root::Session_args const &args,
		                           Affinity           const &affinity)
		{
			enum {
				    ARGS_MAX_LEN = 160,
				FILENAME_MAX_LEN = 128
			};

			char new_args[ARGS_MAX_LEN];
			char filename[FILENAME_MAX_LEN];

			Session_label label(args.string());

			Genode::Arg_string::find_arg(args.string(), "filename").string(
				filename, sizeof(filename), "");

			if (filename[0] == '\0')
				throw Invalid_args();

			nix::Path out;

			try {
				Session_policy policy(label);
				out = _realise(policy, filename);
			} catch (Session_policy::No_policy_defined) {
				out = _realise(Xml_node("</>", 3), filename);
			}

			if (out == "") {
				PERR("no evaluation for '%s'", filename);
				throw Root::Unavailable();
			}

			// XXX: slash hack
			while (out.front() == '/')
				out.erase(0,1);

			out.insert(0, "\"");
			out.push_back('"');

			strncpy(new_args, args.string(), ARGS_MAX_LEN);
			Arg_string::set_arg(new_args, ARGS_MAX_LEN, "filename", out.c_str());
			Arg_string::set_arg(new_args, ARGS_MAX_LEN, "label", "store");

			try { return _backend.session(new_args, affinity); }
			catch (Service::Invalid_args)   { throw Invalid_args();  }
			catch (Service::Quota_exceeded) { throw Quota_exceeded(); }
			catch (...) { }
			throw Root::Unavailable();
		}

		void upgrade(Session_capability        cap,
		             Root::Upgrade_args const &args) override
		{
			_backend.upgrade(cap, args.string());
		}

		void close(Session_capability cap) override
		{
			_backend.close(cap);
		}
};

int main(void)
{
	using namespace Genode;

	static Lock          lock;
	static Nix::Rom_root rom_root(lock);

	/* connection to capability service needed to create capabilities */
	static Cap_connection cap;

	enum { STACK_SIZE = 2*1024*sizeof(long) };
	static Rpc_entrypoint ep(&cap, STACK_SIZE, "nix_rom_ep");

	env()->parent()->announce(ep.manage(&rom_root));

	/* wait on yield signals from the parent and config updates */
	Signal_receiver sig_rec;
	Signal_context  yield_ctx;
	Signal_context  config_ctx;
	env()->parent()->yield_sigh(sig_rec.manage(&yield_ctx));
	config()->sigh(sig_rec.manage(&config_ctx));

	for (;;) {
		Signal sig = sig_rec.wait_for_signal();

		Lock::Guard guard(lock);

		rom_root.free_state();

		if (sig.context() == &yield_ctx) {
			PDBG("yielding resources to parent");
			env()->parent()->yield_response();
		}

		else

		if (sig.context() == &config_ctx) {
			PDBG("reloading config");
			config()->reload();
		}
	}
}