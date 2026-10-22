// Small helpers shared by the hub, producer and station processes.
#pragma once

#include <chrono>
#include <csignal>
#include <string>

#include <omniORB4/CORBA.h>

namespace telemetry::orb {

// Writes the stringified reference atomically (temp file + rename), so a
// process polling for the file never reads half an IOR.
void write_ior_file(CORBA::ORB_ptr orb, CORBA::Object_ptr obj, const std::string& path);

// Waits up to timeout for the file to appear, then returns the reference.
CORBA::Object_ptr read_ior_file(CORBA::ORB_ptr orb, const std::string& path,
                                std::chrono::milliseconds timeout);

// Naming Service helpers. Names use "a/b/c" with an optional ".kind" suffix
// per component. bind_name creates intermediate contexts and rebinds.
void bind_name(CORBA::ORB_ptr orb, const std::string& name, CORBA::Object_ptr obj);
CORBA::Object_ptr resolve_name(CORBA::ORB_ptr orb, const std::string& name);

// Blocks SIGINT and SIGTERM in the calling thread; call before ORB_init so
// every thread the ORB creates inherits the mask and the main thread can
// sigwait for a clean shutdown.
sigset_t block_termination_signals();

// Waits for SIGINT/SIGTERM or until the timeout elapses (0 = wait forever).
// Returns the signal number, or 0 on timeout.
int wait_for_termination(const sigset_t& set, std::chrono::milliseconds timeout);

std::string describe(const CORBA::Exception& ex);

}  // namespace telemetry::orb
