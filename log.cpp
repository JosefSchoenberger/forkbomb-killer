#include "log.h"

#include "spdlog/spdlog.h"
#ifdef USE_SYSTEMD
#include "spdlog/sinks/systemd_sink.h"
#endif

std::optional<std::string> set_logger(std::string s) {
#ifdef USE_SYSTEMD
	static std::shared_ptr<spdlog::logger> default_logger = spdlog::default_logger();
	static bool is_systemd = false;

	if (s.starts_with("systemd")) {
		if (!is_systemd) {
			is_systemd = true;
			auto systemd_sink = std::make_shared<spdlog::sinks::systemd_sink_st>();
			spdlog::logger logger{"forkbomb-killer", systemd_sink};
			spdlog::set_default_logger(std::make_shared<spdlog::logger>(logger));
		}
		s = s.substr(7);
		size_t i = s.find_first_not_of(" ");
		if (i && i != std::string::npos)
			s = s.substr(i);
	} else if (is_systemd) {
		is_systemd = false;
		spdlog::set_default_logger(default_logger);
	}
#endif

	if (s == "trace")
		spdlog::set_level(spdlog::level::trace);
	else if (s == "debug")
		spdlog::set_level(spdlog::level::debug);
	else if (s == "info")
		spdlog::set_level(spdlog::level::info);
	else if (s == "warn" || s == "warning")
		spdlog::set_level(spdlog::level::warn);
	else if (s == "err" || s == "error")
		spdlog::set_level(spdlog::level::err);
	else if (s == "critical")
		spdlog::set_level(spdlog::level::critical);
	else if (s == "off")
		spdlog::set_level(spdlog::level::off);
	else if (s.find_first_not_of(" ") != std::string::npos)
		return spdlog::fmt_lib::format("Unknown log level \"{}\"", s);

	return std::optional<std::string>{};
}

void setup_logger() {
	const char* logger_env = std::getenv("LOGGER");
	const bool is_run_by_systemd = std::getenv("SYSTEMD_EXEC_PID") != NULL;
	std::optional<std::string> err_msg;
	if (logger_env && is_run_by_systemd) {
		std::string s = logger_env;
		if (!s.starts_with("systemd"))
			s = std::string("systemd ") + logger_env;
		err_msg = set_logger(s);
	} else if (is_run_by_systemd) {
		err_msg = set_logger("systemd debug");
	} else if (logger_env) {
		err_msg = set_logger(logger_env);
	} else {
		err_msg = set_logger("debug");
	}
	if (err_msg.has_value())
		spdlog::critical(std::string("Could not set logger: ") + *err_msg);
}
