/*
 * \brief  Line editor
 * \author Norman Feske
 * \author Emery Hemingway
 * \date   2015-06-11
 */

/*
 * Copyright (C) 2015 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _LINE_EDITOR_H_
#define _LINE_EDITOR_H_

/* Genode includes */
#include <terminal_session/terminal_session.h>
#include <base/snprintf.h>
#include <util/string.h>

class Line_editor_base
{
	private:

		Terminal::Session &_terminal;
		char        const *_prompt;
		size_t      const  _prompt_len;
		char              *_buf;
		size_t      const  _buf_size;
		unsigned           _cursor_pos;
		bool               _complete;

		/**
		 * State tracker for escape sequences within user input
		 *
		 * This tracker is used to decode special keys (e.g., cursor keys).
		 */
		struct Seq_tracker
		{
			enum State { INIT, GOT_ESC, GOT_FIRST } state;
			char normal, first, second;
			bool sequence_complete;

			Seq_tracker() : state(INIT), sequence_complete(false) { }

			void input(char c)
			{
				switch (state) {
				case INIT:
					if (c == 27)
						state = GOT_ESC;
					else
						normal = c;
					sequence_complete = false;
					break;

				case GOT_ESC:
					first = c;
					state = GOT_FIRST;
					break;

				case GOT_FIRST:
					second = c;
					state = INIT;
					sequence_complete = true;
					break;
				}
			}

			bool is_normal() const { return state == INIT && !sequence_complete; }

			bool _fn_complete(char match_first, char match_second) const
			{
				return sequence_complete
				    && first  == match_first
				    && second == match_second;
			}

			bool is_key_up()     const { return _fn_complete(91, 65); }
			bool is_key_down()   const { return _fn_complete(91, 66); }
			bool is_key_right()  const { return _fn_complete(91, 67); }
			bool is_key_left()   const { return _fn_complete(91, 68); }
			bool is_key_delete() const { return _fn_complete(91, 51); }
		};

		Seq_tracker _seq_tracker;

		void _write(char c) { _terminal.write(&c, sizeof(c)); }

		void _write(char const *s) { _terminal.write(s, Genode::strlen(s)); }

		void _clear_until_end_of_line() { _write("\e[K "); }

		void _move_cursor_to(unsigned pos)
		{
			char seq[16];
			Genode::snprintf(seq, sizeof(seq), "\e[%zdG", pos + _prompt_len);
			_write(seq);
		}

		void _delete_character()
		{
			Genode::strncpy(&_buf[_cursor_pos], &_buf[_cursor_pos+1], _buf_size);
			_move_cursor_to(_cursor_pos);
			_write(&_buf[_cursor_pos]);
			_clear_until_end_of_line();
			_move_cursor_to(_cursor_pos);
		}

		void _insert_character(char c)
		{
			/* insert regular character */
			if (_cursor_pos >= _buf_size -1)
				return;

			/* make room in the buffer */
			for (unsigned i = _buf_size -1; i > _cursor_pos; --i)
				_buf[i] = _buf[i - 1];
			_buf[_cursor_pos] = c;

			/* update terminal */
			_write(&_buf[_cursor_pos]);
			++_cursor_pos;
			_move_cursor_to(_cursor_pos);
		}

		void _fresh_prompt()
		{
			_write(_prompt);
			_write(_buf);
			_move_cursor_to(_cursor_pos);
		}

		void _handle_key()
		{
			enum { BACKSPACE       = 8,
			       TAB             = 9,
			       LINE_FEED       = 10,
			       CARRIAGE_RETURN = 13 };

			if (_seq_tracker.is_key_left()) {
				if (_cursor_pos > 0) {
					--_cursor_pos;
					_write(BACKSPACE);
				}
				return;
			}

			if (_seq_tracker.is_key_right()) {
				if (_cursor_pos < Genode::strlen(_buf)) {
					++_cursor_pos;
					_move_cursor_to(_cursor_pos);
				}
				return;
			}

			if (_seq_tracker.is_key_delete())
				_delete_character();

			if (!_seq_tracker.is_normal())
				return;

			char const c = _seq_tracker.normal;

			if (c == TAB) {
				perform_completion();
				return;
			}

			if (c == CARRIAGE_RETURN || c == LINE_FEED) {
				if (Genode::strlen(_buf) > 0) {
					_write(LINE_FEED);
					_complete = true;
				}
				return;
			}

			if (c == BACKSPACE) {
				if (_cursor_pos > 0) {
					--_cursor_pos;
					_delete_character();
				}
				return;
			}

			if (c == 126) /* What? */
				return;

			_insert_character(c);
		}

	virtual void perform_completion() = 0;

	public:

		Line_editor_base(Terminal::Session &terminal, char const *prompt, char *buf, size_t buf_size)
		:
			_terminal(terminal),
			_prompt(prompt), _prompt_len(Genode::strlen(prompt)),
			_buf(buf), _buf_size(buf_size)
		{ }

		/**
		 * Reset prompt to initial state after construction
		 */
		void reset()
		{
			*_buf = 0;
			_complete = false;
			_cursor_pos = 0;
			_seq_tracker = Seq_tracker();
			_fresh_prompt();
		}

		/**
		 * Supply a character of user input
		 */
		void submit_input(char c)
		{
			_seq_tracker.input(c);
			_handle_key();
		}

		/**
		 * Returns true if the editing is complete, i.e., the user pressed the
		 * return key.
		 */
		bool is_complete() const { return _complete; }

		/**
		 * Return cursor position
		 */
		unsigned cursor_pos() const { return _cursor_pos; }


};

#endif
