#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <istream>
#include <iterator>
#include <memory>
#include <string>
#include <sys/inotify.h>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "args.h"
#include "inotify.h"
#include "log.h"
#include "spdlog/spdlog.h"

#ifdef USE_SYSTEMD
#include <systemd/sd-daemon.h>

#include "spdlog/sinks/systemd_sink.h"
#endif

static const std::string filename_to_listen_to = "pids.events";

__attribute__((noreturn)) static void bail(const char* err_msg) {
	spdlog::critical(err_msg);
	exit(EXIT_FAILURE);
}

static bool is_inside_dir(std::filesystem::path higher, std::filesystem::path lower) {
	// Adapted from https://stackoverflow.com/a/61125335/11346664
	lower = lower.lexically_normal();
	higher = higher.lexically_normal();

	auto [higher_end, _nothing] = std::mismatch(higher.begin(), higher.end(), lower.begin());

	return higher_end == higher.end();
}

/// Add all directories in this @path (and files matching @filename_to_listen_to) to this Inotify,
/// except if it matches any path in the @excludes vector.
///
/// May throw InotifyError.
void addAllRecursively(Inotify& i, std::filesystem::path const& path, std::string const& filename_to_listen_to,
					   std::vector<std::filesystem::path> excludes = {}) {
	for (auto& ex_path : excludes)
		if (is_inside_dir(ex_path, path))
			return;

	// Note: We need to add this file descriptor first and walk the dir later to avoid a race condition,
	// in which a subdirectory is created while we walk the tree.
	// Entries that are created during the walk will produce an inotify event. They might then also be
	// listed during the walk, but that's fine, Linux will just hand out the same watch descriptor as before.
	int w = i.addWatch(path, IN_MOVED_FROM | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF);
	spdlog::trace("Adding dir {} (watch={})", path.string(), w);

	std::error_code e{};
	for (auto const& dir_entry :
		 std::filesystem::directory_iterator{path, std::filesystem::directory_options::skip_permission_denied, e}) {
		auto p = dir_entry.path();
		try {
			if (dir_entry.is_directory())
				addAllRecursively(i, path / p, filename_to_listen_to, excludes);
			else if (dir_entry.is_regular_file() && p.filename().string() == filename_to_listen_to) {
				for (auto& ex_path : excludes)
					if (is_inside_dir(ex_path, path / p))
						return;
				i.addWatch(p.string(), IN_MODIFY);
			}
		} catch (InotifyError e) {
			if (e.e != ENOENT)
				throw e;
			// The newly created event has been removed in the meanwhile
			spdlog::trace("-> Could not add, does not exist anymore.");
			continue;
		}
	}

	if (e && e.category() == std::generic_category() && e.value() == ENOENT) {
		spdlog::trace("...aaand it has been removed again before I could iterate it.");
		return;
	}
}

static std::string read_file(const std::string &path) {
	std::ifstream ifs(path);

	// https://stackoverflow.com/a/2912614
	std::string content((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));

	content.erase(std::remove(content.begin(), content.end(), '\n'), content.cend());
	return content;
}

void kill_group_for_pid_event(InotifyEvent&& e) {
	assert(e.path_of_watch.ends_with("/" + filename_to_listen_to));

	std::string path = e.path_of_watch.substr(0, e.path_of_watch.length() - filename_to_listen_to.length());
	spdlog::info("Killing cgroup \"{}\"...", path);
	try {
		spdlog::info("pids.current = {}, pids.peak = {}, pids.max = {}, pids.events = {}",
				read_file(path + "pids.current"),
				read_file(path + "pids.peak"),
				read_file(path + "pids.max"),
				read_file(path + "pids.events"));
	} catch (std::exception& e) {
		spdlog::error("Could not log additional parameters about cgroup being killed: {}", e.what());
	}
	path = path + "cgroup.kill";

	int fd = open(path.c_str(), O_WRONLY);
	if (fd < 0) {
		spdlog::error("Could not kill: open \"{}\" as write-only failed: {}", path, strerror(errno));
		return;
	}
	const char* data = "1\n";
	const size_t len = strlen(data);
	ssize_t n_bytes = write(fd, data, len);
	if (n_bytes < 0) {
		spdlog::error("Could not kill: writing \"1\\n\" into cgroup.kill failed: {}", strerror(errno));
		close(fd);
		return;
	}
	if (static_cast<size_t>(n_bytes) < len) {
		spdlog::error("Could not kill: writing 2 bytes into cgroup.kill resulted in only {} bytes written?", n_bytes);
		close(fd);
		return;
	}
	close(fd);
	return;
}

void deal_with_event(
	Inotify& i, const Args& a, InotifyEvent&& e,
	std::unordered_map<int, std::pair<std::chrono::time_point<std::chrono::steady_clock>, uint64_t>>& pid_events,
	std::string const& filename_to_listen_to) {
	if (e.event_mask & IN_CREATE) {
		try {
			if (e.event_mask & IN_ISDIR)
				addAllRecursively(i, e.path_of_watch + "/" + e.path.value(), filename_to_listen_to);
			else if (!e.path.has_value())
				bail("Kernel gave an IN_CREATE event without an path?!?");
			else if (e.path.value() == filename_to_listen_to) {
				spdlog::trace("Added path {}", e.path.value());
				i.addWatch(std::move(e.path.value()), IN_MODIFY, e.watch);
			}
		} catch (InotifyError e) {
			if (e.e != ENOENT)
				throw e;
			// The newly created event has been removed in the meanwhile
			spdlog::trace("-> Could not add, does not exist anymore.");
		}
	} else if (e.event_mask & IN_MODIFY && e.path_of_watch.ends_with("/" + filename_to_listen_to)) {
		auto now = std::chrono::steady_clock::now();
		if (pid_events.contains(e.watch)) {
			auto& entry = pid_events.at(e.watch);
			spdlog::trace(
				"This watch's window started at {:15.9f}s and has had {} events since then",
				std::chrono::duration_cast<std::chrono::nanoseconds>(entry.first.time_since_epoch()).count() / 1e9,
				entry.second);
			if (entry.first < now - std::chrono::duration<float>(a.window_seconds)) {
				entry.first = now;
				entry.second = 0;
			} else {
				entry.second++;
				if (entry.second >= a.event_thresh) {
					entry.second = 0;
					kill_group_for_pid_event(std::move(e));
				}
			}
		} else {
			spdlog::trace("New watch window startging at  {:15.9f}s",
						  std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count() / 1e9);
			pid_events.emplace(e.watch, std::make_pair(now, 1));
		}
	}
}

int main(int argc, char** argv) {
	setup_logger();
	Args a{argc, argv};

	std::unordered_map<int, std::pair<std::chrono::time_point<std::chrono::steady_clock>, uint64_t>> pid_events;

#ifdef DEBUGGING_CLI
	std::thread([&pid_events]() {
		/* Note: This thread is racy. The CLI is only intended for debugging purposes. Won't fix for now. */
		std::cout << "Enter \"help\" for usage." << std::endl;
		std::string input;
		while (true) {
			std::cout << "$ " << std::flush;
			std::getline(std::cin, input);
			if (input == "help") {
				std::cout
					<< "commands:\n"
					   "\texit         - stop this program\n"
					   "\tlist_windows - list all watch descriptors with last window time (for debugging purposes)\n"
					   "\tset_log [logger] - sets logger, just like the LOGGER env\n"
					   "\thelp         - print this help"
					<< std::endl;
			} else if (input == "exit") {
				std::exit(0);
			} else if (input == "") {
				std::cout << std::endl;
				std::exit(0);
			} else if (input == "list") {
				if (pid_events.empty())
					std::cout << "list is empty." << std::endl;
				else
					for (const auto& [k, v] : pid_events) {
						std::cout << "\t" << k << " -> {" << v.first.time_since_epoch().count() << ", " << v.second
								  << "}" << std::endl;
					}
			} else if (input.starts_with("set_log ")) {
				input = input.substr(8);
				auto msg = set_logger(input);
				if (msg.has_value())
					std::cerr << "Error: " << *msg << std::endl;
			} else {
				std::cerr << "unknown command: \"" << input << "\"" << std::endl;
			}
		}
	}).detach();
#endif

	try {
		Inotify i;
		addAllRecursively(i, a.cgroup_path + a.slice_path, filename_to_listen_to,
						  {a.cgroup_path + "/user.slice/user-0.slice"});
		i.addFileRemovalListener([&](int wd, const std::string& /* path */) { pid_events.erase(wd); });
#ifdef USE_SYSTEMD
		sd_notify(0, "READY=1");
#endif
		while (true) {
			struct InotifyEvent e = i.readEvent();
			deal_with_event(i, a, std::move(e), pid_events, filename_to_listen_to);
		}
	} catch (InotifyError e) {
#ifdef USE_SYSTEMD
		auto s = spdlog::fmt_lib::format("ERRNO={}", e.e);
		sd_notify(0, s.c_str());
#endif
		e.bail();
	}
}
