# include <job.hpp>
# include <pwd.h>
# include <grp.h>
# include <sys/stat.h>
# include <optional>

using namespace std::literals;

inline void create_files()
// jobd 启动时，创建需要的文件和目录，并且设定好权限。
{
	if (!std::filesystem::exists("/tmp/gpujob"))
	{
		std::filesystem::create_directory("/tmp/gpujob");
		std::filesystem::permissions("/tmp/gpujob", std::filesystem::perms::all);
	}
	if (!std::filesystem::exists("/tmp/gpujob/in"))
	{
		std::filesystem::create_directory("/tmp/gpujob/in");
		std::filesystem::permissions("/tmp/gpujob/in", std::filesystem::perms::all);
	}
	if (!std::filesystem::exists("/tmp/gpujob/in.lock"))
	{
		std::ofstream("/tmp/gpujob/in.lock");
		std::filesystem::permissions
		(
			"/tmp/gpujob/in.lock",
			std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
			std::filesystem::perms::group_read | std::filesystem::perms::group_write |
			std::filesystem::perms::others_read | std::filesystem::perms::others_write
		);
	}
	if (!std::filesystem::exists("/tmp/gpujob/out.lock"))
	{
		std::ofstream("/tmp/gpujob/out.lock");
		std::filesystem::permissions
		(
			"/tmp/gpujob/out.lock",
			std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
			std::filesystem::perms::group_read | std::filesystem::perms::group_write |
			std::filesystem::perms::others_read | std::filesystem::perms::others_write
		);
	}
	if (!std::filesystem::exists("/tmp/gpujob/out.dat"))
	{
		std::ofstream("/tmp/gpujob/out.dat");
		std::filesystem::permissions
		(
			"/tmp/gpujob/out.dat",
			std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
			std::filesystem::perms::group_read | std::filesystem::perms::group_write |
			std::filesystem::perms::others_read | std::filesystem::perms::others_write
		);
		std::vector<job> jobs;
		std::vector<std::string> gpus;
		{
			std::ofstream os("/tmp/gpujob/out.dat");
			cereal::JSONOutputArchive archive(os);
			archive(jobs, gpus);
		}
	}
}

inline std::tuple<std::vector<job>, std::vector<std::pair<unsigned, std::string>>> read_input_files()
// 读入来自 job 的输入文件，步骤如下：
//	* 加锁
// 	* 依次从 /tmp/gpujob/in 读入文件。读入的过程中，需要设定任务的所有者和状态。
// 	* 然后删除 /tmp/gpujob/in 中的文件。
{
	boost::interprocess::file_lock in_lock{"/tmp/gpujob/in.lock"};
	in_lock.lock();
	std::vector<job> all_jobs;
	std::vector<std::pair<unsigned, std::string>> all_remove_jobs;

	for (const auto& entry : std::filesystem::directory_iterator("/tmp/gpujob/in"))
		if (entry.is_regular_file())
		{
			std::vector<job> jobs;
			std::vector<unsigned> remove_jobs;

			// get owner of the file
			std::optional<std::string> owner;
			{
				struct stat info;
				auto path = entry.path();
				stat(path.c_str(), &info);
				struct passwd *pw = getpwuid(info.st_uid);
				if (pw)
					owner.emplace(pw->pw_name);
			}

			// read content of file
			{
				std::ifstream is(entry.path());
				cereal::JSONInputArchive archive(is);
				archive(jobs, gpus);
			}
			std::filesystem::remove(entry.path());

			for (auto& job : jobs)
			{
				job.owner = owner;
				job.status = job_status::waiting;
				all_jobs.emplace_back(std::move(job));
			}
			for (auto& job : remove_jobs)
				all_remove_jobs.emplace_back(job, owner);
		}
	return {all_jobs, all_remove_jobs};
}

int main()
{
	try
	{
		std::array<std::shared_ptr<boost::process::child>, 3> tasks;
		std::vector<job> jobs;
		unsigned next_id = 0;
		bool jobs_changed = true;

		create_files();

		while (true)
		{
			std::this_thread::sleep_for(1s);

			// read new jobs from in.dat
			if (std::filesystem::exists("/tmp/gpujob/in.modified"))
			{
				auto [new_jobs, remove_jobs] = read_in();

				for (auto& job : new_jobs)
				{
					job.id = next_id++;
					job.state = job::status::pending;
					jobs.push_back(job);
					std::clog << fmt::format("new job: {} {} {} {} {} {} {}\n",
						job.id, job.assign_to, job.script_id, job.user, job.path, job.command, nameof::nameof_enum(job.state));
					auto message = fmt::format("notify 'add {} {}'", job.id, job.path);
					std::system(message.c_str());
				}
				for (auto& job : remove_jobs)
				{
					auto it = std::find_if(jobs.begin(), jobs.end(), [&](auto& j){return j.id == job;});
					if (it != jobs.end())
					// if (it != jobs.end() && it->state == job::status::pending)
					{
						if (it->state == job::status::running)
						{
							auto pid = tasks[it->assign_to]->id();
							tasks[it->assign_to]->detach();
							tasks[it->assign_to].reset();
							std::clog << fmt::format("kill job: {} {}\n", it->id, pid);
							auto command = fmt::format("rkill {}", pid);
							std::system(command.c_str());
						}
						it->state = job::status::finished;
						std::clog << fmt::format("remove job {} success\n", job);
						auto message = fmt::format("notify 'delete {} {}'", it->id, it->path);
						std::system(message.c_str());
					}
					else
						std::clog << fmt::format("remove job {} not found\n", job);
				}
				jobs_changed = true;
			}

			// assign new jobs
			for (auto& task : tasks)
			{
				if (!task || !task->running())
				{
					if (task)
					{
						for (auto& job : jobs)
							if (job.assign_to == &task - tasks.data() && job.state == job::status::running)
							{
								job.state = job::status::finished;
								auto message = fmt::format("notify 'finish {} {}'", job.id, job.path);
								std::system(message.c_str());
							}
						task.reset();
						jobs_changed = true;
					}
					auto it = std::find_if(jobs.begin(), jobs.end(),
						[&](auto& j){return j.state == job::status::pending && j.assign_to == &task - tasks.data();});
					if (it != jobs.end())
					{
						it->state = job::status::running;
						// replace " with \"
						std::string command = it->command;
						for (std::size_t i = 0; i < command.size(); i++)
							if (command[i] == '"')
								command.insert(i++, 1, '\\');
						task = std::make_shared<boost::process::child>(fmt::format
							(
								R"(su - {} -c "cd {} && CUDA_VISIBLE_DEVICES={} {} > {} 2>&1")",
								it->user, it->path, it->assign_to, command, "output.txt"
							));
						auto message = fmt::format("notify 'start {} {}'", it->id, it->path);
						std::system(message.c_str());
						jobs_changed = true;
					}
				}
			}

			// write jobs to out.dat
			if (jobs_changed)
			{
				write_out(jobs);
				jobs_changed = false;
			}
		}
	}
	catch (std::exception const& e)
	{
		std::cerr << e.what() << std::endl;
	}
	catch (...)
	{
		std::cerr << "Unknown exception" << std::endl;
	}
}
