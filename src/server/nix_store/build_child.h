/*
 * \brief  Builder child
 * \author Emery Hemingway
 * \date   2015-03-13
 */

#ifndef _NIX_STORE__BUILD_CHILD_H_
#define _NIX_STORE__BUILD_CHILD_H_

/* Genode includes */
#include <store_hash/encode.h>
#include <file_system_session/file_system_session.h>
#include <base/env.h>
#include <base/child.h>
#include <base/printf.h>
#include <log_session/connection.h>
#include <pd_session/connection.h>
#include <ram_session/connection.h>
#include <cpu_session/connection.h>
#include <rm_session/connection.h>
#include <init/child_policy.h>
#include <os/server.h>
#include <util/avl_string.h>

/* Nix includes */
#include <nix_store/derivation.h>

/* Local imports */
#include "ingest_fs_service.h"
#include "filter_fs_service.h"

namespace Nix_store {

	/**
	 * Used here and in build_job.h
	 */
	enum QUOTA : size_t {
		MEGABYTE      = 1 << 20,
		QUOTA_STEP    = 8*MEGABYTE,
		QUOTA_RESERVE = 1*MEGABYTE,

		ENTRYPOINT_STACK_SIZE = 8*1024*sizeof(long)
	};

	class Child;

}

class Nix_store::Child : public Genode::Child_policy
{
	private:

		char const            *_name;
		File_system::Session  &_fs;
		Nix_store::Derivation  _drv { _name };

		struct Mapping : Avl_node<Mapping>
		{
			Genode::String<File_system::MAX_NAME_LEN> const key;
			Genode::String<File_system::MAX_PATH_LEN> const value;

			Mapping(char const *key_str, char const *value_str)
			: key(key_str), value(value_str) { }

			/************************
			 ** Avl node interface **
			 ************************/

			bool higher(Mapping *m) {
				return (strcmp(m->key.string(), key.string()) > 0); }

			Mapping *lookup(const char *key_str)
			{
				if (key == key_str) return this;

				Mapping *m =
					Avl_node<Mapping>::child(strcmp(key_str, key.string()) > 0);
				return m ? m->lookup(key_str) : 0;
			}

		};

		struct Environment : Genode::Avl_tree<Mapping>
		{
			/*
			 * XXX: resolve inputs to content addressed paths
			 * Parse the inputs first and resolve the symlinks before
			 * populating the mappings.
			 */

			struct Input :  List<Input>::Element
			{
				Nix_store::Name const _link;
				Nix_store::Name const _dest;
				size_t          const _len;

				Input (char const *link, char const *target)
				:
					_link(link),
					_dest(target),
					_len(strlen(_link.string()))
				{ };

				bool match (char const *other) {
					return strcmp(other, _link.string(), _len) == 0; }

				char const *dest() { return _dest.string(); }

				size_t old_len() { return _len; }
			};

			/**
			 * XXX: is this duplicated in the filtered fs service?
			 */
			struct Inputs : List<Input>
			{
				Inputs(File_system::Session &fs, Nix_store::Derivation &drv)
				{
					using namespace File_system;
					using Nix_store::Name;

					Dir_handle root_handle = fs.dir("/", false);
					Handle_guard guard_root(fs, root_handle);

					drv.inputs([&]  (Aterm::Parser &parser) {
						Name input_drv;
						parser.string(&input_drv);

						Nix_store::Derivation depend(input_drv.string());

						parser.list([&] (Aterm::Parser &parser) {
							Name want_id;
							parser.string(&want_id);

							depend.outputs([&] (Aterm::Parser &parser) {
								Name id;
								parser.string(&id);

								if (id != want_id) {
									parser.string(); /* Path */
									parser.string(); /* Algo */
									parser.string(); /* Hash */
									return;
								}

								Name input_path;
								parser.string(&input_path);
								parser.string(); /* Algo */
								parser.string(); /* Hash */

								char const *input_name = input_path.string();
								/* XXX : slash hack */
								while (*input_name == '/') ++input_name;

								Symlink_handle link =
									fs.symlink(root_handle, input_name, false);
								Handle_guard guard(fs, link);
								char target[MAX_NAME_LEN];
								memset(target, 0x00, MAX_NAME_LEN);
								read(fs, link, target, sizeof(target));
								insert(new (env()->heap()) Input(input_path.string(), target));
							});
						});
					});
				}

				~Inputs()
				{
					while (Input *input = first()) {
						remove(input);
						destroy(env()->heap(), input);
					}
				}

				Input *lookup(char const *path)
				{
					for (Input *input = first(); input; input = input->next()) {
						if (input->match(path)) return input;
					}
					return 0;
				}
			};

			Environment(File_system::Session &fs, Nix_store::Derivation &drv)
			{
				using namespace File_system;

				Inputs inputs(fs, drv);

				typedef Genode::Path<MAX_PATH_LEN>   Path;
				typedef Genode::String<MAX_PATH_LEN> String;
				typedef Genode::String<MAX_NAME_LEN> Name;

				drv.environment([&] (Aterm::Parser &parser) {
					Name   key;
					String value;
					parser.string(&key);
					parser.string(&value);

					Path value_path(value.string());

					bool top_level = value_path.has_single_element();
					while(value.length() > 1 && !value_path.has_single_element())
						value_path.strip_last_element();

					Input *input = inputs.lookup(value_path.base());
					Mapping *map;

					if (!input) {
						map = new (env()->heap())
							Mapping(key.string(), value.string());

					} else if (top_level) {
						map = new (env()->heap())
							Mapping(key.string(), input->dest());

					} else {
						/* rewrite the leading directory */
						Path new_path(value.string()+input->old_len(), input->dest());
						map = new (env()->heap())
							Mapping(key.string(), new_path.base());
					}
					insert(map);
				});
			}

			~Environment()
			{
				while(Mapping *map = (Mapping *)first()) {
					remove(map);
					destroy(env()->heap(), map);
				}
			}

			char const *lookup(char const *key)
			{
				Mapping *m = first();
				m = m ? m->lookup(key) : 0;
				return m ? m->value.string() : 0;
			}

		} _environment { _fs, _drv };

		/**
		 * Resources assigned to the child.
 		*/
		struct Resources
		{
			Genode::Pd_connection  pd;
			Genode::Ram_connection ram;
			Genode::Cpu_connection cpu;
			Genode::Rm_connection  rm;

			/**
			 * Constructor
			 */
			Resources(char const               *label,
			          Signal_context_capability exit_sigh)
			: pd(label), ram(label), cpu(label)
			{
				cpu.exception_handler(Thread_capability(), exit_sigh);
				rm.fault_handler(exit_sigh);

				ram.ref_account(Genode::env()->ram_session_cap());

				Genode::env()->ram_session()->transfer_quota(ram.cap(), QUOTA_STEP);
			}
		} _resources;

		Rom_connection             _binary_rom;
		Server::Entrypoint         _entrypoint;
		Signal_context_capability  _exit_sigh;
		/* XXX: use an allocator with a lifetime bound to the child */
		Ingest_service             _fs_ingest_service { _drv, _entrypoint, *Genode::env()->heap() };
		Filter_service             _fs_filter_service { _drv, _entrypoint, *Genode::env()->heap() };
		Parent_service             _fs_parent_service { "File_system" };
		Service_registry           _parent_services;

		Genode::Child  _child;

	public:

		/**
		 * Constructor
		 */
		Child(char const               *name,
		      File_system::Session     &fs,
		      Signal_context_capability exit_sigh)
		:
			_name(name),
			_fs(fs),
			_resources(name, exit_sigh),
			_binary_rom(_drv.builder()),
			_exit_sigh(exit_sigh),
			_child(_binary_rom.dataspace(),
			       _resources.pd.cap(),
			       _resources.ram.cap(),
			       _resources.cpu.cap(),
			       _resources.rm.cap(),
			      &_entrypoint.rpc_ep(),
			        this)
		{
			/*
			 * A whitelist of services to forward from our parent.
			 *
			 * TODO: enforce purity and do not forward ROM,
			 * all executables and libraries should be in the store.
			 */
			static char const *service_names[] = {
				"CAP", "CPU", "LOG", "PD", "RAM", "RM", "ROM", "SIGNAL",
				"Timer", 0
			};
			for (unsigned i = 0; service_names[i]; ++i)
				_parent_services.insert(
					new (env()->heap())
						Parent_service(service_names[i]));

			if (_drv.has_fixed_output()) {
				char service_name[32];

				char const *impure = _environment.lookup("impureServices");
				size_t len = Genode::strlen(impure);
				size_t start = 0, end = 1;

				while (end <= len) {
					if (impure[end] == ' ' || impure[end] == '\0') {
						++end;
						Genode::strncpy(service_name, impure+start, end-start);
						PLOG("%s: forwarding impure service '%s' to parent", _name, service_name);
						_parent_services.insert(new (env()->heap()) Parent_service(service_name));
						start = end;
					} else
						++end;
				}
			}
		}


		/****************************
		 ** Child policy interface **
		 ****************************/

		char const *name() const { return _name; }

		void filter_session_args(const char *service, char *args, size_t args_len)
		{
			/*
			 * Rewrite ROM requests to enforce purity
			 */
			if (strcmp(service, "ROM") == 0) {

				Genode::Label label = Genode::Arg_string::label(args);
				char const *request = label.last_element();

				/*
				 * XXX: make a set_string method on Arg_string::
				 * that inserts the proper punctuation.
				 */
				if (strcmp("binary", request) == 0) {
					Arg_string::set_arg_string(args, args_len, "label", _drv.builder());
				} else if (char const *dest = _environment.lookup(request)) {
					Arg_string::set_arg_string(args, args_len, "label", dest);
				} else {
					PERR("impure ROM request for '%s'", request);
					*args = '\0';
				}

				return;
			}

			/*
			 * Rewrite File_system roots
			 */
			else if (strcmp(service, "File_system") == 0) {
				char root[File_system::MAX_PATH_LEN] = { '\0' };

				Genode::Arg_string::find_arg(args, "root").string(
					root, sizeof(root), "/");

				if (strcmp(root, "", sizeof(root))
				 && strcmp(root, "/", sizeof(root))) {
					if (char const *dest = _environment.lookup(root)) {
						Arg_string::set_arg_string(args, args_len, "root", dest);
						Arg_string::set_arg(args, args_len, "writeable", false);
					} else {
						PERR("impure File_system request for root '%s'", root);
						*args = '\0';
					}
				}
				return;
			}

			/*
			 * Log messages need be seperated between build jobs
			 */
			else if (strcmp(service, "LOG") == 0) {
				char label_buf[Parent::Session_args::MAX_SIZE];
				Arg_string::find_arg(args, "label").string(label_buf, sizeof(label_buf), "");

				/* shorten the log label */
				Genode::String<18> short_name(_name);
				if (label_buf[0]) {
					Genode::Label label(label_buf, short_name.string());
					Arg_string::set_arg_string(args, args_len, "label", label.string());
				} else
					Arg_string::set_arg_string(args, args_len, "label", short_name.string());
				return;
			}

			/*
			 * labeling anything else could endanger purity.
			 */
			Arg_string::remove_arg(args, "label");
		}

		Service *resolve_session_request(const char *service_name,
		                                 const char *args)
		{
			Service *service = 0;
			if (!(*args)) /* invalidated by filter_session_args */
				return 0;

			if (strcmp("File_system", service_name) == 0)
			{
				Genode::Label label = Genode::Arg_string::label(args);
				char root[3] = { '\0' };

				if (!strcmp("ingest", label.last_element()))
					return &_fs_ingest_service;

				Genode::Arg_string::find_arg(args, "root").string(
					root, sizeof(root), "/");

				if (strcmp(root, "", sizeof(root))
				 && strcmp(root, "/", sizeof(root)))
					return &_fs_filter_service;

				/*
				 * If there is a root argument then connect directly to 
				 * the store backend because the session is isolated by
				 * the root offset.
				 */
				return &_fs_parent_service;
			}

			/* get it from the parent if it is allowed */
			service = _parent_services.find(service_name);

			if (!service)
				PERR("%s request from %s rejected", service_name, _name);
			return service;
		}

		void exit(int exit_value)
		{
			if (exit_value == 0 && _fs_ingest_service.finalize(_fs, _drv))
				PINF("success: %s", _name);
			else
				PERR("failure: %s", _name);

			/* TODO: write a store placeholder that marks a failure */

			Signal_transmitter(_exit_sigh).submit();
		}

		void resource_request(Parent::Resource_args const &args)
		{
			PDBG("%s", args.string());
			size_t ram_request =
				Arg_string::find_arg(args.string(), "ram_quota").ulong_value(0);

			if (!ram_request) return;

			ram_request = max(size_t(QUOTA_STEP), size_t(ram_request));

			size_t ram_avail =
				Genode::env()->ram_session()->avail() - QUOTA_RESERVE;

			if (ram_avail > ram_request) {

				Genode::env()->ram_session()->transfer_quota(
					_resources.ram.cap(), ram_avail);

				_child.notify_resource_avail();
				return;
			}

			char arg_buf[32];
			snprintf(arg_buf, sizeof(arg_buf),
			         "ram_quota=%ld", ram_request);

			Genode::env()->parent()->resource_request(arg_buf);
		}

		void upgrade_ram()
		{
			size_t quota_avail =
				Genode::env()->ram_session()->avail() - QUOTA_RESERVE;

				Genode::env()->ram_session()->transfer_quota(
					_resources.ram.cap(), quota_avail);

			_child.notify_resource_avail();
		}
};

#endif
