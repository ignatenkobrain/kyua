// Copyright 2011 Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
// * Neither the name of Google Inc. nor the names of its contributors
//   may be used to endorse or promote products derived from this software
//   without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

extern "C" {
#include <sys/stat.h>

#include <signal.h>
}

#include <cerrno>
#include <iostream>

#include "engine/exceptions.hpp"
#include "engine/plain_iface/test_case.hpp"
#include "engine/plain_iface/test_program.hpp"
#include "engine/results.hpp"
#include "utils/datetime.hpp"
#include "utils/env.hpp"
#include "utils/format/macros.hpp"
#include "utils/fs/auto_cleaners.hpp"
#include "utils/fs/exceptions.hpp"
#include "utils/fs/operations.hpp"
#include "utils/logging/macros.hpp"
#include "utils/optional.ipp"
#include "utils/process/children.ipp"
#include "utils/process/exceptions.hpp"
#include "utils/sanity.hpp"
#include "utils/signals/exceptions.hpp"
#include "utils/signals/misc.hpp"
#include "utils/signals/programmer.hpp"

namespace datetime = utils::datetime;
namespace fs = utils::fs;
namespace plain_iface = engine::plain_iface;
namespace process = utils::process;
namespace results = engine::results;
namespace signals = utils::signals;
namespace user_files = engine::user_files;

using utils::none;
using utils::optional;


namespace {


/// Exit code returned when the exec of the test program fails.
static int exec_failure_code = 120;


/// Number of the stop signal.
///
/// This is set by interrupt_handler() when it receives a signal that ought to
/// terminate the execution of the current test case.
static int interrupted_signo = 0;


/// Signal handler for termination signals.
///
/// \param signo The signal received.
///
/// \post interrupted_signo is set to the received signal.
static void
interrupt_handler(const int signo)
{
    const char* message = "[-- Signal caught; please wait for clean up --]\n";
    ::write(STDERR_FILENO, message, std::strlen(message));
    interrupted_signo = signo;

    POST(interrupted_signo != 0);
    POST(interrupted_signo == signo);
}


/// Syntactic sugar to validate if there is a pending signal.
///
/// \throw interrupted_error If there is a pending signal that ought to
///     terminate the execution of the program.
static void
check_interrupt(void)
{
    LD("Checking for pending interrupt signals");
    if (interrupted_signo != 0) {
        LI("Interrupt pending; raising error to cause cleanup");
        throw engine::interrupted_error(interrupted_signo);
    }
}


/// Atomically creates a new work directory with a unique name.
///
/// The directory is created under the system-wide configured temporary
/// directory as defined by the TMPDIR environment variable.
///
/// \return The path to the new work directory.
///
/// \throw fs::error If there is a problem creating the temporary directory.
static fs::path
create_work_directory(void)
{
    const char* tmpdir = std::getenv("TMPDIR");
    if (tmpdir == NULL)
        return fs::mkdtemp(fs::path("/tmp/kyua.XXXXXX"));
    else
        return fs::mkdtemp(fs::path(F("%s/kyua.XXXXXX") % tmpdir));
}


/// Formats the termination status of a process to be used with validate_result.
///
/// \param status The status to format.
///
/// \return A string describing the status.
static std::string
format_status(const process::status& status)
{
    if (status.exited())
        return F("Exited with code %d") % status.exitstatus();
    else if (status.signaled())
        return F("Received signal %d%s") % status.termsig() %
            (status.coredump() ? " (core dumped)" : "");
    else
        return F("Terminated in an unknown manner");
}


/// Isolates the current process from the rest of the system.
///
/// This is intended to be used right before executing a test program because it
/// attempts to isolate the current process from the rest of the system.
///
/// By isolation, we understand:
///
/// * Change the cwd of the process to a known location that will be cleaned up
///   afterwards by the runner monitor.
/// * Reset a set of critical environment variables to known good values.
/// * Reset the umask to a known value.
/// * Reset the signal handlers.
///
/// \throw std::runtime_error If there is a problem setting up the process
///     environment.
static void
isolate_process(const fs::path& cwd)
{
    // The utils::process library takes care of creating a process group for
    // us.  Just ensure that is still true, or otherwise things will go pretty
    // badly.
    INV(::getpgrp() == ::getpid());

    ::umask(0022);

    for (int i = 0; i <= signals::last_signo; i++) {
        try {
            if (i != SIGKILL && i != SIGSTOP)
                signals::reset(i);
        } catch (const signals::system_error& e) {
            // Just ignore errors trying to reset signals.  It might happen
            // that we try to reset an immutable signal that we are not aware
            // of, so we certainly do not want to make a big deal of it.
        }
    }

    // TODO(jmmv): It might be better to do the opposite: just pass a good known
    // set of variables to the child (aka HOME, PATH, ...).  But how do we
    // determine this minimum set?
    utils::unsetenv("LANG");
    utils::unsetenv("LC_ALL");
    utils::unsetenv("LC_COLLATE");
    utils::unsetenv("LC_CTYPE");
    utils::unsetenv("LC_MESSAGES");
    utils::unsetenv("LC_MONETARY");
    utils::unsetenv("LC_NUMERIC");
    utils::unsetenv("LC_TIME");

    utils::setenv("TZ", "UTC");

    if (::chdir(cwd.c_str()) == -1)
        throw std::runtime_error(F("Failed to enter work directory %s") % cwd);
    utils::setenv("HOME", fs::current_path().str());
}


/// Functor to execute a test case in a subprocess.
class execute_test_case {
    plain_iface::test_case _test_case;
    fs::path _work_directory;

    /// Exception-safe version of operator().
    void
    safe_run(void) const
    {
        const fs::path test_program = _test_case.test_program().absolute_path();
        const fs::path abs_test_program = test_program.is_absolute() ?
            test_program : test_program.to_absolute();

        isolate_process(_work_directory);

        std::vector< std::string > args;
        try {
            process::exec(abs_test_program, args);
        } catch (const process::system_error& e) {
            std::cerr << "Failed to execute test program: " << e.what() << '\n';
            std::exit(exec_failure_code);
        }
    }

public:
    /// Constructor for the functor.
    ///
    /// \param test_case_ The data of the test case, including the program name,
    ///     the test case name and its metadata.
    /// \param work_directory_ The path to the directory to chdir into when
    ///     running the test program.
    execute_test_case(const plain_iface::test_case& test_case_,
                      const fs::path& work_directory_) :
        _test_case(test_case_),
        _work_directory(work_directory_)
    {
    }

    /// Entry point for the functor.
    void
    operator()(void)
    {
        try {
            safe_run();
        } catch (const std::runtime_error& e) {
            std::cerr << "Caught unhandled exception while setting up the test"
                "case: " << e.what() << '\n';
        } catch (...) {
            std::cerr << "Caught unknown exception while setting up the test"
                "case\n";
        }
        std::abort();
    }
};


/// Forks a subprocess and waits for its completion.
///
/// \param hook The code to execute in the subprocess.
/// \param outfile The file that will receive the stdout output.
/// \param errfile The file that will receive the stderr output.
///
/// \return The exit status of the process or none if the timeout expired.
template< class Hook >
optional< process::status >
fork_and_wait(Hook hook, const fs::path& outfile, const fs::path& errfile)
{
    std::auto_ptr< process::child_with_files > child =
        process::child_with_files::fork(hook, outfile, errfile);
    try {
        const datetime::delta timeout(60, 0);  // TODO(jmmv): Parametrize.
        return utils::make_optional(child->wait(timeout));
    } catch (const process::system_error& error) {
        if (error.original_errno() == EINTR) {
            (void)::kill(child->pid(), SIGKILL);
            (void)child->wait();
            check_interrupt();
            UNREACHABLE;
        } else
            throw error;
    } catch (const process::timeout_error& error) {
        return none;
    }
}


/// Converts the exit status of the test program to a result.
///
/// \param maybe_status The exit status of the program, or none if it timed out.
///
/// \return A test case result.
static results::result_ptr
calculate_result(const optional< process::status >& maybe_status)
{
    if (!maybe_status)
        return results::result_ptr(new results::broken("Test case timed out"));
    const process::status& status = maybe_status.get();

    if (status.exited()) {
        if (status.exitstatus() == EXIT_SUCCESS)
            return results::result_ptr(new results::passed());
        else if (status.exitstatus() == exec_failure_code)
            return results::result_ptr(
                new results::broken("Failed to execute test program"));
        else
            return results::result_ptr(
                new results::failed(format_status(status)));
    } else {
        return results::result_ptr(new results::broken(
                                       format_status(status)));
    }
}


/// Auxiliary function to execute a test case within a work directory.
///
/// This is an auxiliary function for run_test_case_safe that is protected from
/// the reception of common termination signals.
///
/// \param test_case The test case to execute.
/// \param workdir The directory in which the test case has to be run.
///
/// \return The result of the execution of the test case.
///
/// \throw interrupted_error If the execution has been interrupted by the user.
static results::result_ptr
run_test_case_safe_workdir(const plain_iface::test_case& test_case,
                           const fs::path& workdir)
{
    const fs::path rundir(workdir / "run");
    fs::mkdir(rundir, 0755);

    const fs::path result_file(workdir / "result.txt");

    check_interrupt();

    LI(F("Running test case '%s'") % test_case.identifier().str());
    optional< process::status > body_status = fork_and_wait(
        execute_test_case(test_case, rundir),
        workdir / "stdout.txt", workdir / "stderr.txt");

    check_interrupt();

    return calculate_result(body_status);
}


/// Auxiliary function to execute a test case.
///
/// This is an auxiliary function for run_test_case that is protected from
/// leaking exceptions.  Any exception not managed here is probably a mistake,
/// but is correctly captured in the caller.
///
/// \param test_case The test case to execute.
///
/// \return The result of the execution of the test case.
///
/// \throw interrupted_error If the execution has been interrupted by the user.
static results::result_ptr
run_test_case_safe(const plain_iface::test_case& test_case)
{
    // These three separate objects are ugly.  Maybe improve in some way.
    signals::programmer sighup(SIGHUP, interrupt_handler);
    signals::programmer sigint(SIGINT, interrupt_handler);
    signals::programmer sigterm(SIGTERM, interrupt_handler);

    results::result_ptr result;

    fs::auto_directory workdir(create_work_directory());
    try {
        check_interrupt();
        result = run_test_case_safe_workdir(test_case, workdir.directory());

        try {
            workdir.cleanup();
        } catch (const fs::error& e) {
            if (result->good()) {
                result = results::result_ptr(new results::broken(F(
                    "Could not clean up test work directory: %s") % e.what()));
            } else {
                LW(F("Not reporting work directory clean up failure because "
                     "the test is already broken: %s") % e.what());
            }
        }
    } catch (const engine::interrupted_error& e) {
        workdir.cleanup();

        sighup.unprogram();
        sigint.unprogram();
        sigterm.unprogram();

        throw e;
    }

    sighup.unprogram();
    sigint.unprogram();
    sigterm.unprogram();

    check_interrupt();

    return result;
}


}  // anonymous namespace


/// Constructs a new test case.
///
/// \param test_program_ The test program this test case belongs to.  This
///     object must exist during the lifetime of the test case.
plain_iface::test_case::test_case(const base_test_program& test_program_) :
    base_test_case(test_program_, "main")
{
}


/// Returns a string representation of all test case properties.
///
/// The returned keys and values match those that can be defined by the test
/// case.
///
/// \return A key/value mapping describing all the test case properties.
engine::properties_map
plain_iface::test_case::get_all_properties(void) const
{
    return properties_map();
}


/// Executes the test case.
///
/// This should not throw any exception: problems detected during execution are
/// reported as a broken test case result.
///
/// \param unused_config The run-time configuration for the test case.
///
/// \return The result of the execution.
engine::results::result_ptr
plain_iface::test_case::do_run(const user_files::config& unused_config) const
{
    LI(F("Processing test case '%s'") % identifier().str());

    results::result_ptr result;
    try {
        result = run_test_case_safe(*this);
    } catch (const interrupted_error& e) {
        throw e;
    } catch (const std::exception& e) {
        result = results::result_ptr(new results::broken(F(
            "The test caused an error in the runtime system: %s") % e.what()));
    }
    INV(result.get() != NULL);
    return result;
}