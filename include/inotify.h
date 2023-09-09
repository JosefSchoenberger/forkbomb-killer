#pragma once
#include <assert.h>

#include <functional>
#include <optional>
#include <string>
#include <cinttypes>
#include <unordered_map>

struct InotifyError {
	int e;
	std::string msg;
	std::string tostring();
	__attribute__((noreturn)) void bail(const char* err_msg_override = NULL) const;
};

struct InotifyEvent {
	int watch;
	uint32_t event_mask;
	uint32_t cookie;
	std::optional<std::string> path;
	std::string path_of_watch;

	std::string debug_string() const;
};

class Inotify final {
	int inotify_fd = -1;
	std::unordered_map<int, std::string> by_watches;
	std::unordered_map<std::string, int> by_paths;
	std::vector<std::function<void(int, const std::string&)>> removal_listener;

	__attribute__((aligned(4))) char buffer[1024];
	size_t buffer_next_event_idx = 0, buffer_filled_to_idx = 0;

	void notify_all_removal_listeners(int wd, const std::string& path);

public:
	Inotify();
	~Inotify();

	Inotify(Inotify&) = delete;
	Inotify(Inotify&&);
	Inotify& operator=(Inotify&) = delete;
	Inotify& operator=(Inotify&&);

	int addWatch(std::string path, int events_mask, int path_relative_to_watch = -1);
	void removeWatch(std::string const& path);
	void removeWatch(int watch);

	// append a handler. This handler will be invoked whenever a file is not listened to anymore.
	// This function will be called with the file's watch-descriptor and filename as its arguments.
	void addFileRemovalListener(std::function<void(int, const std::string&)>&& listener);

	struct InotifyEvent readEvent();
};
