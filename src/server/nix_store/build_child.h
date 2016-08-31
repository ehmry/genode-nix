/*
 * \brief  Builder child
 * \author Emery Hemingway
 * \date   2015-03-13
 */

/*
 * Copyright (C) 2015-2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _NIX_STORE__BUILD_CHILD_H_
#define _NIX_STORE__BUILD_CHILD_H_

/* Genode includes */
#include <store_hash/encode.h>
#include <file_system_session/file_system_session.h>
#include <base/attached_rom_dataspace.h>
#include <base/attached_ram_dataspace.h>
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

/* Nix includes */
#include <nix_store/derivation.h>

/* Local imports */
#include "environment.h"
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

		Name            const  _name;
		Genode::Env           &_env;
		File_system::Session  &_fs;
		Nix_store::Derivation  _drv { _env, _name.string() };

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
			Resources(Genode::Env &env, char const *label)
			: pd(env, label), ram(env, label), cpu(env, label)
			{
				ram.ref_account(env.ram_session_cap());
				env.ram().transfer_quota(ram.cap(), QUOTA_STEP);
			}
		} _resources { _env, _name.string() };

		Genode::Child::Initial_thread _initial_thread { _resources.cpu, _resources.pd,
		                                                _name.string() };

		Genode::Session_label const _binary_label =
			Genode::prefixed_label(Genode::Session_label("store"),
			                       Genode::Session_label(_drv.builder()));

		Nix::Rom_connection _elf_rom { _env, _drv.builder() };
		Genode::Rom_dataspace_capability _elf_rom_ds = _elf_rom.dataspace();

		Genode::Region_map_client _address_space { _resources.pd.address_space() };

		Genode::Child _child;

		Signal_context_capability  _exit_sigh;
		Inputs      const _inputs      { _env, *_child.heap(), _fs, _drv };
		Environment const _environment { _env, *_child.heap(), _fs, _drv, _inputs };

		Genode::Attached_ram_dataspace _config_dataspace
			{ _env.ram(), _env.rm(), _drv.size() };

		Init::Child_policy_provide_rom_file _config_policy
			{ "config", _config_dataspace.cap(), &_env.ep().rpc_ep() };

		Ingest_service   _fs_ingest_service { _drv, _env, *_child.heap() };
		Filter_service   _fs_filter_service { _env, _inputs };
		Parent_service   _fs_parent_service { "File_system" };
		Service_registry _parent_services;

	public:

		/**
		 * Constructor
		 */
		Child(char const                   *name,
		      Genode::Env                  &env,
		      File_system::Session         &fs,
		      Signal_context_capability     exit_sigh,
		      Genode::Dataspace_capability  ldso_ds)
		:
			_name(name), _env(env), _fs(fs),
			_child(_elf_rom_ds, ldso_ds,
			       _resources.pd.cap(),  _resources.pd,
			       _resources.ram.cap(), _resources.ram,
			       _resources.cpu.cap(), _initial_thread,
			       env.rm(), _address_space, 
			       env.ep().rpc_ep(),
			       *this),
			_exit_sigh(exit_sigh)
		{
			/* ROM is not forwarded verbatim, labels are modified */
			static char const *service_names[] = {
				"CPU", "LOG", "PD", "RAM", "ROM", "RM", "Timer", 0
			};
			for (unsigned i = 0; service_names[i]; ++i)
				_parent_services.insert(
					new (_child.heap())
						Parent_service(service_names[i]));

			if (_drv.has_fixed_output()) {
				char service_name[32];

				char const *impure = _environment.lookup("impureServices");
				if (!impure) {
					PWRN("fixed output derivation without `impureSerices', %s", _name.string());
					return;
				}

				size_t len = Genode::strlen(impure);
				size_t start = 0, end = 1;

				while (end <= len) {
					if (impure[end] == ' ' || impure[end] == '\0') {
						++end;
						Genode::strncpy(service_name, impure+start, end-start);
						Genode::log(_name.string(), ": forwarding impure service ",
						            (char const *)service_name, " to parent");
						_parent_services.insert(new (_child.heap())
							Parent_service(service_name));
						start = end;
					} else
						++end;
				}
			}

			char *config_addr = _config_dataspace.local_addr<char>();
			_drv.config(config_addr, _config_dataspace.size());
		}


		/****************************
		 ** Child policy interface **
		 ****************************/

		char const *name() const {
			return _name.string();
		}

		void filter_session_args(const char *service, char *args, size_t args_len)
		{
			using namespace Genode;

			/*
			 * Rewrite ROM requests to enforce purity
			 */
			if (strcmp(service, "ROM") == 0) {

				Session_label const label = label_from_args(args);
				Session_label const request = label.last_element();

				if (request == "binary") {
					Arg_string::set_arg_string(args, args_len, "label", _binary_label.string());

				} else if (request == "config") {
					return;

				} else if (char const *dest = _environment.lookup(request.string())) {
					Session_label const new_label = prefixed_label(
						Session_label("store"), Session_label(dest));
					Arg_string::set_arg_string(args, args_len, "label", new_label.string());
				} else {
					Genode::error("impure ROM request for '", request.string(), "'");
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
						Arg_string::set_arg_string(args, args_len, "label", "store");
						Arg_string::set_arg_string(args, args_len, "root", dest);
						Arg_string::set_arg(args, args_len, "writeable", false);
					} else {
						Genode::error("impure File_system request for root '",(char const *)root,"'");
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
				Genode::String<18> short_name(_name.string());
				if (label_buf[0]) {
					Genode::Session_label const label = prefixed_label(
						short_name, Genode::Session_label(label_buf));
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
			Service *service = nullptr;
			if (!(*args)) /* invalidated by filter_session_args */
				return 0;

			/* check for config file request */
			if ((service = _config_policy.resolve_session_request(service_name, args)))
				return service;

			if (strcmp("File_system", service_name) == 0)
			{
				Genode::Session_label label = label_from_args(args);
				char root[3] = { '\0' };

				if (!strcmp("ingest", label.last_element().string()))
					return &_fs_ingest_service;

				Genode::Arg_string::find_arg(args, "root").string(
					root, sizeof(root), "/");

				if (!(strcmp(root, "", sizeof(root))
				 && strcmp(root, "/", sizeof(root))))
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
				Genode::log(service_name, " request rejected from ", _name.string());
			return service;
		}

		void exit(int exit_value)
		{
			if (exit_value == 0 && _fs_ingest_service.finalize(_fs, _drv))
				Genode::log("\033[32m" "success: ", _name.string(), "\033[0m");
			else
				Genode::log("\033[31m" "failure: ", _name.string(), "\033[0m");

			/* TODO: write a store placeholder that marks a failure */

			Signal_transmitter(_exit_sigh).submit();
		}

		void resource_request(Parent::Resource_args const &args)
		{
			size_t ram_request =
				Arg_string::find_arg(args.string(), "ram_quota").ulong_value(0);

			if (!ram_request) return;

			ram_request = max(size_t(QUOTA_STEP), size_t(ram_request));

			size_t ram_avail = _env.ram().avail();
			if (ram_avail > (ram_request+QUOTA_RESERVE)) {
				_env.ram().transfer_quota(
					_resources.ram.cap(), ram_avail);
				_child.notify_resource_avail();
			} else {
				char arg_buf[32];
				snprintf(arg_buf, sizeof(arg_buf),
				         "ram_quota=%ld", ram_request);
				_env.parent().resource_request(arg_buf);
			}
		}

		void upgrade_ram()
		{
			size_t quota_avail = _env.ram().avail();

			if (quota_avail <= QUOTA_RESERVE)
				return;

			_env.ram().transfer_quota(
				_resources.ram.cap(), quota_avail - QUOTA_RESERVE);

			_child.notify_resource_avail();
		}
};

#endif
