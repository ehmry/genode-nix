/*
 * \brief  Policy and service for directing Terminal to LOG
 * \author Emery Hemingway
 * \date   2015-08-14
 */

/*
 * Copyright (C) 2015 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

/* Genode includes */
#include <terminal_session/capability.h>
#include <log_session/connection.h>
#include <os/attached_ram_dataspace.h>
#include <base/connection.h>
#include <util/arg_string.h>
#include <base/rpc_server.h>
#include <base/env.h>

#ifndef _BUILDER__TERMINAL_H_
#define _BUILDER__TERMINAL_H_

namespace Builder {

	class Terminal_policy;
	class Buffered_output;
}


class Builder::Terminal_policy
{
	private:

		class Local_terminal_component
		: public Rpc_object<Terminal::Session, Local_terminal_component>
		{
			private:

				enum { IO_BUF_SIZE = 4096 };

				/**
				 * Buffer shared with the terminal client
				 */
				Genode::Attached_ram_dataspace _io_buffer;

				/**
				 * Utility for the buffered output of small successive write operations
				 */
				class Buffered_output
				{
					private:

						enum { SIZE = Genode::Log_session::String::MAX_SIZE };

						typedef Genode::size_t size_t;

						char _buf[SIZE + 1 /* room for null-termination */ ];

						/* index of next character within '_buf' to write */
						unsigned _index = 0;

						Genode::Log_session &_log;

						void _flush()
						{
							/* append null termination */
							_buf[_index] = 0;

							/* flush buffered characters to LOG */
							_log.write(_buf);

							/* reset */
							_index = 0;
						}

						size_t _remaining_capacity() const { return SIZE - _index; }

					public:

						Buffered_output(Genode::Log_session &log)
						: _log(log) { }

						size_t write(char const *src, size_t num_bytes)
						{
							size_t const consume_bytes = Genode::min(num_bytes,
			                                         _remaining_capacity());

							for (unsigned i = 0; i < consume_bytes; i++) {
								char const c = src[i];
								_buf[_index++] = c;
								if (c == '\n')
									_flush();
							}

							if (_remaining_capacity() == 0)
								_flush();

							return consume_bytes;
						}
				} _output;

			public:

				/**
				 * Constructor
				 */
				Local_terminal_component(Log_session &log)
				:
					_io_buffer(env()->ram_session(), IO_BUF_SIZE),
					_output(log)
				{ }

				/********************************
				 ** Terminal session interface **
				 ********************************/

				Size size()  { return Size(0,0); }
				bool avail() { return false; }

				Genode::size_t _read(Genode::size_t) { return 0; }

				void _write(Genode::size_t  num_bytes)
				{
					num_bytes = Genode::min(num_bytes, _io_buffer.size());

					char const *src = _io_buffer.local_addr<char>();

					for (size_t written_bytes = 0; written_bytes < num_bytes; )
						written_bytes += _output.write(src + written_bytes,
						                               num_bytes - written_bytes);
				}

				Dataspace_capability _dataspace() { return _io_buffer.cap(); }

				void connected_sigh(Genode::Signal_context_capability cap) {
					Genode::Signal_transmitter(cap).submit(); }

				void read_avail_sigh(Genode::Signal_context_capability cap) {
					return; }

				size_t read(void *buf, size_t) { return 0; }
				size_t write(void const *buf, size_t) { return 0; }

		} _local_component;

		Genode::Rpc_entrypoint       &_ep;
		Terminal::Session_capability  _session_cap;

		struct Local_service : public Genode::Service
		{
			Terminal::Session_capability _cap;

			/**
			 * Constructor
			 *
			 * \param terminal_cap  capability to return on session requests
			 */
			Local_service(Terminal::Session_capability terminal_cap)
			: Genode::Service("Terminal"), _cap(terminal_cap) { }

			Genode::Session_capability session(char const *,
			                                   Genode::Affinity const &) {
				return _cap; }

			void upgrade(Genode::Session_capability, const char *) { }
			void close(Genode::Session_capability) { }

		} _local_service;

	public:

		/**
		 * Constructor
		 */
		Terminal_policy(Log_session &log, Genode::Rpc_entrypoint &ep)
		:
			_local_component(log),
			_ep(ep),
			_session_cap(_ep.manage(&_local_component)),
			_local_service(_session_cap)
		{ }

		/**
		 * Destructor
		 */
		~Terminal_policy() { _ep.dissolve(&_local_component); }

		Genode::Service *service() { return &_local_service; }
};

#endif