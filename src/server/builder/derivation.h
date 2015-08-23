/*
 * \brief  Aterm formated derivation parsing
 * \author Emery Hemingway
 * \date   2015-03-13
 */

#ifndef _BUILDER__DERIVATION_H_
#define _BUILDER__DERIVATION_H_

#include <builder_session/builder_session.h>

/*Genode includes. */
#include <base/env.h>
#include <util/list.h>
#include <util/string.h>
#include <util/token.h>
#include <file_system_session/file_system_session.h>
#include <nix/types.h>

#include "aterm_parser.h"


using namespace Aterm;


namespace Builder {

	typedef Genode::List<Aterm::Parser::String> String_list;

	class Derivation {

		public:

			enum { MAX_DERIVATION_SIZE = 4096 };

			struct Output : Genode::List<Output>::Element
			{
				Parser::String id;
				Parser::String path;
				Parser::String algo;
				Parser::String hash;
			};

			class Input : public Genode::List<Input>::Element
			{
				private:

					Genode::Allocator *_alloc;

				public:
					Nix::Name path;
					String_list  ids;

					Input(Genode::Allocator *alloc) : _alloc(alloc) { };

					~Input()
					{
						while (ids.first()) {
							Parser::String *id = ids.first();
							ids.remove(id);
							destroy(_alloc, id);
						}
					}
			};

			struct Env_pair : Genode::List<Env_pair>::Element
			{
				char   key[File_system::MAX_NAME_LEN];
				char value[File_system::MAX_PATH_LEN];

				Env_pair(Parser::String k, Parser::String v)
				{
					k.value(  key, sizeof(key)  );
					v.value(value, sizeof(value));
				}
			};

		private:

			/* Alloc a dataspace for this so it fits on a singe page? */
			char                   _content[MAX_DERIVATION_SIZE];
			Genode::Allocator     *_alloc;
			Genode::List<Output>   _outputs;
			Genode::List<Input>    _inputs;
			String_list            _sources;
			char                   _platform[32];
			char                   _builder[File_system::MAX_PATH_LEN];
			Genode::List<Env_pair>     _env;

		public:

			Derivation(Genode::Allocator    *alloc,
			           File_system::Session &store_fs,
			           char const           *name)
			: _alloc(alloc)
			{
				using namespace File_system;

				File_handle file;
				try {
					Dir_handle root = store_fs.dir("/", false);
					Handle_guard root_guard(store_fs, root);

					file = store_fs.file(root, name, READ_ONLY, false);
				} catch (File_system::Lookup_failed) {
					PERR("derivation not found in store for %s", name);
					throw Invalid_derivation();
				}
				Handle_guard file_guard(store_fs, file);

				file_size_t file_size = store_fs.status(file).size;
				if (file_size > sizeof(_content)) {
					PERR("derivation file is %llu bytes, only %lu bytes available", file_size, sizeof(_content));
					throw Invalid_derivation();
				}

				size_t n = read(store_fs, file, _content, sizeof(_content), 0);
				if (n != file_size) {
					PERR("I/O error reading derivation");
					throw Exception();
				}

				Aterm::Parser parser(_content, n);

				parser.constructor("Derive", [&]
				{
					/*************
					 ** Outputs **
					 *************/
					parser.list([&]
					{
						parser.tuple([&]
						{
							Output *o = new (_alloc) Output;

							o->id   = parser.string();
							o->path = parser.string();
							o->algo = parser.string();
							o->hash = parser.string();

							_outputs.insert(o);
						});
					});

					/************
					 ** Inputs **
					 ************/
					parser.list([&]
					{
						parser.tuple([&]
						{
							Input *i = new (_alloc) Input(_alloc);
							Parser::String p = parser.string();
							i->path = Nix::Name(p.base(), p.len());
							parser.list([&] {
								i->ids.insert(new (_alloc)
									Parser::String(parser.string()));
							});
							_inputs.insert(i);
						});
					});

					/*************
					 ** Sources **
					 *************/
					parser.list([&]
					{
						_sources.insert(new (_alloc)
							Parser::String(parser.string()));
					});

					/**************
					 ** Platform **
					 **************/
					parser.string().value(
						_platform, sizeof(_platform));

					/********************
					 ** Builder binary **
					 ********************/
					parser.string().value(
						_builder, sizeof(_builder));

					/**********
					 ** Args **
					 **********/

					parser.list([&parser]
					{
						PERR("ignoring command line argument %s", parser.string().base());
					});

					/*************************
					 ** Roms (normally env) **
					 *************************/
					parser.list([&]
					{
						parser.tuple([&]
						{
							Env_pair *p = new (_alloc)
								Env_pair(parser.string(), parser.string());

							_env.insert(p);
						});
					});
				});
			}

			template <typename T>
			void clear(Genode::List<T> list)
			{
				while (list.first()) {
					T *e = list.first();
					list.remove(e);
					destroy(_alloc, e);
				}
			}

			~Derivation()
			{
				clear<Output>(_outputs);

				while (_inputs.first()) {
					Input *i = _inputs.first();
					clear<Parser::String>(i->ids);
					_inputs.remove(i);
					destroy(_alloc, i);
				}

				clear<Parser::String>(_sources);
				clear<Env_pair>(_env);
			}

			/**
			 * Return the front of the outputs linked list.
			 */
			Output *output() { return _outputs.first(); }

			/**
			 * Return the front of the inputs linked list.
			 */
			Input *input() { return _inputs.first(); }

			/**
			 * Return the front of the sources linked list.
			 */
			Parser::String *source() { return _sources.first(); }

			/**
			 * Return the builder platform.
			 */
			char const *platform() const { return _platform; }

			/**
			 * Return the builder executable filename.
			 */
			char const *builder() const { return _builder; }

			/**
			 * Return the store path of the first output.
			 */
			void path(char *dest, size_t len)
			{
				Output *o = _outputs.first();
				if (!o) throw Invalid_derivation();
				o->path.value(dest, len);
			}

			/**
			 * Return a pointer to the front of the
			 * environment linked list.
			 */
			Env_pair *env() { return _env.first(); }

	};

};

#endif
