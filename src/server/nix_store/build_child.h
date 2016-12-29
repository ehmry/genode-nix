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
#include <timer_session/timer_session.h>
#include <os/session_requester.h>
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
	};

	class Child;

	typedef Genode::Registered<Genode::Parent_service> Parent_service;
}


class Nix_store::Child : public Genode::Child_policy
{
	private:

		Genode::Child_policy::Name const _name;

		Genode::Env           &_env;
		File_system::Session  &_fs;
		Nix_store::Derivation  _drv { _env, _name.string() };

		enum { ENTRYPOINT_STACK_SIZE = 12*1024 };
		Genode::Rpc_entrypoint _entrypoint;

		Genode::Session_label const _binary_label =
			Genode::prefixed_label(Genode::Session_label("store"),
			                       Genode::Session_label(_drv.builder()));

		Nix::Rom_connection _elf_rom { _env, _drv.builder() };
		Genode::Rom_dataspace_capability _elf_rom_ds = _elf_rom.dataspace();

		Genode::Parent_service _env_ram_service { _env, Genode::Ram_session::service_name() };
		Genode::Parent_service _env_cpu_service { _env, Genode::Cpu_session::service_name() };
		Genode::Parent_service _env_pd_service  { _env, Genode:: Pd_session::service_name() };
		Genode::Parent_service _env_log_service { _env, Genode::Log_session::service_name() };
		Genode::Parent_service _env_rom_service { _env, Genode::Rom_session::service_name() };
		Genode::Parent_service _env_timer_service { _env, Timer::Session::service_name() };


		Genode::Registry<Parent_service> _parent_services;

		Genode::Session_requester _session_requester;

		Genode::Child _child { _env.rm(), _entrypoint, *this };

		Signal_context_capability  _exit_sigh;
		Inputs      const _inputs      { _env, _child.heap(), _fs, _drv };
		Environment const _environment { _env, _child.heap(), _fs, _drv, _inputs };

		Genode::Attached_ram_dataspace _config_dataspace
			{ _env.ram(), _env.rm(), _drv.size() };

		Init::Child_policy_provide_rom_file _config_policy
			{ "config", _config_dataspace.cap(), &_entrypoint };

		Ingest_service  _fs_ingest_service { _drv, _env, _child.heap() };
		Filter_service  _fs_filter_service { _env, _inputs };
		Parent_service  _fs_parent_service { _parent_services, "File_system" };

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
			_entrypoint(&_env.pd(), ENTRYPOINT_STACK_SIZE, _name.string(),
			            false, Affinity::Location()),
			_session_requester(_entrypoint, _env.ram(), _env.rm()),
			_exit_sigh(exit_sigh)
		{
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
						new (_child.heap())
							Parent_service(_parent_services, service_name);
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

		Genode::Child_policy::Name name() const {
			return _name; }

		void filter_session_args(Service::Name const &service, char *args, size_t args_len)
		{
			using namespace Genode;

			/*
			 * Rewrite ROM requests to enforce purity
			 */
			if (service == "ROM") {

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
			else if (service == "File_system") {
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
			else if (service == "LOG") {
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

		Genode::Service &resolve_session_request(
			Genode::Service::Name const &service_name,
			Genode::Session_state::Args const &args) override
		{
			/* route environment session requests to the parent */
			Genode::Session_label const label(Genode::label_from_args(args.string()));
			if (label == name()) {
				if (service_name == Genode::Ram_session::service_name()) return _env_ram_service;
				if (service_name == Genode::Cpu_session::service_name()) return _env_cpu_service;
				if (service_name == Genode::Pd_session::service_name())  return _env_pd_service;
				if (service_name == Genode::Log_session::service_name()) return _env_log_service;
				if (service_name == Timer::Session::service_name()) return _env_timer_service;
			}

			/* route initial ROM requests (binary and linker) to the parent */
			if (service_name == Genode::Rom_session::service_name())
				if (label.last_element() == linker_name()) return _env_rom_service;

			Genode::Service *service = nullptr;

			/* check for config file request */
			service = _config_policy.resolve_session_request(
				service_name.string(), args.string());
			if (service)
				return *service;

			if (service_name == "File_system") {
				Genode::Session_label label =
					label_from_args(args.string());
				char root[3] = { '\0' };

				if (label.last_element() == "ingest")
					return _fs_ingest_service;

				Genode::Arg_string::find_arg(args.string(), "root").string(
					root, sizeof(root), "/");

				if (!(strcmp(root, "", sizeof(root))
				 && strcmp(root, "/", sizeof(root))))
					return _fs_filter_service;

				/*
				 * If there is a root argument then connect directly to 
				 * the store backend because the session is isolated by
				 * the root offset.
				 */
				return _fs_parent_service;
			}

			/* get it from the parent if it is allowed */
			_parent_services.for_each([&] (Parent_service &ps) {
				if (!service && (ps.name() == service_name))
					service = &ps; });

			if (!service)
				throw Genode::Parent::Service_denied();
			return *service;
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

		Ram_session           &ref_ram() override {
			return _env.ram(); }
		Ram_session_capability ref_ram_cap() const override {
			return _env.ram_session_cap();  }

		void init(Ram_session &session, Capability<Ram_session> cap) override
		{
			session.ref_account(_env.ram_session_cap());
			_env.ram().transfer_quota(cap, QUOTA_STEP);
		}

		void resource_request(Parent::Resource_args const &args) override
		{
			Genode::log("build child \"", name(), "\" requests resources: ", args.string());
			size_t ram_request =
				Arg_string::find_arg(args.string(), "ram_quota").ulong_value(0);

			if (!ram_request) return;

			ram_request = max(size_t(QUOTA_STEP), size_t(ram_request));

			size_t ram_avail = _env.ram().avail();
			if (ram_avail > (ram_request+QUOTA_RESERVE)) {
				_env.ram().transfer_quota(
					_child.ram_session_cap(), ram_request);
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

			size_t transfer = (quota_avail-QUOTA_RESERVE)-(quota_avail%QUOTA_STEP);

			_env.ram().transfer_quota(
				_child.ram_session_cap(), quota_avail - QUOTA_RESERVE);

			_child.notify_resource_avail();
		}
};

#endif
