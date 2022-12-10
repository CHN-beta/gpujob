# pragma once
# include <string>
# include <optional>
# include <vector>
# include <map>
# include <filesystem>
# include <fstream>
# include <boost/interprocess/sync/file_lock.hpp>
# include <cereal/cereal.hpp>
# include <cereal/types/map.hpp>
# include <cereal/types/optional.hpp>
# include <cereal/types/string.hpp>
# include <cereal/types/vector.hpp>
# include <cereal/types/utility.hpp>
# include <cereal/archives/json.hpp>
# include <fmt/format.h>
# include <fmt/ranges.h>
# include <pwd.h>
# include <grp.h>
# include <sys/stat.h>

struct Job_t
// 用来描述一个任务的相关信息
// Program 通常为 bash 而不是 vasp 或 lammps 等, 这是为了方便设置环境
// 不再另外写脚本来设置环境, 而是直接在这里设置
// 考虑到特殊字符的问题, 涉及到用户输入的字符串参数, 都用环境变量传入而不是直接传入
{
	unsigned Id;
	std::string User, Program, Comment;
	std::map<std::string, std::string> Environment;
	std::vector<std::string> Arguments;
	unsigned UsingCores;
	std::vector<unsigned> UsingGpus;
	enum class Status_t {Pending, Running, Finished} Status;
	bool RunInContainer;
	bool RunNow;

	template <class Archive> void serialize(Archive & ar)
	{
		ar(Id, User, Program, Comment, Environment, Arguments, UsingCores, UsingGpus, Status, RunInContainer, RunNow);
	}
};

struct Input_t
// 客户端发送给服务端的信息
{
	std::vector<Job_t> NewJobs;	// 写入时, Id 填 0, User 留空, Status 填 pending; 读取时 User 填实际值, Status 填 Pending
	std::vector<std::pair<unsigned, std::string>> RemoveJobs;	// 第二个位置写用户名, 写时留空, 读取时填实际值

	template <class Archive> void serialize(Archive & ar)
	{
		ar(NewJobs, RemoveJobs);
	}
};

struct Output_t
// 服务端发送给客户端的信息
{
	std::vector<Job_t> Jobs;
	template <class Archive> void serialize(Archive & ar)
	{
		ar(Jobs);
	}
};

inline void write_in(Input_t input)
{
	boost::interprocess::file_lock in_lock{"/tmp/gpujob/in.lock"};
	in_lock.lock();
	unsigned i = 0;
	while (std::filesystem::exists(fmt::format("/tmp/gpujob/in/{}", i)))
		i++;
	{
		std::ofstream out{fmt::format("/tmp/gpujob/in/{}", i)};
		cereal::JSONOutputArchive{out}(input);
	}
}

inline std::optional<Input_t> read_in()
{
	auto get_owner = [](std::string filename) -> std::optional<std::string>
	{
		struct stat info;
		if (stat(filename.c_str(), &info))
			return {};
		struct passwd *pw = getpwuid(info.st_uid);
		if (pw)
			return pw->pw_name;
		else
			return {};
	};

	boost::interprocess::file_lock in_lock{"/tmp/gpujob/in.lock"};
	in_lock.lock();
	std::optional<Input_t> result;
	for (auto & p : std::filesystem::directory_iterator("/tmp/gpujob/in"))
	{
		try
		{
			if (p.is_regular_file())
			{
				Input_t input;
				{
					std::ifstream in{p.path()};
					cereal::JSONInputArchive{in}(result);
				}
				if (auto owner = get_owner(p.path()); owner)
				{
					if (!result)
						result.emplace();
					for (auto& job : input.NewJobs)
					{
						job.User = *owner;
						job.Status = Job_t::Status_t::Pending;
						result->NewJobs.push_back(job);
					}
					for (auto& job : input.RemoveJobs)
					{
						job.second = *owner;
						result->RemoveJobs.push_back(job);
					}
				}
				else
					continue;
				std::filesystem::remove(p.path());
			}
		}
		catch (...) {}
	}
	return result;
}

inline void write_out(Output_t output)
{
	boost::interprocess::file_lock out_lock{"/tmp/gpujob/out.lock"};
	out_lock.lock();
	{
		std::ofstream out{"/tmp/gpujob/out.dat"};
		cereal::JSONOutputArchive{out}(output);
	}
}

inline Output_t read_out()
{
	boost::interprocess::file_lock out_lock{"/tmp/gpujob/out.lock"};
	out_lock.lock();
	Output_t result;
	{
		std::ifstream in{"/tmp/gpujob/out.dat"};
		cereal::JSONInputArchive{in}(result);
	}
	return result;
}
