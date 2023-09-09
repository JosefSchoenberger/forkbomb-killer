#pragma once
#include <string.h>

class Args {
public:
	std::string cgroup_path = "/sys/fs/cgroup";
	std::string slice_path = "/user.slice/";
	float window_seconds = 10.0;
	unsigned event_thresh = 50;

	Args(int argc, char** argv);
};
