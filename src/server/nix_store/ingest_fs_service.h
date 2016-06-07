/*
 * \brief  Service for file system inputs and outputs
 * \author Emery Hemingway
 * \date   2015-06-27
 */

/*
 * Copyright (C) 2015-2016 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _NIX_STORE__INGEST_FS_SERVICE_H_
#define _NIX_STORE__INGEST_FS_SERVICE_H_

/* Genode includes */
#include <file_system/util.h>
#include <file_system_session/capability.h>

/* Nix includes */
#include <nix_store/derivation.h>
#include <hash/sha256.h>
#include <hash/blake2s.h>

/* Local includes */
#include "ingest_component.h"

namespace Nix_store { class Ingest_service; }

class Nix_store::Ingest_service : public Genode::Service
{
	private:

		Genode::Env                    &_env;
		Ingest_component                _component;
		File_system::Session_capability _cap = _env.ep().manage(_component);

		static void hash_file(File_system::Session &fs, File_system::File_handle handle, Hash::Function &hash)
		{
			using namespace File_system;

			File_system::Session::Tx::Source &source = *fs.tx();
			/* try to round to the nearest multiple of the hash block size */
			size_t packet_size =
			((source.bulk_buffer_size() / hash.block_size()) * hash.block_size()) / 2;
			File_system::Packet_descriptor raw_packet = source.alloc_packet(packet_size);
			Packet_guard guard(source, raw_packet);

			while (packet_size > raw_packet.size())
				packet_size /= 2;

			/* Do a short read to align the packet stream with the block size */
			size_t n = packet_size;
			seek_off_t offset = 0;

			collect_acknowledgements(source);
			do {
				File_system::Packet_descriptor
					packet(raw_packet, handle, File_system::Packet_descriptor::READ, n, offset);

				source.submit_packet(packet);
				packet = source.get_acked_packet();
				n = packet.length();
				hash.update((uint8_t *)source.packet_content(packet), n);
				offset += n;
			} while (n);
		}

		/**
		 * TODO: recursive hashing
		 */
		static bool _verify(File_system::Session &fs, Hash::Function &hash, char const *hex, char const *filename)
		{
			/* I'm too lazy to write a decoder, so encode to hex and compare the strings */
			uint8_t buf[hash.size()*2+1];
			buf[hash.size()*2] = 0;

			static uint8_t const base16[] = {
				'0','1','2','3','4','5','6','7',
				'8','9','a','b','c','d','e','f'
			};

			File_system::Dir_handle root = fs.dir("/", false);
			File_system::File_handle handle;
			try { handle = fs.file(root, filename, File_system::READ_ONLY, false); }
			catch (...) {
				Genode::error("failed to open fixed output ", filename, " for verification", filename);
				throw ~0;
			}
			File_system::Handle_guard guard(fs, handle);

			hash_file(fs, handle, hash);
			hash.digest(buf, sizeof(buf));

			for (int i = hash.size()-1, j = hash.size()*2-1; i >= 0; --i) {
				buf[j--] = base16[buf[i] & 0x0F];
				buf[j--] = base16[buf[i] >> 4];
			}

			if (strcmp(hex, (char*)buf) == 0)
				return true;

			Genode::error("fixed output ", filename, " is invalid, wanted ", hex, ", got ", (char*)buf);
			return false;
		}

		/**
		 * Create a link from input addressed path
		 * to ouput addressed path
		 */
		void _link_from_inputs(File_system::Session    &fs,
		                       char              const *id,
		                       char              const *path)
		{
			while (*path == '/') ++path;
			using namespace File_system;

			char const *final_str = _component.ingest(id);
			if (!(final_str && *final_str)) {
				Genode::error(id, " not found at ingest session");
				throw ~0;
			}

			/* create symlink at real file system */
			Dir_handle root = fs.dir("/", false);
			Handle_guard root_guard(fs, root);

			Symlink_handle link = fs.symlink(root, path, true);
			File_system::write(fs, link, final_str, Genode::strlen(final_str));
			fs.close(link);
		}

		/**
		 * Finalize the derivation outputs at the ingest session and
		 * create symlinks from the derivation outputs to hashed outputs.
		 */
		bool _finalize(File_system::Session &fs, Nix_store::Derivation &drv)
		{
			using namespace File_system;

			unsigned outstanding = 0;

			/* run thru the outputs and finalize the paths */
			drv.outputs([&] (Aterm::Parser &parser) {
				Nix_store::Name id;
				Nix_store::Name path;
				Nix_store::Name algo;
				Nix_store::Name digest;

				parser.string(&id);

				char const *output = _component.ingest(id.string());
				if (!(output && *output)) {
					/* If output symlinks are missing, then failure is implicit. */
					Genode::error(id.string(), " not found at the ingest session");
					throw ~0;
				}

				parser.string(&path);
				parser.string(&algo);
				parser.string(&digest);

				try {
				if ((algo != "") || (digest != "")) {
					bool valid = false;
					if (algo == "sha256") {
						Hash::Sha256 hash;
						valid = _verify(fs, hash, digest.string(), output);
					} else if (algo == "blake2s") {
						Hash::Blake2s hash;
						valid = _verify(fs, hash, digest.string(), output);
					} else
						Genode::error("unknown hash algorithm ", algo.string());
					if (!valid) {
						Genode::error("fixed output ", id.string(), ":", path.string(), " is invalid");
						throw ~0;
					}
				}
				} catch (...) {
					Genode::error("caught an error verifying ", id.string(), ":", path.string());
					throw;
				}
				++outstanding;
			});

			/*
			 * Run thru again and link the input paths to content paths.
			 * This happens in two steps because it is important than
			 * links are only created if all outputs are valid.
			 */
			drv.outputs([&] (Aterm::Parser &parser) {
				Nix_store::Name id;
				Nix_store::Name path;

				parser.string(&id);
				parser.string(&path);

				_link_from_inputs(fs, id.string(), path.string());
				--outstanding;

				parser.string(/* Algo */); 
				parser.string(/* Hash */);
			});
			if (outstanding)
				Genode::error(outstanding, " outputs outstanding");
			return outstanding == 0;
		}

		void revoke_cap()
		{
			if (_cap.valid()) {
				_env.ep().dissolve(_component);
				_cap = File_system::Session_capability();
			}
		}

	public:

		/**
		 * Constructor
		 */
		Ingest_service(Nix_store::Derivation &drv, Genode::Env &env, Genode::Allocator &alloc)
		: Genode::Service("File_system"), _env(env), _component(env, alloc) { }

		~Ingest_service() { revoke_cap(); }

		bool finalize(File_system::Session &fs, Nix_store::Derivation &drv)
		{
			revoke_cap();
			try { return _finalize(fs, drv); } catch (...) { }
			return false;
		}


		/***********************
		 ** Service interface **
		 ***********************/

		Genode::Session_capability session(char const *args, Genode::Affinity const &) override
		{
			return _cap;
		}

		void upgrade(Genode::Session_capability, const char *args)
		{
			Genode::error("client is upgrading session, but don't know where to send it, ", args);
			//_env().parent().upgrade(_component.cap(), args);
		}

};

#endif
