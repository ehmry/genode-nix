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
		File_system::Session  &_fs;
		Nix_store::Derivation  _drv { _name.string() };
		Inputs           const _inputs      { _fs, _drv,          *Genode::env()->heap() };
		Environment      const _environment { _fs, _drv, _inputs, *Genode::env()->heap() };

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
		// XXX: this far before fault

		Rom_connection             _binary_rom;
		Server::Entrypoint         _entrypoint;
		Signal_context_capability  _exit_sigh;
		/* XXX: use an allocator with a lifetime bound to the child */
		Ingest_service             _fs_ingest_service { _drv, _entrypoint, *Genode::env()->heap() };
		Filter_service             _fs_filter_service { _entrypoint, _inputs };
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
						PLOG("%s: forwarding impure service '%s' to parent", _name.string(), service_name);
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

		char const *name() const {
			return _name.string();
		}

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
				Genode::String<18> short_name(_name.string());
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
				PERR("%s request from %s rejected", service_name, _name.string());
			return service;
		}

		void exit(int exit_value)
		{
			if (exit_value == 0 && _fs_ingest_service.finalize(_fs, _drv))
				PINF("success: %s", _name.string());
			else
				PERR("failure: %s", _name.string());

			/* TODO: write a store placeholder that marks a failure */

			Signal_transmitter(_exit_sigh).submit();
		}

		void resource_request(Parent::Resource_args const &args)
		{
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
