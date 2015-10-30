#ifndef _NIX_REPL_H_
#define _NIX_REPL_H_

/* Genode includes */
#include <vfs/file_system_factory.h>
#include <vfs/dir_file_system.h>
#include <terminal_session/terminal_session.h>
#include <os/config.h>

/* Local includes */
//#include "job.h"
#include "line_editor.h"

/* Nix includes */
#include <shared.hh>
#include <eval.hh>
#include <eval-inline.hh>
#include <store-api.hh>
#include <common-opts.hh>
#include <get-drvs.hh>
#include <derivations.hh>
#include <affinity.hh>

#define ESC_RED "\033[31m"
#define ESC_GRE "\033[32m"
#define ESC_YEL "\033[33m"
#define ESC_BLU "\033[34m"
#define ESC_BACK_BLU "\033[44m"
#define ESC_MAG "\033[35m"
#define ESC_CYA "\033[36m"
#define ESC_WHI "\033[37m"
#define ESC_END "\033[0m"

namespace nix {

using namespace std;

string removeWhitespace(string s)
{
    s = chomp(s);
    size_t n = s.find_first_not_of(" \n\r\t");
    if (n != string::npos) s = string(s, n);
    return s;
}


struct NixRepl : Line_editor_base
{
    char              *_buf;
    size_t      const  _buf_size;

    Vfs::Dir_file_system vfs;
    nix::Vfs_root        vfs_root;
    nix::Store           store;
    nix::EvalState       state;

    Strings loadedFiles;

    const static int envSize = 32768;
    StaticEnv staticEnv;
    Env * env;
    int displ;
    StringSet varNames;

    StringSet completions;
    StringSet::iterator curCompletion;

    NixRepl(Terminal::Session &terminal,
            const char        *prompt,
            char              *buf,
            size_t             buf_size);

    void build(string arg);
    void completePrefix(string prefix);
    void perform_completion();
    bool processLine(string line);
    void loadFile(const Path & path);
    void initEnv();
    void reloadFiles();
    void addAttrsToScope(Value & attrs);
    void addVarToScope(const Symbol & name, Value & v);
    Expr * parseString(string s);
    void evalString(string s, Value & v);
    void evaluate();

    typedef set<Value *> ValuesSeen;
    void printValue(Value & v, unsigned int maxDepth);
    void printValue(Value & v, unsigned int maxDepth, ValuesSeen & seen);

	/**
	 * A terminal output buffer.
	 */
	struct Terminal_buffer
	{
		char               buf[2048];
		unsigned           pos;
		Terminal::Session &terminal;

		Terminal_buffer(Terminal::Session &term)
		: pos(0), terminal(term)
		{ }

		void flush()
		{
			terminal.write(buf, pos);
			pos = 0;
		}

		void write(char c)
		{
			if (pos > sizeof(buf)-2) flush();

			buf[pos++] = c;
		}

		void write(char const *s)
		{
			size_t len = Genode::strlen(s);

			while (len) {
				size_t n = min(sizeof(buf) - pos, len);
				terminal.write(s, n);
				s += n;
				len -= n;
			}
		}

		void write(std::string str)
		{
			size_t len = str.size();

			for (;;) {
				size_t n = min(sizeof(buf) - pos, len);
				terminal.write(str.c_str(), n);
				len -= n;
				if (!len) break;
				str = str.substr(n, len-n);
			}
		}

	} _term;

};


NixRepl::NixRepl(Terminal::Session &terminal,
                 const char        *prompt,
                 char              *buf,
                 size_t             buf_size)
    : Line_editor_base(terminal, prompt, buf, buf_size)
    , _buf(buf)
    , _buf_size(buf_size)
    , vfs(Genode::config()->xml_node().sub_node("nix").sub_node("vfs"),
          Vfs::global_file_system_factory())
    , vfs_root(vfs)
    , store(vfs_root)
    , state(vfs_root, store, Genode::config()->xml_node().sub_node("nix"))
    , staticEnv(false, &state.staticBaseEnv)
    , _term(terminal)
{
    initEnv();
}


void NixRepl::completePrefix(string prefix)
{
    completions.clear();

    size_t dot = prefix.rfind('.');

    if (dot == string::npos) {
        /* This is a variable name; look it up in the current scope. */
        StringSet::iterator i = varNames.lower_bound(prefix);
        while (i != varNames.end()) {
            if (string(*i, 0, prefix.size()) != prefix) break;
            completions.insert(*i);
            i++;
        }
    } else {
        try {
            /* This is an expression that should evaluate to an
               attribute set.  Evaluate it to get the names of the
               attributes. */
            string expr(prefix, 0, dot);
            string prefix2 = string(prefix, dot + 1);

            Expr * e = parseString(expr);
            Value v;
            e->eval(state, *env, v);
            state.forceAttrs(v);

            foreach (Bindings::iterator, i, *v.attrs) {
                string name = i->name;
                if (string(name, 0, prefix2.size()) != prefix2) continue;
                completions.insert(expr + "." + name);
            }

        } catch (ParseError & e) {
            // Quietly ignore parse errors.
        } catch (EvalError & e) {
            // Quietly ignore evaluation errors.
        } catch (UndefinedVarError & e) {
            // Quietly ignore undefined variable errors.
        }
    }
}


bool isVarName(const string & s)
{
    // FIXME: not quite correct.
    foreach (string::const_iterator, i, s)
        if (!((*i >= 'a' && *i <= 'z') ||
              (*i >= 'A' && *i <= 'Z') ||
              (*i >= '0' && *i <= '9') ||
              *i == '_' || *i == '\''))
            return false;
    return true;
}

void NixRepl::build(string arg)
{
   Value v;
    evalString(arg, v);
    DrvInfo drvInfo(state);
    if (!getDerivation(state, v, drvInfo, false))
        throw Error("expression does not evaluation to a derivation, so I can't build it");
    Path drvPath = drvInfo.queryDrvPath();

    PathSet paths { drvPath };
    try {
        store.buildPaths(paths);
    } catch (Builder::Invalid_derivation) {
        _term.write("Builder reported that ");
        _term.write(drvPath);
        _term.write(" was invalid.\n");
    } catch (Builder::Missing_dependency) {
        _term.write("Builder reported that ");
        _term.write(drvPath);
        _term.write(" has missing dependencies, a Nix library should have taken care of that.\n");
    }
}


bool NixRepl::processLine(string line)
{
    string command, arg;

    if (line[0] == ':') {
        size_t p = line.find(' ');
        command = string(line, 0, p);
        if (p != string::npos) arg = removeWhitespace(string(line, p));
    } else {
        arg = line;
    }

    if (command == ":?" || command == ":help") {
        _term.write("The following commands are available:\n"
		            "\n"
		            "  <expr>        Evaluate and print expression\n"
		            "  <x> = <expr>  Bind expression to variable\n"
		            "  :a <expr>     Add attributes from resulting set to scope\n"
		            "  :b <expr>     Build derivation\n"
		            "  :l <path>     Load Nix expression and add it to scope\n"
		            "  :p <expr>     Evaluate and print expression recursively\n"
		            "  :q            Exit nix-repl\n"
		            "  :r            Reload all files\n"
		            "  :t <expr>     Describe result of evaluation\n");
    }

    else if (command == ":a" || command == ":add") {
        Value v;
        evalString(arg, v);
        addAttrsToScope(v);
    }

    else if (command == ":l" || command == ":load") {
        state.resetFileCache();
        loadFile(arg);
    }

    else if (command == ":r" || command == ":reload") {
        state.resetFileCache();
        reloadFiles();
    }

    else if (command == ":t") {
        Value v;
        evalString(arg, v);
        _term.write(showType(v));
    }

    else if (command == ":b")
		build(arg);

    else if (command == ":p" || command == ":print") {
        Value v;
        evalString(arg, v);
        printValue(v, 1000000000);
    }

    else if (command == ":q" || command == ":quit")
        return false;

    else if (command != "")
        throw Error(format("unknown command ‘%1%’") % command);

    else {
        size_t p = line.find('=');
        string name;
        if (p != string::npos &&
            p < line.size() &&
            line[p + 1] != '=' &&
            isVarName(name = removeWhitespace(string(line, 0, p))))
        {
            Expr * e = parseString(string(line, p + 1));
            Value & v(*state.allocValue());
            v.type = tThunk;
            v.thunk.env = env;
            v.thunk.expr = e;
            addVarToScope(state.symbols.create(name), v);
        } else {
            Value v;
            evalString(line, v);
            printValue(v, 1);
        }
    }
    return true;
}


void NixRepl::loadFile(const Path & path)
{
    loadedFiles.remove(path);
    loadedFiles.push_back(path);
    Value v, v2;
    state.evalFile(lookupFileArg(state, path), v);
    Bindings & bindings(*state.allocBindings(0));
    state.autoCallFunction(bindings, v, v2);
    addAttrsToScope(v2);
}


void NixRepl::initEnv()
{
    env = &state.allocEnv(envSize);
    env->up = &state.baseEnv;
    displ = 0;
    staticEnv.vars.clear();

    varNames.clear();
    foreach (StaticEnv::Vars::iterator, i, state.staticBaseEnv.vars)
        varNames.insert(i->first);

    char filename[1024];
    Genode::config()->xml_node().for_each_sub_node("load", [&] (Genode::Xml_node node) {
        if (node.has_attribute("file")) {
            node.attribute("file").value(filename, sizeof(filename));
            loadFile(Path(filename));
        }
    });

    reset();
}


void NixRepl::reloadFiles()
{
    initEnv();

    Strings old = loadedFiles;
    loadedFiles.clear();

    foreach (Strings::iterator, i, old) {
        _term.write((format("Loading ‘%1%’...\n") % *i).str());
        loadFile(*i);
    }
}


void NixRepl::addAttrsToScope(Value & attrs)
{
    state.forceAttrs(attrs);
    foreach (Bindings::iterator, i, *attrs.attrs)
        addVarToScope(i->name, *i->value);
    _term.write((format("Added %1% variables.\n") % attrs.attrs->size()).str());
}


void NixRepl::addVarToScope(const Symbol & name, Value & v)
{
    if (displ >= envSize)
        throw Error("environment full; cannot add more variables");
    staticEnv.vars[name] = displ;
    env->values[displ++] = &v;
    varNames.insert((string) name);
}


Expr * NixRepl::parseString(string s)
{
    Expr * e = state.parseExprFromString(s, "/", staticEnv);
    return e;
}


void NixRepl::evalString(string s, Value & v)
{
    Expr * e = parseString(s);
    e->eval(state, *env, v);
    state.forceValue(v);
}


void NixRepl::printValue(Value & v, unsigned int maxDepth)
{
    ValuesSeen seen;
    return printValue(v, maxDepth, seen);
}


// FIXME: lot of cut&paste from Nix's eval.cc.
void NixRepl::printValue(Value & v, unsigned int maxDepth, ValuesSeen & seen)
{
    state.forceValue(v);

    switch (v.type) {

    case tInt:
        _term.write(ESC_CYA);
        {
        	char buf[16];
        	snprintf(buf, sizeof(buf), "%d", v.integer);
	        _term.write(buf);
        }
        _term.write(ESC_END);
        break;

    case tBool:
        _term.write(ESC_CYA);
        _term.write(v.boolean ? "true" : "false");
        _term.write(ESC_END);
        break;

    case tString:
        _term.write(ESC_YEL "\"");
		// TODO: check these escapes
        _term.write(v.string.s);
        _term.write("\"" ESC_END);
        break;

    case tPath:
        _term.write(ESC_GRE);
        _term.write(v.path); // !!! escaping?
        _term.write(ESC_END);
        break;

    case tNull:
        _term.write(ESC_CYA "null" ESC_END);
        break;

    case tAttrs: {
        seen.insert(&v);

        bool isDrv = state.isDerivation(v);

        if (isDrv) {
            _term.write("<<derivation ");
            Bindings::iterator i = v.attrs->find(state.sDrvPath);
            PathSet context;
            Path drvPath = i != v.attrs->end() ? state.coerceToPath(*i->pos, *i->value, context) : "???";
            _term.write(drvPath);
			_term.write(">>");
        }

        else if (maxDepth > 0) {
            _term.write("{ ");

            typedef std::map<string, Value *> Sorted;
            Sorted sorted;
            foreach (Bindings::iterator, i, *v.attrs)
                sorted[i->name] = i->value;

            /* If this is a derivation, then don't show the
               self-references ("all", "out", etc.). */
            StringSet hidden;
            if (isDrv) {
                hidden.insert("all");
                Bindings::iterator i = v.attrs->find(state.sOutputs);
                if (i == v.attrs->end())
                    hidden.insert("out");
                else {
                    state.forceList(*i->value);
                    for (unsigned int j = 0; j < i->value->list.length; ++j)
                        hidden.insert(state.forceStringNoCtx(*i->value->list.elems[j]));
                }
            }

            foreach (Sorted::iterator, i, sorted) {
                _term.write(i->first);
				_term.write(" = ");
                if (hidden.find(i->first) != hidden.end())
                    _term.write("<<...>>");
                else if (seen.find(i->second) != seen.end())
                    _term.write("<<repeated>>");
                else
                    try {
                        printValue(*i->second, maxDepth - 1, seen);
                    } catch (AssertionError & e) {
                        _term.write(ESC_RED "<<error: ");
						_term.write(e.msg());
						_term.write(">>" ESC_END);
                    }
                _term.write("; ");
            }

            _term.write("}");
        } else
            _term.write("{ ... }");
        break;
    }

    case tList:
        seen.insert(&v);

        _term.write("[ ");
        if (maxDepth > 0)
            for (unsigned int n = 0; n < v.list.length; ++n) {
                _term.write(' ');
                if (seen.find(v.list.elems[n]) != seen.end())
                    _term.write("<<repeated>>");
                else
                    try {
                        printValue(*v.list.elems[n], maxDepth - 1, seen);
                    } catch (AssertionError & e) {
                        _term.write("ESC_RED <<error: ");
						_term.write(e.msg());
						_term.write(">>" ESC_END);
                    }
            }
        else
            _term.write(" ...");
        _term.write(" ]");
        break;

    case tLambda:
        _term.write(ESC_BLU "<<lambda>>" ESC_END);
        break;

    case tPrimOp:
        _term.write(ESC_MAG "<<primop>>" ESC_END);
        break;

    case tPrimOpApp:
        _term.write(ESC_BLU "<<primop-app>>" ESC_END);
        break;

    default:
        _term.write(ESC_RED "<<unknown>>" ESC_END);
        break;
    }
    _term.flush();
}

void NixRepl::perform_completion()
{
	PWRN("%s not implemented", __func__);
}

void NixRepl::evaluate()
{
	if (!*_buf) return;

	string line(_buf);

	try {
		processLine(removeWhitespace(line));
	} catch (Error & e) {
		_term.write(ESC_RED "error: "); _term.write(e.msg()); _term.write(ESC_END);
	}
	_term.write('\n');
	_term.flush();
}

}

#endif