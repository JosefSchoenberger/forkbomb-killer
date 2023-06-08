#include <stdlib.h>
#include <iostream>
#include <getopt.h>
#include <string>

#include "args.h"
#include "spdlog/spdlog.h"

Args::Args(int argc, char** argv) {
	int choice;
	while (1) {
		static struct option long_options[] = {
			{"version",         no_argument,       0, 'v'},
			{"help",            no_argument,       0, 'h'},
			{"cgroup-mnt",      required_argument, 0, 'c'},
			{"slice",           required_argument, 0, 's'},
			{"window-seconds",  required_argument, 0, 'w'},
			{"event-threshold", required_argument, 0, 't'},
			{0,0,0,0}
		};

		int option_index = 0;

		choice = getopt_long( argc, argv, "vhc:s:w:t:", long_options, &option_index);

		if (choice == -1)
			break;

		size_t endidx;
		try {
			switch(choice) {
				case 'v':
					std::cout << "forkbomb-killer 0.1" << std::endl;
					exit(EXIT_SUCCESS);
					break;
				case 'h':
					std::cout << "Usage: forkbomb-killer [options]\n\n"
						"Options:\n"
						"  -h --help                   Print this help message and exit\n"
						"  -c --cgroup-mnt=<path>      Path where cgroup is mounted [default: " << cgroup_path << "]\n"
						"  -s --slice=<path>           Slice in which all cgroups should be indexed [default: " << slice_path << "]\n"
						"  -w --window-seconds=<float> Window length in seconds for counting failed forks [default: " << window_seconds << "]\n"
						"  -t --event-threshold=<int>  Threshold for amount of failed forks in time window before killing slice [default: " << event_thresh << "]\n"
						"  -v --version                Print version and exit"
						<< std::endl;
					exit(EXIT_SUCCESS);
					break;
				case 'c':
					cgroup_path = optarg;
					break;
				case 's':
					slice_path = optarg;
					break;
				case 'w':
					window_seconds = std::stof(optarg, &endidx);
					if (endidx > std::strlen(optarg)) {
						std::cerr << "Error: \"" << optarg << "\" is not a valid float" << std::endl;
						std::exit(1);
					}
					break;
				case 't': {
					long long val = std::stoll(optarg, &endidx);
					if (val < 0 || val >= (1LL << 8 * sizeof(unsigned))) {
						throw std::out_of_range("");
					}
					event_thresh = val;
					if (endidx > std::strlen(optarg)) {
						std::cerr << "Error: \"" << optarg << "\" is not a valid unsigned integer" << std::endl;
						std::exit(1);
					}
				} break;
				case '?':
					// getopt_long will have already printed an error
					break;
			}
		} catch (std::invalid_argument const& e) {
			std::cerr << "Error: Could not parse \"" << optarg << "\"" << std::endl;
			std::exit(1);
		} catch (std::out_of_range const& e) {
			std::cerr << "Error: \"" << optarg << "\" is out of range" << std::endl;
			std::exit(1);
		};
	}

	/* Deal with non-option arguments here */
	if (optind < argc) {
		std::cerr << "There are trailing arguments:" << std::endl;
		for (int i = optind; i < argc; i++)
			std::cerr << "  " << argv[i] << std::endl;
		std::cerr << "Aborting." << std::endl;
		exit(EXIT_FAILURE);
	}

	//std::cout << "Args:\n\tcgroup-mnt=\"" << cgroup_path << "\"\n\tslice=\"" << slice_path << "\"\n\twindows-seconds=\"" << window_seconds << "\"\n\tevent-threshold=" << event_thresh << std::endl;
}
