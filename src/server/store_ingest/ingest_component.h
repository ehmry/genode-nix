/*
 * \brief  File system hashing proxy controller
 * \author Emery Hemingway
 * \date   2015-05-28
 */

/*
 * Copyright (C) 2015-2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _STORE_INGEST__INGEST_COMPONENT_H_
#define _STORE_INGEST__INGEST_COMPONENT_H_

/* Genode includes */
#include <store_ingest_session/store_ingest_session.h>

/* Local includes */
#include "fs_component.h"

namespace Store_ingest { class Ingest_component; }


class Store_ingest::Ingest_component :
	public Genode::Rpc_object<Store_ingest::Session>
{
	private:

		Fs_component                     _fs_session;
		File_system::Session_capability  _session_cap;
		Server::Entrypoint              &_ep;

	public:

		/****************************
		 ** Store ingest interface **
		 ****************************/

		Ingest_component(Server::Entrypoint &ep,
			             Genode::Allocator  &alloc,
			             size_t              ram_quota,
			             size_t              tx_buf_size)
		:
			_fs_session(ep, alloc, ram_quota, tx_buf_size),
			_session_cap(ep.manage(_fs_session)),
			_ep(ep)
		{ }

		~Ingest_component() { revoke_session(); }

		void upgrade_ram_quota(size_t ram_quota) {
			_fs_session.upgrade_ram_quota(ram_quota); }

		void expect(Name const &name) override {
			_fs_session.expect(name.string()); }

		File_system::Session_capability
			file_system_session(Session_args const &args) override
		{
			char const *args_str = args.string();

			if (!Arg_string::find_arg(args_str, "writeable").bool_value(true)) {
				/*
				 * Unwriteable sessions are rejected because
				 * the restriction may have been injected
				 * by an intermediate component.
				 */
				PERR("refusing read-only session");
				throw Root::Invalid_args();
			}

			return _session_cap;
		}

		void revoke_session() override
		{
			if (_session_cap.valid()) {
				_ep.dissolve(_fs_session);
				_session_cap = File_system::Session_capability();
			}
		}

		Name ingest(Name const &name) override
		{
			try { return _fs_session.ingest(name.string()); }
			catch (...) { PERR("error in %s", __func__); }
			return "";
		}

};


#endif
