#include "inotify.h"

#include <errno.h>
#include <string.h>
#include <sys/inotify.h>

#include "spdlog/spdlog.h"

#ifdef USE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

std::string InotifyError::tostring() {
	return strerror(e);
}

__attribute__((noreturn)) void InotifyError::bail(const char* err_msg_override) const {
	spdlog::critical("{}: {} (errno {})", err_msg_override ?: msg, strerror(errno), errno);
	exit(EXIT_FAILURE);
}

std::string InotifyEvent::debug_string() const {
	std::string ev_string = "{watch=" + std::to_string(watch) + ", mask=[";

	uint32_t m = event_mask;
	bool anything = false;
	while (m) {
		switch (m & (-m)) {
#define X(x)                                  \
	case x:                                   \
		ev_string += anything ? ", " #x : #x; \
		anything = true;                      \
		m &= ~x;                              \
		break
			X(IN_ACCESS);
			X(IN_ATTRIB);
			X(IN_CLOSE_WRITE);
			X(IN_CLOSE_NOWRITE);
			X(IN_CREATE);
			X(IN_DELETE);
			X(IN_DELETE_SELF);
			X(IN_MODIFY);
			X(IN_MOVE_SELF);
			X(IN_MOVED_FROM);
			X(IN_MOVED_TO);
			X(IN_OPEN);
			X(IN_IGNORED);
			X(IN_ISDIR);
			X(IN_Q_OVERFLOW);
			X(IN_UNMOUNT);
#undef X
			default:
				// clear the lowest set bit
				m &= ~(m & (-m));
		}
	}

	ev_string += "], cookie=" + std::to_string(cookie) + ", path=" + path.value_or("\"\"") +
				 ", path_of_watch=" + path_of_watch + "}";
	return ev_string;
}

static void systemd_set_status(size_t i) {
#ifndef USE_SYSTEMD
	(void)i;
#else
	auto s = spdlog::fmt_lib::format("STATUS=Currently watching {} paths", i);
	sd_notify(0, s.c_str());
#endif
}

Inotify::Inotify() {
	inotify_fd = inotify_init1(IN_CLOEXEC);
	if (inotify_fd < 0)
		throw InotifyError{errno, "Could not create inotify filedescriptor"};
}

Inotify::~Inotify() {
	if (inotify_fd < 0)
		return;
	close(inotify_fd);
}

Inotify::Inotify(Inotify&& other) : inotify_fd(other.inotify_fd) {
	other.inotify_fd = -1;
}

Inotify& Inotify::operator=(Inotify&& other) {
	inotify_fd = other.inotify_fd;
	other.inotify_fd = -1;
	return *this;
}

int Inotify::addWatch(std::string path, int events_mask, int path_relative_to_watch) {
	if (path_relative_to_watch != -1) {
		path = by_watches.at(path_relative_to_watch) + "/" + path;
	}
	int watch = inotify_add_watch(inotify_fd, path.c_str(), events_mask);
	if (watch < 0)
		throw InotifyError{errno, "Could not add path \"" + path + "\" to inotify fd"};
	by_watches.insert({watch, path});
	by_paths.insert({path, watch});
	systemd_set_status(by_paths.size());
	return watch;
}

void Inotify::removeWatch(std::string const& path) {
	int watch = by_paths.at(path);
	if (inotify_rm_watch(inotify_fd, watch))
		throw InotifyError{errno, "Could not remove watch from inotify fd"};
	notify_all_removal_listeners(watch, path);
	by_paths.erase(path);
	by_watches.erase(watch);
	systemd_set_status(by_paths.size());
}

void Inotify::removeWatch(int watch) {
	if (inotify_rm_watch(inotify_fd, watch))
		throw InotifyError{errno, "Could not remove watch from inotify fd"};
	auto& path = by_watches.at(watch);
	notify_all_removal_listeners(watch, path);
	by_paths.erase(path);
	by_watches.erase(watch);
	systemd_set_status(by_paths.size());
}


void Inotify::addFileRemovalListener(std::function<void(int, const std::string&)>&& listener) {
	removal_listener.push_back(listener);
}

void Inotify::notify_all_removal_listeners(int wd, const std::string& path) {
	for (auto& listener : removal_listener) {
		listener(wd, path);
	}
}

struct InotifyEvent Inotify::readEvent() {
	while (true) {
		if (buffer_filled_to_idx == buffer_next_event_idx) {
			buffer_filled_to_idx = buffer_next_event_idx = 0;
			ssize_t n_bytes = read(inotify_fd, buffer, sizeof(buffer));
			if (n_bytes < 0) {
				buffer_filled_to_idx = 0;
				throw InotifyError{errno, "Could not read event from inotify fd"};
			}
			if (n_bytes == 0) {
				buffer_filled_to_idx = 0;
				throw InotifyError{0, "Could not read any event from inotify: read returned 0"};
			}
			buffer_filled_to_idx = n_bytes;
		}

		assert(buffer_filled_to_idx - buffer_next_event_idx >= 16);

		struct inotify_event* event_ptr = reinterpret_cast<struct inotify_event*>(buffer + buffer_next_event_idx);

		buffer_next_event_idx += sizeof(*event_ptr) + event_ptr->len;

		struct InotifyEvent new_event = {
			.watch = event_ptr->wd,
			.event_mask = event_ptr->mask,
			.cookie = event_ptr->cookie,
			.path = event_ptr->len ? std::optional{std::string{event_ptr->name}} : std::optional<std::string>{},
			.path_of_watch = by_watches.contains(event_ptr->wd) ? by_watches[event_ptr->wd] : "",
		};

		if (!by_watches.contains(new_event.watch)) {
			spdlog::log(
#ifdef MORE_EFFORT_REMOVAL
				new_event.event_mask & IN_IGNORED || new_event.event_mask & IN_DELETE_SELF ? spdlog::level::trace
				                                                                           : spdlog::level::warn,
#else
				spdlog::level::warn,
#endif
				"Got event for unknown watch: {}", new_event.debug_string());
			continue;
		}

		spdlog::trace(new_event.debug_string());
		if (new_event.event_mask & IN_IGNORED
#ifdef MORE_EFFORT_REMOVAL
			|| new_event.event_mask & IN_DELETE_SELF
#endif
		) {
			spdlog::trace("Removing watch={} ({})", new_event.watch, new_event.path_of_watch);
			// Kernel already removes this watch, we only delete this entry from our maps
			auto& path = by_watches.at(new_event.watch);
			notify_all_removal_listeners(new_event.watch, path);
			by_paths.erase(path);
			by_watches.erase(new_event.watch);
			systemd_set_status(by_paths.size());
			continue;
		}
#ifdef MORE_EFFORT_REMOVAL
		if (new_event.event_mask & IN_DELETE) {
			// std::vector<std::pair<std::string, int>> watches_to_delete;
			std::vector<std::function<void()>> watches_to_delete;
			for (auto& [p, w] : by_paths) {
				if (w != new_event.watch && p.starts_with(new_event.path_of_watch + "/" + new_event.path.value())) {
					watches_to_delete.push_back([w, p, this, &new_event]() {
						if (p == new_event.path_of_watch + "/" + new_event.path.value()) {
							spdlog::trace("Assuming gone: watch={} ({})", p, w);
							auto& path = by_watches.at(w);
							notify_all_removal_listeners(w, path);
							by_paths.erase(path);
							by_watches.erase(w);
							systemd_set_status(by_paths.size());
						} else {
							spdlog::trace("Proactively removing watch={} ({})", p, w);
							removeWatch(w);
						}
					});
				}
			}

			for (auto f : watches_to_delete)
				f();
		}
#endif

		return new_event;
	}
}
