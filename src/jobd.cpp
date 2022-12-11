# include <set>
# include <job.hpp>
# include <boost/process.hpp>
# include <nameof.hpp>

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
		Output_t out;
		write_out(out);
	}
}

int main()
{
	try
	{
		std::vector<Job_t> jobs;
		std::map<unsigned, std::unique_ptr<boost::process::child>> tasks;
		unsigned next_id = 0;

		auto notify = [](std::string comment){boost::process::child{"/usr/local/bin/notify", comment}.detach();};

		create_files();

		while (true)
		{
			std::this_thread::sleep_for(1s);

			bool jobs_changed = false;

			// read new jobs
			if (auto input = read_in())
			{
				for (auto& job : input->NewJobs)
				{
					job.Id = next_id++;
					jobs.push_back(job);
					std::clog << fmt::format
					(
						"new job: {} {} {} {} {} {} {} {} {} {} {}\n",
						job.Id, job.User, job.Program, job.Comment, job.Environment, job.Arguments, job.UsingCores,
						job.UsingGpus, nameof::nameof_enum(job.Status), job.RunInContainer, job.RunNow
					);
					notify(fmt::format("new job: {} {}", job.Id, job.Comment));
				}
				for (auto& job : input->RemoveJobs)
				{
					auto it = std::find_if(jobs.begin(), jobs.end(), [&](auto& j){return j.Id == job.first;});
					if (it != jobs.end())
					{
						if (it->User == job.second)
						{
							if (it->Status == Job_t::Status_t::Running)
							{
								auto pid = tasks[it->Id]->id();
								tasks[it->Id]->detach();
								tasks[it->Id].reset();
								std::clog << fmt::format("kill job: {} {}\n", it->Id, pid);
								auto command = fmt::format("rkill {}", pid);
								std::system(command.c_str());
							}
							it->Status = Job_t::Status_t::Finished;
							std::clog << fmt::format("remove job {} success\n", job);
							notify(fmt::format("remove job: {} {}", it->Id, it->Comment));
						}
					}
					else
						std::clog << fmt::format("remove job {} not found\n", job);
				}
				jobs_changed = true;
			}

			// check if jobs finished
			for (auto& task : tasks)
				if (!task.second->running())
				{
					auto it = std::find_if(jobs.begin(), jobs.end(), [&](auto& job){return job.Id == task.first;});
					if (it != jobs.end())
					{
						it->Status = Job_t::Status_t::Finished;
						std::clog << fmt::format("job {} finished\n", it->Id);
						notify(fmt::format("finish job: {} {}", it->Id, it->Comment));
					}
					else
						std::unreachable();
					task.second.reset();
					jobs_changed = true;
				}
			std::erase_if(tasks, [](auto& task){return !task.second;});

			// assign new jobs
			if (jobs_changed)
			{
				std::set<unsigned> gpu_used;
				unsigned cpu_used;
				auto rebuild_usage_statistic = [&]
				{
					gpu_used.clear();
					cpu_used = 0;
					for (auto& task : tasks)
					{
						auto it = std::find_if(jobs.begin(), jobs.end(), [&](auto& job){return job.Id == task.first;});
						if (it != jobs.end())
						{
							for (auto gpu : it->UsingGpus)
								gpu_used.insert(gpu);
							cpu_used += it->UsingCores;
						}
						else
							std::unreachable();
					}
				};
				rebuild_usage_statistic();

				for (auto& job : jobs)
					if (job.Status == Job_t::Status_t::Pending)
					{
						if ((!std::ranges::any_of(job.UsingGpus, [&](auto gpu){return gpu_used.contains(gpu);})
							&& cpu_used + job.UsingCores <= std::thread::hardware_concurrency()) || job.RunNow)
						{
							// systemd-run -M root@.host -P -q -E ENV_NAME sleep 5
							std::vector<std::string> args = 
							{
								"-M"s, fmt::format("{}@{}", job.User, job.RunInContainer ? "ubuntu-22.04"s : ".host"s ),
								"-P"s, "-q"s, "--user"s,
							};
							for (auto& [name, value] : job.Environment)
								args.push_back(fmt::format("--setenv={}", name));
							args.push_back(job.Program);
							args.insert(args.end(), job.Arguments.begin(), job.Arguments.end());

							boost::process::environment env = boost::this_process::environment();
							for (auto& [name, value] : job.Environment)
								env[name] = value;

							std::clog << fmt::format("run job args: {}\n", args);
							tasks[job.Id] = std::make_unique<boost::process::child>
								(boost::process::search_path("systemd-run"), boost::process::args(args), env);

							std::clog << fmt::format("run job: {} {}\n", job.Id, job.Comment);
							notify(fmt::format("run job: {} {}", job.Id, job.Comment));
							job.Status = Job_t::Status_t::Running;
							rebuild_usage_statistic();
						}
					}
			}

			// write jobs to out.dat
			if (jobs_changed)
				write_out({jobs});
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
