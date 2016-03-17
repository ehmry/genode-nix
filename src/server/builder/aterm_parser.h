/*
 * \brief  Simple Aterm parser
 * \author Emery Hemingway
 * \date   2015-03-13
 */

#ifndef _BUILDER__ATERM_PARSER_H_
#define _BUILDER__ATERM_PARSER_H_

#include <util/string.h>
#include <base/exception.h>
#include <base/stdint.h>

namespace Aterm {

	using namespace Genode;

	class Parser;
}

class Aterm::Parser
{
	public:

		struct Exception : Genode::Exception { };
		struct Malformed_element : Exception { };
		struct Wrong_element     : Exception { };
		struct End_of_term       : Exception { };
		struct Bad_logic         : Exception { };
		struct Overflow          : Exception { };

	private:

		enum { MAX_DEPTH = 32 };

		enum State : char { NULL = 0, TUPLE = ')', LIST = ']' };

		char const *_pos;
		size_t      _len;
		int         _depth;
		State       _state[MAX_DEPTH];

		void push(State state)
		{
			if (_depth == MAX_DEPTH) throw Overflow();

			memcpy(&_state[1], &_state[0], sizeof(State)*_depth);
			++_depth; _state[0] = state;
		}

		void pop()
		{
			if (_depth == 1) throw Bad_logic();
			--_depth;
			memcpy(&_state[0], &_state[1], sizeof(State)*_depth);
			advance();
		}

		void advance() { ++_pos; --_len; }

		void check_end()
		{
			if (*_pos == ',')        return advance();
			if (_depth == 1)         return;
			if (*_pos == _state[0])  return pop();

			throw Malformed_element();
		}

	public:

		/**
		 * Constructor
		 */
		Parser(char const *start, size_t len)
		: _pos(start), _len(len), _depth(1) { _state[0] = NULL; };

		/*
		 * The following functions apply a function to a term and
		 * return the base address of the term. The return value is
		 * for the sake of deferred processing.
		 */

		template <typename FUNC>
		char const *constructor(char const *name, FUNC const &func)
		{
			size_t len = Genode::strlen(name);
			if (len > _len || Genode::strcmp(_pos, name, len))
				throw Wrong_element();
			char const *base = _pos;

			_pos += len;
			_len -= len;

			tuple(func);
			return base;
		}

		template<typename FUNC>
		char const *tuple(FUNC const &func)
		{
			if (!_pos || !_len) throw End_of_term();
			if (*_pos != '(') throw Wrong_element();
			char const *base = _pos;

			advance();
			push(TUPLE);
			func(*this);
			check_end();
			return base;
		}

		template<typename FUNC>
		char const *list(FUNC const &func)
		{
			if (!_pos || !_len) throw End_of_term();
			if (*_pos != '[') throw Wrong_element();
			char const *base = _pos;

			advance();
			if (*_pos == ']') {
				advance();
				check_end();
				return base;
			}

			int start_depth = _depth;
			push(LIST);
			while (_depth > start_depth)
				func(*this);
			check_end();
			return base;
		}

		void string()
		{
			if (*_pos != '"') throw Wrong_element();

			++_pos;
			++_len;

			while (_len  && (*_pos == '\\' || *_pos != '"')) {
				++_pos;
				++_len;
			}
			++_pos;
			++_len;

			check_end();
		}

		template <size_t N>
		void string(Genode::String<N> *out)
		{
			if (*_pos != '"') throw Wrong_element();

			size_t len = 0;
			char const *pos = _pos + 1;

			while (pos[len] == '\\' || pos[len] != '"') {
				++len;
				if (_len - len == 0) throw Malformed_element();
			}
			_len -= 2 + len;
			_pos += 2 + len;

			check_end();
			*out = Genode::String<N>(pos, len);
		}

		long integer()
		{
			long i;

			while (*_pos >= '0' && *_pos <= '9') {
				i = i*10 + long(*_pos);
				_pos++; _len--;
			}
			check_end();
			return i;
		}

};

#endif