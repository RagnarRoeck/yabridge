// yabridge: a Wine VST bridge
// Copyright (C) 2020  Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "group.h"

#include <unistd.h>
#include <boost/asio/read_until.hpp>
#include <boost/process/environment.hpp>
#include <regex>

#include "../../common/sockets.h"

// FIXME: `std::filesystem` is broken in wineg++, at least under Wine 5.8. Any
//        path operation will thrown an encoding related error
namespace fs = boost::filesystem;

using namespace std::literals::chrono_literals;

/**
 * The delay between calls to the event loop at a more than cinematic 30 fps.
 */
constexpr std::chrono::duration event_loop_interval = 1000ms / 30;

/**
 * Listen on the specified endpoint if no process is already listening there,
 * otherwise throw. This is needed to handle these three situations:
 *
 * 1. The endpoint does not already exist, and we can simply create an endpoint.
 * 2. The endpoint already exists but it is stale and no process is currently
 *    listening. In this case we can remove the file and start listening.
 * 3. The endpoint already exists and another process is currently listening on
 *    it. In this situation we will throw immediately and we'll terminate this
 *    process.
 *
 * If anyone knows a better way to handle this, please let me know!
 *
 * @throw std::runtime_error If another process is already listening on the
 *        endpoint.
 */
boost::asio::local::stream_protocol::acceptor create_acceptor_if_inactive(
    boost::asio::io_context& io_context,
    boost::asio::local::stream_protocol::endpoint& endpoint);

/**
 * Create a logger prefix containing the group name based on the socket path.
 */
std::string create_logger_prefix(const fs::path& socket_path);

StdIoCapture::StdIoCapture(boost::asio::io_context& io_context,
                           int file_descriptor)
    : pipe(io_context),
      target_fd(file_descriptor),
      original_fd_copy(dup(file_descriptor)) {
    // We'll use the second element of these two file descriptors to reopen
    // `file_descriptor`, and the first one to read the captured contents from
    if (::pipe(pipe_fd) != 0) {
        throw std::system_error(errno, std::system_category());
    }

    // We've already created a copy of the original file descriptor, so we can
    // reopen it using the newly created pipe
    dup2(pipe_fd[1], target_fd);
    close(pipe_fd[1]);

    pipe.assign(pipe_fd[0]);
}

StdIoCapture::~StdIoCapture() {
    // Restore the original file descriptor and close the pipe. The other wend
    // was already closed in the constructor.
    dup2(original_fd_copy, target_fd);
    close(original_fd_copy);
    close(pipe_fd[0]);
}

GroupBridge::GroupBridge(boost::filesystem::path group_socket_path)
    : logger(Logger::create_from_environment(
          create_logger_prefix(group_socket_path))),
      plugin_context(),
      stdio_context(),
      stdout_redirect(stdio_context, STDOUT_FILENO),
      stderr_redirect(stdio_context, STDERR_FILENO),
      group_socket_endpoint(group_socket_path.string()),
      group_socket_acceptor(
          create_acceptor_if_inactive(plugin_context, group_socket_endpoint)),
      events_timer(plugin_context),
      shutdown_timer(plugin_context) {
    // Write this process's original STDOUT and STDERR streams to the logger
    // TODO: This works for output generated by plugins, but not for debug
    //       messages generated by wineserver. Is it possible to catch those?
    async_log_pipe_lines(stdout_redirect.pipe, stdout_buffer, "[STDOUT] ");
    async_log_pipe_lines(stderr_redirect.pipe, stderr_buffer, "[STDERR] ");

    stdio_handler = std::jthread([&]() { stdio_context.run(); });
}

GroupBridge::~GroupBridge() {
    stdio_context.stop();
}

void GroupBridge::handle_plugin_dispatch(const GroupRequest request) {
    // At this point the `active_plugins` map will already contain the
    // intialized plugin's `Vst2Bridge` instance and this thread's handle
    auto& [thread, bridge] = active_plugins.at(request);

    // Blocks this thread until the plugin shuts down, handling all events on
    // the main IO context
    bridge->handle_dispatch();
    logger.log("'" + request.plugin_path + "' has exited");

    // After the plugin has exited we'll remove this thread's plugin from the
    // active plugins. This is done within the IO context because the call to
    // `FreeLibrary()` has to be done from the main thread, or else we'll
    // potentially corrupt our heap. This way we can also properly join the
    // thread again. If no active plugins remain, then we'll terminate the
    // process.
    boost::asio::post(plugin_context, [&, request]() {
        std::lock_guard lock(active_plugins_mutex);

        // The join is implicit because we're using std::jthread
        active_plugins.erase(request);
    });

    // Defer actually shutting down the process to allow for fast plugin
    // scanning by allowing plugins to reuse the same group host process
    shutdown_timer.expires_after(2s);
    shutdown_timer.async_wait([&](const boost::system::error_code& error) {
        // A previous timer gets canceled automatically when another plugin
        // exits
        if (error.failed()) {
            return;
        }

        std::lock_guard lock(active_plugins_mutex);
        if (active_plugins.size() == 0) {
            logger.log(
                "All plugins have exited, shutting down the group process");
            plugin_context.stop();
        }
    });
}

void GroupBridge::handle_incoming_connections() {
    accept_requests();
    async_handle_events();

    logger.log(
        "Group host is up and running, now accepting incoming connections");
    plugin_context.run();
}

bool GroupBridge::should_skip_message_loop() {
    // We do not need additional locking since the call to `AEffect::dispatcher`
    // and the actual event handling and message loop handling are performed
    // within the IO context and these values thus can't change while another
    // the message loop is being running
    std::lock_guard lock(active_plugins_mutex);
    for (auto& [parameters, value] : active_plugins) {
        auto& [thread, bridge] = value;
        if (bridge->should_skip_message_loop()) {
            return true;
        }
    }

    return false;
}

void GroupBridge::accept_requests() {
    group_socket_acceptor.async_accept(
        [&](const boost::system::error_code& error,
            boost::asio::local::stream_protocol::socket socket) {
            std::lock_guard lock(active_plugins_mutex);

            // Stop the whole process when the socket gets closed unexpectedly
            if (error.failed()) {
                logger.log("Error while listening for incoming connections:");
                logger.log(error.message());

                plugin_context.stop();
            }

            // Read the parameters, and then host the plugin in this process
            // just like if we would be hosting the plugin individually through
            // `yabridge-hsot.exe`. We will reply with this process's PID so the
            // yabridge plugin will be able to tell if the plugin has caused
            // this process to crash during its initialization to prevent
            // waiting indefinitely on the sockets to be connected to.
            const auto request = read_object<GroupRequest>(socket);
            write_object(socket, GroupResponse{boost::this_process::get_id()});

            // Collisions in the generated socket names should be very rare, but
            // it could in theory happen
            assert(!active_plugins.contains(request));

            // The plugin has to be initiated on the IO context's thread because
            // this has to be done on the same thread that's handling messages,
            // and all window messages have to be handled from the same thread.
            logger.log("Received request to host '" + request.plugin_path +
                       "' using socket endpoint base directory '" +
                       request.endpoint_base_dir + "'");
            try {
                auto bridge = std::make_unique<Vst2Bridge>(
                    plugin_context, request.plugin_path,
                    request.endpoint_base_dir);
                logger.log("Finished initializing '" + request.plugin_path +
                           "'");

                // Start listening for dispatcher events sent to the plugin's
                // socket on another thread. The actual event handling will
                // still occur within this IO context.
                active_plugins[request] =
                    std::pair(std::jthread([&, request]() {
                                  handle_plugin_dispatch(request);
                              }),
                              std::move(bridge));
            } catch (const std::runtime_error& error) {
                logger.log("Error while initializing '" + request.plugin_path +
                           "':");
                logger.log(error.what());
            }

            accept_requests();
        });
}

void GroupBridge::async_handle_events() {
    // Try to keep a steady framerate, but add in delays to let other events get
    // handled if the GUI message handling somehow takes very long.
    events_timer.expires_at(
        std::max(events_timer.expiry() + event_loop_interval,
                 std::chrono::steady_clock::now() + 5ms));
    events_timer.async_wait([&](const boost::system::error_code& error) {
        if (error.failed()) {
            return;
        }

        {
            // Always handle X11 events
            std::lock_guard lock(active_plugins_mutex);
            for (auto& [parameters, value] : active_plugins) {
                auto& [thread, bridge] = value;
                bridge->handle_x11_events();
            }
        }

        // Handle Win32 messages unless plugins are in the middle of opening
        // their editor
        if (!should_skip_message_loop()) {
            std::lock_guard lock(active_plugins_mutex);

            MSG msg;

            // Keep the loop responsive by not handling too many events at once
            //
            // For some reason the Melda plugins run into a seemingly infinite
            // timer loop for a little while after opening a second editor.
            // Without this limit everything will get blocked indefinitely. How
            // could this be fixed?
            for (int i = 0; i < max_win32_messages &&
                            PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE);
                 i++) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }

        async_handle_events();
    });
}

void GroupBridge::async_log_pipe_lines(
    boost::asio::posix::stream_descriptor& pipe,
    boost::asio::streambuf& buffer,
    std::string prefix) {
    boost::asio::async_read_until(
        pipe, buffer, '\n',
        [&, prefix](const boost::system::error_code& error, size_t) {
            // When we get an error code then that likely means that the pipe
            // has been clsoed and we have reached the end of the file
            if (error.failed()) {
                return;
            }

            std::string line;
            std::getline(std::istream(&buffer), line);
            logger.log(prefix + line);

            async_log_pipe_lines(pipe, buffer, prefix);
        });
}

boost::asio::local::stream_protocol::acceptor create_acceptor_if_inactive(
    boost::asio::io_context& io_context,
    boost::asio::local::stream_protocol::endpoint& endpoint) {
    // First try to listen on the endpoint normally
    try {
        return boost::asio::local::stream_protocol::acceptor(io_context,
                                                             endpoint);
    } catch (const boost::system::system_error& error) {
        // If this failed, then either there is a stale socket file or another
        // process is already is already listening. In the last case we will
        // simply throw so the other process can handle the request.
        std::ifstream open_sockets("/proc/net/unix");
        std::string endpoint_path = endpoint.path();
        for (std::string line; std::getline(open_sockets, line);) {
            if (line.size() < endpoint_path.size()) {
                continue;
            }

            std::string file = line.substr(line.size() - endpoint_path.size());
            if (file == endpoint_path) {
                // Another process is already listening, so we don't have to do
                // anything
                throw error;
            }
        }

        // At this point we can remove the stale socket and start listening
        fs::remove(endpoint_path);
        return boost::asio::local::stream_protocol::acceptor(io_context,
                                                             endpoint);
    }
}

std::string create_logger_prefix(const fs::path& socket_path) {
    // The group socket filename will be in the format
    // '/tmp/yabridge-group-<group_name>-<wine_prefix_id>-<architecture>.sock',
    // where Wine prefix ID is just Wine prefix ran through `std::hash` to
    // prevent collisions without needing complicated filenames. We want to
    // extract the group name.
    std::string socket_name =
        socket_path.filename().replace_extension().string();

    std::smatch group_match;
    std::regex group_regexp("^yabridge-group-(.*)-[^-]+-[^-]+$",
                            std::regex::ECMAScript);
    if (std::regex_match(socket_name, group_match, group_regexp)) {
        socket_name = group_match[1].str();

#ifdef __i386__
        // Mark 32-bit versions to avoid potential confusion caused by 32-bit
        // and regular 64-bit group processes with the same name running
        // alongside eachother
        socket_name += "-x32";
#endif
    }

    return "[" + socket_name + "] ";
}
