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
#include <vfs/file_system_factory.h>
#include <vfs/dir_file_system.h>
#include <os/session_policy.h>
#include <os/config.h>
#include <base/service.h>
#include <rom_session/capability.h>
#include <cap_session/connection.h>
#include <base/rpc_server.h>
#include <root/root.h>
#include <base/sleep.h>
#include <util/label.h>

namespace Nix {
	using namespace Genode;

	struct State_handle;
	struct State;

	template <typename SESSION_TYPE> class Service_proxy;

	class Rom_root;
	class File_system_root;
}


/**
 * Handle that prevents the state from being destroyed
 */
class Nix::State_handle
{
	private:

		Lock           &lock;

	public:

		nix::Store     &store;
		nix::EvalState &eval_state;

		State_handle(Lock &lock, nix::Store &store, nix::EvalState &eval)
		: lock(lock), store(store), eval_state(eval) { }

		~State_handle() { lock.unlock(); }
};


/**
 * Allocates and destroys state
 */
class Nix::State
{
	private:

		Vfs::Dir_file_system  vfs;
		Lock &_lock;

		struct Internal_state
		{
			nix::Store            store;
			nix::EvalState        eval_state;

			Internal_state()
			: eval_state(store, config()->xml_node().sub_node("nix"))
			{ }
		};

		Internal_state *_state;

	public:

		State(Lock &lock)
		:
			vfs(config()->xml_node().sub_node("vfs"),
			    Vfs::global_file_system_factory()),
			_lock(lock), _state(nullptr)
		{ nix::initNix(vfs); }

		~State()
		{
			_lock.lock();
			if (_state != nullptr)
				destroy (env()->heap(), _state);
		}

		State_handle lock()
		{
			_lock.lock();
			if (_state == nullptr)
				_state = new (env()->heap()) Internal_state;

			return State_handle(_lock, _state->store, _state->eval_state);
		}

		/**
		 * Called from the main thread after locking
		 */
		void free()
		{
			if (_state == nullptr) return;

			destroy(env()->heap(), _state);
			_state = nullptr;
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

			State_handle state = _state.lock();

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
		: _backend(service_name), _state(state) { }

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

			Session_label label(args.string());
			try {
				Session_policy policy(label);
				nix::handleExceptions("nix", [&] {
					out = _realise(policy, nix_arg);
				});
			} catch (Session_policy::No_policy_defined) {
				nix::handleExceptions("nix", [&] {
					out = _realise(Xml_node("<policy/>", 9), nix_arg);
				});
			}

			if (out == "") {
				PERR("no evaluation for '%s'", nix_arg);
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
			Genode::Label label = Genode::Arg_string::label(args.string());
			Genode::strncpy(arg, label.last_element(), arg_len);
		}

		void rewrite_args(char *args, size_t args_len, nix::Path &out) override
		{
			// XXX: slash hack
			while (out.front() == '/')
				out.erase(0,1);

			Arg_string::set_arg_string(
				args, args_len, "label", Label(out.c_str(), "store").string());
		}

	public:

		Rom_root(State &state)
		: Service_proxy<Rom_session>("ROM", state)
		{ }
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


int main(void)
{
	using namespace Nix;

	static Lock  lock(Cancelable_lock::LOCKED);
	static State state(lock);

	static Rom_root rom_root(state);
	static File_system_root fs_root(state);

	static Cap_connection cap;

	/*
	 * XXX: Nix uses this stack for the evaluation,
	 * so the threat of a blown stack is entirely
	 * dependent on the complexity of the evaulation.
	 *
	 * It needs to be dealt with gracefully.
	 */
	enum { STACK_SIZE = 32*1024*sizeof(long) };
	static Rpc_entrypoint ep(&cap, STACK_SIZE, "nix_ep");

	static Signal_receiver sig_rec;
	static Signal_context  yield_ctx;
	static Signal_context  config_ctx;

	env()->parent()->yield_sigh(sig_rec.manage(&yield_ctx));
	config()->sigh(sig_rec.manage(&config_ctx));

	env()->parent()->announce(ep.manage(&rom_root));
	env()->parent()->announce(ep.manage(&fs_root));

	/* wait on yield signals and config updates */
	for (;;) {
		lock.unlock();
		Signal sig = sig_rec.wait_for_signal();
		lock.lock();

		/* destroy the Nix state */
		state.free();

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
