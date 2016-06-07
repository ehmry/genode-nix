/*
 * \brief  Nix store component
 * \author Emery Hemingway
 * \date   2015-03-13
 */

/* Genode includes */
#include <base/component.h>

/* Nix includes */
#include <nix/attached_rom_dataspace.h>
#include <nix/file_system_connection.h>

/* Local includes */
#include "ingest_component.h"
#include "build_component.h"

/* Jitterentropy */
namespace Jitter { extern "C" {
#include <jitterentropy.h>
} }


/***************
 ** Component **
 ***************/

namespace Component {

	Genode::size_t stack_size() { return 32*1024*sizeof(long); }

	void construct(Genode::Env &env)
	{
		using namespace Genode;
		using namespace File_system;

		static Genode::Heap heap { &env.ram(), &env.rm() };

		/* used by the ingest component */
		Jitter::jent_entropy_init();

		/* create an empty file to be sure we have write access */
		Genode::Allocator_avl   fs_alloc { &heap };
		Nix::File_system_connection fs(env, fs_alloc);

		static char const *placeholder = ".nix_store";

		try {
			Dir_handle root_handle = fs.dir("/", false);
			Handle_guard guard(fs, root_handle);

			try { fs.unlink(root_handle, placeholder); }
			catch (Lookup_failed) { }

			/* XXX: write a nonce and read back from ROM */
			fs.close(fs.file(
				root_handle, placeholder, READ_WRITE, true));
		} catch (...) {
			Genode::error("access issues at backend");
			env.parent().exit(~0);
			throw;
		}

		/*
		 * verify that ROM requests are routed properly
		 *
		 * XXX: write a nonce to the placeholder
		 */
		try { Nix::Rom_connection rom(env, placeholder); }
		catch (...) {
			Genode::error("failed to aquire store ROM");
			env.parent().exit(~0);
			throw;
		}

		static Sliced_heap sliced_heap { &env.ram(), &env.rm() };

		static Nix_store::Ingest_root ingest_root { env, sliced_heap, heap };
		static Nix_store::Build_root   build_root { env, sliced_heap, heap };
	}

}