/*
 * \brief  Nix_store utilities
 * \author Emery Hemingway
 * \date   2016-06-24
 */

/*
 * Copyright (C) 2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _NIX_STORE__UTIL_H_
#define _NIX_STORE__UTIL_H_

/* Genode includes */
#include <file_system_session/file_system_session.h>
#include <os/path.h>


namespace Nix_store {

	typedef Genode::Path<Nix_store::MAX_PATH_LEN> Object_path;

	/**
	 * \throw lookup failed
	 */
	Object_path dereference(File_system::Session &fs, char const *name)
	{
		using namespace File_system;

		Object_path path(name);

		for (;;) {
			Node_handle node = fs.node(path.base());
			Handle_guard node_guard(fs, node);

			switch (fs.status(node).mode) {
			case Status::MODE_FILE:
			case Status::MODE_DIRECTORY:
				return path;
			case Status::MODE_SYMLINK: {
				Symlink_handle link = fs.symlink(
					ROOT_HANDLE, path.base()+1, false);
				Handle_guard link_guard(fs, link);

				File_system::Session::Tx::Source &source = *fs.tx();
				while (source.ack_avail())
					source.release_packet(source.get_acked_packet());

				File_system::Packet_descriptor packet(
					source.alloc_packet(Object_path::capacity()),
					link, File_system::Packet_descriptor::READ,
					Object_path::capacity(), 0);

				Genode::memset(source.packet_content(packet), 0x00, packet.size());

				source.submit_packet(packet);
				packet = source.get_acked_packet();
				char *p = source.packet_content(packet);
				p[min(packet.length(), packet.size()-1)] = '\0';
				path.import(p);
				source.release_packet(packet);
			}}
		}
		return path;
	}

}

#endif /* _NIX_STORE__UTIL_H_ */