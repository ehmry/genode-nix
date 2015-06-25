/* Genode includes */
#include <rom_session/rom_session.h>
#include <cap_session/connection.h>
#include <file_system_session/connection.h>
#include <file_system/util.h>
#include <util/arg_string.h>
#include <base/rpc_server.h>
#include <base/env.h>
#include <base/printf.h>

#ifndef _BUILDER__STORE_ROM_H_
#define _BUILDER__STORE_ROM_H_

namespace Builder { class Store_rom_policy; }

class Builder::Store_rom_policy : public Genode::List<Store_rom_policy>::Element
{
	public:
		enum { ROM_NAME_MAX_LEN = 32 };

	private:

		struct Local_rom_session_component : Genode::Rpc_object<Genode::Rom_session>
		{
			/**
			 * Dataspace exposed as ROM module to the client
			 */
			Genode::Ram_dataspace_capability _ds;

			/**
			 * Constructor
			 */
			Local_rom_session_component(File_system::Session &fs,
			                            char const           *file_path)
			{
				using namespace File_system;

				Genode::Path<MAX_PATH_LEN> dir_path(file_path);

				/*
				 * TODO: do this in Genode::Path
				 */

				char const *file_name = 0;
				for (char const *p = file_path; *p;)
					if (*p++ == '/') file_name = p;

				dir_path.strip_last_element();

				Dir_handle root = fs.dir(dir_path.base(), false);
				Handle_guard root_guard(fs, root);

				File_handle file = fs.file(root, file_name, READ_ONLY, false);
				Handle_guard file_guard(fs, file);

				file_size_t size = fs.status(file).size;

				_ds = env()->ram_session()->alloc(size);

				char *dst_addr = env()->rm_session()->attach(_ds);

				file_size_t n = read(fs, file, dst_addr, size);
				env()->rm_session()->detach(dst_addr);

				if (n != size) {
					PERR("Failed to read %s into dataspace", file_name);
					throw Exception();
				}
			}

			/**
			 * Destructor
			 */
			~Local_rom_session_component()
			{
				Genode::env()->ram_session()->free(_ds);
			}

			/***************************
			 ** ROM session interface **
			 ***************************/

			Genode::Rom_dataspace_capability dataspace()
			{
				Genode::Dataspace_capability ds_cap = _ds;
				return Genode::static_cap_cast<Genode::Rom_dataspace>(ds_cap);
			}

			void sigh(Genode::Signal_context_capability) { }

		} _local_rom_session;

		Genode::Rpc_entrypoint *_ep;
		Genode::Rom_session_capability _rom_session_cap;

		char _rom_name[ROM_NAME_MAX_LEN];

		struct Local_rom_service : public Genode::Service
		{
			Genode::Rom_session_capability _rom_cap;

			/**
			 * Constructor
			 *
			 * \param rom_cap  capability to return on session requests
			 * \param valid    true if local rom service is backed by a
			 *                 valid dataspace
			 */
			Local_rom_service(Genode::Rom_session_capability rom_cap)
			: Genode::Service("ROM"), _rom_cap(rom_cap) { }

			Genode::Session_capability session(char const * /*args*/,
			                                   Genode::Affinity const &) {
				return _rom_cap; }

			void upgrade(Genode::Session_capability, const char * /*args*/) { }
			void close(Genode::Session_capability) { }

		} _local_rom_service;

	public:

		/**
		 * Constructor
		 */
		Store_rom_policy(File_system::Session   &fs,
			             char const             *rom_name,
			             char const             *file_path,
		                 Genode::Rpc_entrypoint *ep)
		:
			_local_rom_session(fs, file_path), _ep(ep),
			_rom_session_cap(_ep->manage(&_local_rom_session)),
			_local_rom_service(_rom_session_cap)
		{
			Genode::strncpy(_rom_name, rom_name, sizeof(_rom_name));
		}

		char const *name() const { return _rom_name; }
		Genode::Service *service() { return &_local_rom_service; }

		/**
		 * Destructor
		 */
		~Store_rom_policy() { _ep->dissolve(&_local_rom_session); }

		Genode::Service *resolve_session_request(const char *service_name,
		                                         const char *args)
		{
			/* drop out if request refers to another file name */
			char buf[ROM_NAME_MAX_LEN];
			Genode::Arg_string::find_arg(args, "filename").string(buf, sizeof(buf), "");
			return !Genode::strcmp(buf, _rom_name) ? &_local_rom_service : 0;
		}

		Dataspace_capability dataspace() {
			return _local_rom_session.dataspace(); }
};

#endif