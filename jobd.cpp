# include "job.hpp"

using namespace std::literals;

int main()
{
	try
	{
		std::array<std::shared_ptr<boost::process::child>, 3> tasks;
		std::vector<job> jobs;
		unsigned next_id = 0;
		bool jobs_changed = false;

		while (true)
		{
			std::this_thread::sleep_for(1s);

			// read new jobs from in.dat
			if (std::filesystem::exists("/tmp/gpujob/in.dat"))
			{
				auto [new_jobs, remove_jobs] = read_in();

				for (auto& job : new_jobs)
				{
					job.id = next_id++;
					job.state = job::status::pending;
					jobs.push_back(job);
				}
				for (auto& job : remove_jobs)
				{
					auto it = std::find_if(jobs.begin(), jobs.end(), [&](auto& j){return j.id == job;});
					if (it != jobs.end())
					{
						if (it->state == job::status::running)
						{
							tasks[it->assign_to]->terminate();
							tasks[it->assign_to]->wait();
							tasks[it->assign_to].reset();
						}
						jobs.erase(it);
					}
				}
				jobs_changed = true;
			}

			// assign new jobs
			for (auto& task : tasks)
			{
				if (!task)
				{
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
						task = std::make_shared<boost::process::child>
							(fmt::format(R"(runuser {} -c "cd {} && {} > {} 2>&1")",
								it->user, it->path, command, "output.txt"));
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
