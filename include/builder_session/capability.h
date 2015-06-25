/*
 * \brief  Client-side Builder session interface
 * \author Emery Hemingway
 * \date   2015-05-27
 */

#ifndef _INCLUDE__BUILDER_SESSION__CAPABILITY_H_
#define _INCLUDE__BUILDER_SESSION__CAPABILITY_H_

#include <builder_session/builder_session.h>
#include <base/capability.h>

namespace Builder { typedef Genode::Capability<Session> Session_capability; }

#endif