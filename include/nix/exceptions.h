#ifndef _INCLUDE__NIX__EXCEPTIONS_H_
#define _INCLUDE__NIX__EXCEPTIONS_H_

template<typename FUNC>
int handleExceptions(const nix::string & programName, FUNC const &fun)
{
	using namespace nix;

    string error = ANSI_RED "error:" ANSI_NORMAL " ";
    try {
        fun();
    } catch (Exit & e) {
        return e.status;
    } catch (UsageError & e) {
        printMsg(lvlError,
            format(error + "%1%\nTry ‘%2% --help’ for more information.")
            % e.what() % programName);
        return 1;
    } catch (BaseError & e) {
        printMsg(lvlError, format(error + "%1%%2%") % e.prefix() % e.msg());
        if (e.prefix() != "")
            printMsg(lvlError, "TODO: make a show-trace config attr");
        return e.status;
    } catch (std::bad_alloc & e) {
        printMsg(lvlError, error + "out of memory");
        return 1;
    } catch (std::exception & e) {
        printMsg(lvlError, error + e.what());
        return 1;
    } catch (...) {
		PERR("caught unhandled exception, good luck");
		throw;
	}
    return 0;
}

#endif