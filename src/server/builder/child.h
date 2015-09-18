/*
 * \brief  Builder child
 * \author Emery Hemingway
 * \date   2015-03-13
 */

#ifndef _BUILDER__CHILD_H_
#define _BUILDER__CHILD_H_

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
#include <cli_monitor/ram.h>
#include <os/server.h>
#include <util/avl_string.h>

/* Local imports */
#include "config.h"
#include "derivation.h"
#include "fs_policy.h"

namespace Builder {

	class Quota_exceeded : public Genode::Exception { };

	struct Resources;
	class  Child_policy;
	class  Child;

}

/**
 * Resources assigned to the child.
 */
struct Builder::Resources
{
	enum { MINIMUM_QUOTA = 8 * 1024 * sizeof(long) };

	Genode::Pd_connection  pd;
	Genode::Ram_connection ram;
	Genode::Cpu_connection cpu;
	Genode::Rm_connection  rm;

	Ram &ram_manager;

	/**
	 * Constructor
	 */
	Resources(char const               *label,
	          Ram                      &jobs_ram,
	          Signal_context_capability exit_sigh)
	:
		pd(label), ram(label), cpu(label), /* TODO: priority */
		ram_manager(jobs_ram)
	{
		cpu.exception_handler(Thread_capability(), exit_sigh);
		rm.fault_handler(exit_sigh);

		Ram::Status status = ram_manager.status();

		/*
		 * TODO: dynamic quota
		 */
		Genode::size_t ram_quota = status.avail / 2;
		if (ram_quota < MINIMUM_QUOTA)
			throw Ram::Transfer_quota_failed();

		ram.ref_account(Genode::env()->ram_session_cap());
		PLOG("transfering %lu bytes to %s ram quota", ram_quota, label);
		Genode::env()->ram_session()->transfer_quota(ram.cap(), ram_quota);
	}

	void transfer_ram(size_t amount) {
		ram_manager.transfer_to(ram.cap(), amount); }

};


class Builder::Child_policy : public Genode::Child_policy
{
	private:

		char const                *_name;
		Log_connection             _log;
		File_system::Session      &_fs;
		Derivation                &_drv;

		typedef Avl_string<MAX_NAME_LEN> Input;

		class Inputs : public Avl_tree<Avl_string_base>
		{
			private:

				/**
				 *
				 */
				void add_drv_output(char const *buf, size_t len, char const *id)
				{
					struct Done { };

					try {
						Aterm::Parser parser(buf, len);
						parser.constructor("Derive", [&] {
							/*************
							 ** Outputs **
							 *************/
							parser.list([&] {
								parser.tuple([&] {
									char output_id[MAX_NAME_LEN-Store_hash::HASH_PREFIX_LEN];
									parser.string().value(output_id, sizeof(output_id));
									if (!strcmp(id, output_id, sizeof(output_id))) {
										char output_hash[MAX_NAME_LEN];
										parser.string().value(output_hash, sizeof(output_hash));
										Input *input = new (env()->heap()) Input(output_hash);
										insert(input);

										/* A hack to return without defining the entire term. */
										throw Done();

										parser.string(); /* Algo */
										parser.string(); /* Hash */
									}
								});
							});

							/* The output id was not found in this derivation. */
							throw Invalid_derivation();
						});
					} catch (Done) {
					} catch (Rom_connection::Rom_connection_failed) {
						throw Missing_dependency();
					}
				}

			public:

				Inputs(Derivation &drv)
				{
					for (Derivation::Input *drv_input = drv.input();
					     drv_input; drv_input = drv_input->next()) {

						char const *depend_drv = drv_input->path.string();
						while (*depend_drv == '/') ++depend_drv;
						char *p = 0;
						try {
							Rom_connection rom(depend_drv, "store");
							Rom_dataspace_capability ds_cap = rom.dataspace();
							p = env()->rm_session()->attach(ds_cap);
							size_t len = Dataspace_client(ds_cap).size();

							for (Aterm::Parser::String *s = drv_input->ids.first();
							     s; s = s->next()) {
								char id[MAX_NAME_LEN-Store_hash::HASH_PREFIX_LEN];
								s->value(id, sizeof(id));

								add_drv_output(p, len, id);
							}
						} catch (...) {
							if (p) env()->rm_session()->detach(p);
							throw;
						}
						env()->rm_session()->detach(p);
					}

					for (Aterm::Parser::String *src = drv.source();
					     src; src = src->next()) {
						char path[MAX_NAME_LEN];
						src->value(path, sizeof(path));
						Input *input = new (env()->heap()) Input(path);
						insert(input);
					}
				}

				~Inputs()
				{
					while(Input *input = (Input *)first()) {
						remove(input);
						destroy(env()->heap(), input);
					}
				}

				bool contains(char const *name)
				{
					Avl_string_base *node = first();
					return node->find_by_name(name);
				}

		} _inputs;

		class Environment : public Avl_tree<Avl_string_base>
		{
			private:

				struct Mapping : Avl_string<MAX_NAME_LEN>
				{
					char const *value;

					Mapping(char const *name, char const *value)
					: Avl_string(name), value(value) { }
				};

			public:

				Environment(Derivation &drv, Inputs &inputs)
				{
					for (Derivation::Env_pair *pair = drv.env(); pair; pair = pair->next()) {
						Mapping *node = new (env()->heap())
							Mapping(pair->key, pair->value);
						insert(node);
					}
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
					if (Avl_string_base *node = first()) {
						if ((node = node->find_by_name(key))) {
							Mapping *map = static_cast<Mapping *>(node);
							return map->value;
						}
					}
					return 0;
				}

		} _environment;

		Signal_context_capability  _exit_sigh;
		Resources                 &_resources;
		Parent_service             _fs_input_service;
		Store_fs_policy            _fs_output_policy;
		Parent_service             _parent_fs_service;
		Service_registry           _parent_services;

	public:

		/**
		 * Constructor
		 */
		Child_policy(char const               *name,
		             File_system::Session     &fs,
		             Resources                &resources,
		             Server::Entrypoint       &ep,
		             Derivation               &drv,
		             Signal_context_capability exit_sigh)
		:
			_name(name),
			_log(name),
			_fs(fs),
			_drv(drv),
			_inputs(drv),
			_environment(drv, _inputs),
			_exit_sigh(exit_sigh),
			_resources(resources),
			_fs_input_service("File_system"),
			_fs_output_policy(_fs_input_service, ep),
			_parent_fs_service("File_system")
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
		}

		/*
		 * TODO: actually implement resource trading
		 */

		/****************************
		 ** Child policy interface **
		 ****************************/

		char const *name() const { return _name; }

		/**
		 * Rewrite ROM and File_system requests to enforce purity, set session
		 * labels to a user defined value so that the proper routing is enforced.
		 */
		void filter_session_args(const char *service, char *args, size_t args_len)
		{
			if (strcmp(service, "ROM") == 0) {
				char filename[File_system::MAX_PATH_LEN] = { 0 };
				char label_buf[64];

				Genode::Arg_string::find_arg(args, "filename").string(
					filename, sizeof(filename), "");

				if (!*filename) {
					PERR("invalid ROM request");
					return;
				}

				/* XXX: make a set_string method on Arg_string:: */
				if (strcmp(filename, "binary") == 0) {
					snprintf(filename, sizeof(filename), "\"%s\"", _drv.builder());
					Arg_string::set_arg(args, args_len,
					                    "filename", filename);

				} else if (char const *dest = _environment.lookup(filename)) {
					snprintf(filename, sizeof(filename), "\"%s\"", dest);

					Arg_string::set_arg(args, args_len, "filename", filename);
				} else {
					PERR("impure ROM request for '%s'", filename);
					*args = '\0';
					return;
				}

				Genode::snprintf(label_buf, sizeof(label_buf),
				                 "\"%s\"", rom_label());
				Arg_string::set_arg(args, args_len, "label", label_buf);
				return;
			}

			if (strcmp(service, "File_system") == 0) {
				char root[File_system::MAX_PATH_LEN] = { 0 };
				Genode::Arg_string::find_arg(args, "root").string(
					root, sizeof(root), "/");

				if (strcmp(root, "/") != 0) {
					char root[File_system::MAX_PATH_LEN];
					char label_buf[64];

					if (char const *dest = _environment.lookup(root)) {
						snprintf(root, sizeof(root), "\"%s\"", dest);
						Arg_string::set_arg(args, args_len, "root", root);
					} else {
						PERR("impure FS request for '%s'", root);
						*args = '\0';
						return;
					}

					Genode::snprintf(label_buf, sizeof(label_buf),
					                 "\"%s\"", fs_label());
					Arg_string::set_arg(args, args_len, "label", label_buf);
				}
				return;
			}

			char label_buf[Parent::Session_args::MAX_SIZE];
			Arg_string::find_arg(args, "label").string(label_buf, sizeof(label_buf), "");

			char value_buf[Parent::Session_args::MAX_SIZE];
			Genode::snprintf(value_buf, sizeof(value_buf),
			                 "\"%s%s%s\"",
			                 _name,
			                 Genode::strcmp(label_buf, "") == 0 ? "" : " -> ",
			                 label_buf);

			Arg_string::set_arg(args, args_len, "label", value_buf);
		}

		Service *resolve_session_request(const char *service_name,
		                                 const char *args)
		{
			Service *service = 0;
			if (!(*args)) /* invalidated by filter_session_args */
				return service;

			if (strcmp("File_system", service_name) == 0) {
				char root[File_system::MAX_PATH_LEN];
				Arg_string::find_arg(args, "root").string(root, sizeof(root), "/");

				if (strcmp("/", root, sizeof(root)) == 0)
					/* this is a session for writing the derivation outputs */
					return _fs_output_policy.service();

				return &_parent_fs_service;
			}

			/* get it from the parent if it is allowed */
			service = _parent_services.find(service_name);

			if (!service)
				PERR("%s request from %s rejected", service_name, _name);
			return service;
		}

		void exit(int exit_value)
		{
			if (exit_value == 0 && _fs_output_policy.finalize(_fs, _drv))
				PINF("success: %s", _name);
			else
				PERR("failure: %s", _name);

			/* TODO: write a store placeholder that marks a failure */

			Signal_transmitter(_exit_sigh).submit();
		}
};


class Builder::Child
{
	public:

		class Quota_exceeded : public Genode::Exception { };

	private:

		enum { ENTRYPOINT_STACK_SIZE  =   8*1024*sizeof(long) };

		Resources           _resources;
		Rom_connection      _binary_rom;
		Server::Entrypoint  _entrypoint;
		Child_policy        _child_policy;
		Genode::Child       _child;

	public:

		/**
		 * Constructor
		 */
		Child(char const               *label,
		      File_system::Session     &fs,
		      Cap_session              &cap_session,
		      Derivation               &drv,
		      Ram                      &ram_manager,
		      Signal_context_capability exit_sigh)
		:
			_resources(label, ram_manager, exit_sigh),
			_binary_rom(drv.builder(), rom_label()),
			_child_policy(label, fs, _resources,  _entrypoint, drv, exit_sigh),
			_child(_binary_rom.dataspace(),
			       _resources.pd.cap(),
			       _resources.ram.cap(),
			       _resources.cpu.cap(),
			       _resources.rm.cap(),
			      &_entrypoint.rpc_ep(),
			      &_child_policy)
		{ }
};

#endif