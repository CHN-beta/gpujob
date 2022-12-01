# pragma once
# include <string>
# include <optional>
# include <cereal/cereal.hpp>

struct job
{
	unsigned id;
	std::optional<unsigned> assign_to;
	std::string user, command, comment;
	enum class status { pending, running, finished } state;
	bool run_in_container;

	template <class Archive> void serialize(Archive & ar)
	{
		ar(id, assign_to, user, command, comment, state);
	}
};



inline void write_in(std::vector<job> new_jobs, std::vector<unsigned> remove_jobs)
{
	boost::interprocess::file_lock in_lock{"/tmp/gpujob/in.lock"};
	in_lock.lock();
	{
		std::ofstream out{"/tmp/gpujob/in.dat"};
		cereal::JSONOutputArchive{out}(new_jobs, remove_jobs);
	}
	if (!std::filesystem::exists("/tmp/gpujob/in.modified"))
		std::ofstream{"/tmp/gpujob/in.modified"};
}

inline void append_in(std::vector<job> new_jobs, std::vector<unsigned> remove_jobs)
{
	boost::interprocess::file_lock in_lock{"/tmp/gpujob/in.lock"};
	in_lock.lock();
	std::vector<job> previous_new_jobs;
	std::vector<unsigned> previous_remove_jobs;
	if (std::filesystem::exists("/tmp/gpujob/in.dat"))
	{
		std::ifstream in{"/tmp/gpujob/in.dat"};
		cereal::JSONInputArchive{in}(previous_new_jobs, previous_remove_jobs);
	}
	previous_new_jobs.insert(previous_new_jobs.end(), new_jobs.begin(), new_jobs.end());
	previous_remove_jobs.insert(previous_remove_jobs.end(), remove_jobs.begin(), remove_jobs.end());
	{
		std::ofstream out{"/tmp/gpujob/in.dat"};
		cereal::JSONOutputArchive{out}(previous_new_jobs, previous_remove_jobs);
	}
	if (!std::filesystem::exists("/tmp/gpujob/in.modified"))
		std::ofstream{"/tmp/gpujob/in.modified"};
}



inline std::vector<std::string> queue_gpu_name()
{
	std::vector<std::string> names;
	boost::process::ipstream out;
	boost::process::child c{"bash -c \". nvhpc && pgaccelinfo | grep 'Device Name'\"", boost::process::std_out > out};
	std::string line;
	while (c.running() && std::getline(out, line) && !line.empty())
	{
		std::regex re{"Device Name:\\s+NVIDIA GeForce (.*)"};
		std::smatch match;
		if (std::regex_search(line, match, re))
			names.push_back(match[1]);
	}
	c.wait();
	return names;
}

inline void write_out(std::vector<job> jobs)
{
	auto gpus = queue_gpu_name();

	if (!std::filesystem::exists("/tmp/gpujob"))
		std::filesystem::create_directory("/tmp/gpujob");
	if (!std::filesystem::exists("/tmp/gpujob/out.lock"))
		std::ofstream{"/tmp/gpujob/out.lock"};
	boost::interprocess::file_lock out_lock{"/tmp/gpujob/out.lock"};
	out_lock.lock();
	{
		std::ofstream out{"/tmp/gpujob/out.dat"};
		cereal::JSONOutputArchive{out}(jobs, gpus);
	}
}
