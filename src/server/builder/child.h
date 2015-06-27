/*
 * \brief  Builder child
 * \author Emery Hemingway
 * \date   2015-03-13
 */

#ifndef _BUILDER__BUILDER_H_
#define _BUILDER__BUILDER_H_

/* Genode includes */
#include <base/env.h>
#include <base/child.h>
#include <base/printf.h>
#include <pd_session/connection.h>
#include <ram_session/connection.h>
#include <cpu_session/connection.h>
#include <rm_session/connection.h>
#include <file_system_session/file_system_session.h>
#include <init/child_policy.h>
#include <cli_monitor/ram.h>
#include <os/server.h>

/* Local imports */
#include "derivation.h"
#include "rom_policy.h"
#include "import_policy.h"

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
	enum { MINIMUM_QUOTA = 4096 };

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

		enum { CONFIG_SIZE = 4096 };

		char const                *_name;
		File_system::Session      &_fs;
		Derivation                &_drv;
		Signal_context_capability  _exit_sigh;
		Resources                 &_resources;
		Store_rom_policy           _binary_policy;
		Store_fs_policy            _fs_policy;
		Service_registry           _parent_services;

		struct Store_rom_registry
		{
			List<Store_rom_policy>  _policies;
			File_system::Session   &_fs;
			Genode::Rpc_entrypoint &_root_ep;
			Derivation             &_drv;


			Store_rom_registry(File_system::Session   &fs,
			                   Genode::Rpc_entrypoint &root_ep,
			                   Derivation             &drv)
			: _fs(fs), _root_ep(root_ep), _drv(drv) { }

			~Store_rom_registry()
			{
				for (Store_rom_policy *policy = _policies.first(); policy; policy = policy->next())
					destroy(env()->heap(), policy);
			}

			Service *resolve_session_request(const char *service_name,
			                                 const char *args)
			{
				char rom_name[Store_rom_policy::ROM_NAME_MAX_LEN];
				Genode::Arg_string::find_arg(args, "filename").string(
					rom_name, sizeof(rom_name), "");
				if (!*rom_name) return 0;

				for (Store_rom_policy *curr = _policies.first(); curr; curr = curr->next()) {
					if (!strcmp(rom_name, curr->name()))
						return curr->service();
				}

				for (Derivation::Env_pair *rom = _drv.rom(); rom; rom = rom->next()) {
					if (strcmp(rom->key, rom_name)) continue;
					if (rom->value[0] != '/') continue;
					PLOG("rom:%s=%s", rom->key, rom->value);
					/*
					 * TODO: check if rom->value is in inputs,
					 * otherwise purity is violated.
					 */
					try {
						Store_rom_policy *policy = new (env()->heap())
							Store_rom_policy(_fs, rom->key, rom->value, _root_ep);
						_policies.insert(policy);
						return policy->service();
					} catch (File_system::Lookup_failed) {
						PWRN("%s not found in store", rom->value);
						return 0;
					}
				}
				PWRN("%s not in env", rom_name);
				return 0;
			}

		} _rom_registry;

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
			_fs(fs),
			_drv(drv),
			_exit_sigh(exit_sigh),
			_resources(resources),
			_binary_policy(_fs, "binary", drv.builder(), ep.rpc_ep()),
			_fs_policy(ep),
			_rom_registry(_fs, ep.rpc_ep(), drv)
		{
			/*
			 * A whitelist of services to forward from our parent.
			 *
			 * TODO: enforce purity and do not forward ROM,
			 * all executables and libraries should be in the store.
			 */
			static char const *service_names[] = {
				"RM", "ROM", "LOG", "CAP", "SIGNAL", "PD", "CPU",
				"File_system", "Terminal", "Timer", 0
			};
			for (unsigned i = 0; service_names[i]; ++i)
				_parent_services.insert(
					new (env()->heap()) 
						Parent_service(service_names[i]));
		}

		Dataspace_capability binary() { return _binary_policy.dataspace(); }

		/*
		 * TODO: actually implement resource trading
		 */

		/****************************
		 ** Child policy interface **
		 ****************************/

		char const *name() const { return _name; }

		Service *resolve_session_request(const char *service_name,
		                                 const char *args)
		{
			Service *service = 0;

			if (!strcmp("ROM", service_name)) {
				/* check for binary request */
				if ((service = _binary_policy.resolve_session_request(service_name, args)))
					return service;

				/* Check for ROM defined in derivation */
				if ((service = _rom_registry.resolve_session_request(service_name, args)))
					return service;
			}

			if ((service = _fs_policy.resolve_session_request(service_name, args)))
				return service;

			/* get it from the parent if it is allowed */
			service = _parent_services.find(service_name);

			if (!service) {
				PERR("illegal %s request from %s", service_name, _name);
				exit(-1);
			}
			return service;
		}

		void filter_session_args(const char *service, char *args, size_t args_len)
		{
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

		void exit(int exit_value)
		{
			if (exit_value) {
				PERR("failure: %s", _name);
				// TODO: write a store placeholder that marks failure
			}
			else {
				_fs_policy.finalize(_fs, _drv);
				PINF("success: %s", _name);
			}

			Signal_transmitter(_exit_sigh).submit();
		}

};


class Builder::Child
{
	public:

		class Quota_exceeded : public Genode::Exception { };

	private:

		enum { ENTRYPOINT_STACK_SIZE  =   6*1024*sizeof(long) };

		Resources           _resources;
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
			//_entrypoint(&cap_session, ENTRYPOINT_STACK_SIZE, label),
			_child_policy(label, fs, _resources,  _entrypoint, drv, exit_sigh),
			_child(_child_policy.binary(),
			       _resources.pd.cap(),
			       _resources.ram.cap(),
			       _resources.cpu.cap(),
			       _resources.rm.cap(),
			      &_entrypoint.rpc_ep(),
			      &_child_policy)
		{ }
};

#endif