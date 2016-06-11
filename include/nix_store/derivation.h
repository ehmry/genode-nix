/*
 * \brief  Aterm formated derivation parsing
 * \author Emery Hemingway
 * \date   2015-03-13
 */

#ifndef _INCLUDE__NIX_STORE__DERIVATION_H_
#define _INCLUDE__NIX_STORE__DERIVATION_H_

/*Genode includes. */
#include <os/attached_rom_dataspace.h>
#include <util/list.h>
#include <util/string.h>
#include <util/token.h>
#include <file_system_session/file_system_session.h>

/* Nix store includes */
#include <nix_store/aterm_parser.h>
#include <nix_store/types.h>
#include <nix/attached_rom_dataspace.h>
#include <nix/types.h>

namespace Nix_store {

	/**
	 * Derivations are loaded from ROM rather than from the
	 * file system because loading is only done after a client
	 * has pushed or loaded a derviation, so there is a potential
	 * for caching. Its also makes for much less local code.
	 */
	class Derivation : Nix::Attached_rom_dataspace {

		private:

			Genode::String<File_system::MAX_PATH_LEN> _builder;
			Genode::String<32>                        _platform;

			Genode::size_t _len;

			char const *_outputs;
			char const *_inputs;
			char const *_sources;
			char const *_environment;

			inline Genode::size_t remain(char const *base) {
				return _len - (base - local_addr<char>()); }

		public:

			Derivation(Genode::Env &env, char const *name)
			:
				Nix::Attached_rom_dataspace(env, name),
				_len(Genode::strlen(local_addr<char>()))
			{
				Aterm::Parser parser(local_addr<char>(), _len);

				parser.constructor("Derive", [&] (Aterm::Parser &parser)
				{
					/*************
					 ** Outputs **
					 *************/
					_outputs = parser.list([] (Aterm::Parser &parser)
					{
						parser.tuple([] (Aterm::Parser &parser)
						{
							parser.string(); /* Id   */
							parser.string(); /* Path */
							parser.string(); /* Algo */
							parser.string(); /* Hash */
						});
					});

					/************
					 ** Inputs **
					 ************/
					_inputs = parser.list([] (Aterm::Parser &parser) {
						parser.tuple([] (Aterm::Parser &parser)
						{
							parser.string(); /* Derivation */
							parser.list([] (Aterm::Parser &parser) {
								parser.string(); /* Output */
							});
						});
					});

					/*************
					 ** Sources **
					 *************/
					_sources = parser.list([] (Aterm::Parser &parser) {
						 parser.string();
					});

					/**************
					 ** Platform **
					 **************/
					parser.string(&_platform);

					/********************
					 ** Builder binary **
					 ********************/
					parser.string(&_builder);

					/**********
					 ** Args **
					 **********/

					parser.list([name] (Aterm::Parser &parser) {
						Genode::log(name, " contains a command line argument", name);
						throw Invalid_derivation();
					});

					/*****************
					 ** Environment **
					 *****************/
					_environment = parser.list([] (Aterm::Parser &parser)
					{
						parser.tuple([] (Aterm::Parser &parser)
						{
							parser.string(); /* Key   */
							parser.string(); /* Value */
						});
					});
				});
			}

			/**
			 * Return the builder platform.
			 */
			char const *platform() const { return _platform.string(); }

			/**
			 * Return the builder executable filename.
			 */
			char const *builder() const { return _builder.string(); }

			template<typename FUNC>
			void outputs(FUNC const &func)
			{
				Aterm::Parser outputs(_outputs, remain(_outputs));
				outputs.list([&func] (Aterm::Parser &parser) { parser.tuple(func); });
			}

			template<typename FUNC>
			void inputs(FUNC const &func)
			{
				Aterm::Parser inputs(_inputs, remain(_inputs));
				inputs.list([&func] (Aterm::Parser &parser) { parser.tuple(func); });
			}

			template<typename FUNC>
			void sources(FUNC const &func)
			{
				Aterm::Parser sources(_sources, remain(_sources));
				sources.list(func);
			}

			template<typename FUNC>
			void environment(FUNC const &func)
			{
				Aterm::Parser environment(_environment, remain(_environment));
				environment.list([&func] (Aterm::Parser &parser) { parser.tuple(func); });
			}

			bool has_fixed_output()
			{
				Genode::String<2> path;
				Genode::String<2> algo;
				Genode::String<2> hash;

				unsigned known = 0, unknown = 0;

				outputs([&] (Aterm::Parser &parser) {
					parser.string(); /* id */
					parser.string(&path);
					parser.string(&algo);
					parser.string(&hash);

					if ((path != "") && (algo != "") && (hash != ""))
						++known;
					else
						++unknown;
				});

				return (known && (!unknown));
			}

	};

};

#endif
