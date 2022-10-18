# include "job.hpp"

using namespace std::literals;

void print_help()
{
	std::cerr << R"(
Usage: 
	job {s,submit} [gpuid [script_id [custom script]]]
	job {q,query}
	job {c,cancel} [job_id]
)";
}

int main(int argc, const char** argv)
{
	try
	{
		if (argc == 1)
		{
			print_help();
			return 1;
		}
		std::vector<std::string> args{argv + 1, argv + argc};
		if (args[0] == "s" || args[0] == "submit")
		{
			unsigned gpuid;
			if (args.size() >= 2)
			{
				if (std::regex_match(args[1], std::regex{"[0-2]"}))
					gpuid = std::stoi(args[1]);
				else
					throw std::runtime_error{fmt::format("Invalid gpuid: {}", args[1])};
			}
			else
			{
				auto gpus = queue_gpu_name();
				if (gpus.size() != 3)
					throw std::runtime_error{"Invalid number of gpus"};
				auto out = read_out();
				std::array<unsigned, 3> runing;
				std::array<unsigned, 3> pending;
				for (auto& job : out)
				{
					if (job.state == job::status::running)
						runing[job.assign_to]++;
					else if (job.state == job::status::pending)
						pending[job.assign_to]++;
				}
				for (auto i = 0; i < gpus.size(); ++i)
					std::cout << fmt::format("\t{} [{}]: {} running, {} pending\n", i, gpus[i], runing[i], pending[i]);
				std::cout << "你想要哪张显卡给你干活，输入 0-3 按回车，按 Ctrl+C 取消: ";
				std::cin >> gpuid;
				if (gpuid > 2 || std::cin.fail())
					throw std::runtime_error{fmt::format("Invalid gpuid: {}", gpuid)};
			}

			unsigned script_id;
			if (args.size() >= 3)
			{
				if (std::regex_match(args[2], std::regex{"[0-3]"}))
					script_id = std::stoi(args[2]);
				else
					throw std::runtime_error{fmt::format("Invalid script_id: {}", args[2])};
			}
			else
			{
				std::cout << "\t0) VASP 6.3.0 std\t\t1) VASP 6.3.0 gam\n";
				std::cout << "\t2) VASP 6.3.1 std\t\t3) VASP 6.3.1 gam\n";
				std::cout << "\t4) 自定义脚本（别玩脱了）\n";
				std::cout << "想要使用哪个版本的 VASP，输入 0-4 按回车，按 Ctrl+C 取消: ";
				std::cin >> script_id;
				if (script_id > 4 || std::cin.fail())
					throw std::runtime_error{fmt::format("Invalid script_id: {}", script_id)};
			}

			std::string script;
			if (args.size() == 4)
			{
				if (args.size() == 4)
					script = args[3];
				else
				{
					std::cout << "输入你的脚本，按 Ctrl+C 取消: ";
					std::cin >> script;
					if (std::cin.fail())
						throw std::runtime_error{fmt::format("Invalid script: {}", script)};
				}
			}
			else
			{
				script = std::vector<std::string>
				{
					"vasp_gpu_std_630.sh", "vasp_gpu_gam_630.sh",
					"vasp_gpu_std_631.sh", "vasp_gpu_gam_631.sh"
				}[script_id];
			}

			auto user = std::getenv("USER");
			if (!user)
				throw std::runtime_error{"Can't get user name"};
			job j
			{
				.id = 0, .assign_to = gpuid, .script_id = script_id, .user = user,
				.path = std::filesystem::current_path().string(), .command = script, .state = job::status::pending
			};

			append_in({j}, {});
			std::cout << "提交成功，可能需要几秒才会出现在列表中。\n" << std::endl;
		}
		else if (args[0] == "q" || args[0] == "query")
		{
			auto gpus = queue_gpu_name();
			if (gpus.size() != 3)
				throw std::runtime_error{"Invalid number of gpus"};
			auto out = read_out();
			// remove finished jobs
			for (std::size_t i = 0; i < out.size(); i++)
			{
				if (out[i].state == job::status::finished)
				{
					out.erase(out.begin() + i);
					i--;
				}
			}
			// sort by assign_to
			std::sort(out.begin(), out.end(), [](const job& a, const job& b) { return a.assign_to < b.assign_to; });
			if (out.empty())
				std::cout << "No job but touching fish.\n" << std::endl;
			else
			{
				std::cout << "id\tuser\tscript_name\tgpu name\tstatus\tpath\n";
				for (auto& job : out)
					std::cout << fmt::format
					(
						"{}\t{}\t{}\t{}\t{}\t{}\n", job.id, job.user,
						std::vector<std::string>
							{"6.3.0-std", "6.3.0-gam", "6.3.1-std", "6.3.1-gam", "custom"}[job.script_id],
							gpus[job.assign_to], nameof::nameof_enum(job.state), job.path
					);
			}
		}
		else if (args[0] == "c" || args[0] == "cancle")
		{
			auto user = std::getenv("USER");
			if (!user)
				throw std::runtime_error{"Can't get user name"};

			unsigned id;
			if (args.size() >= 2)
			{
				if (std::regex_match(args[1], std::regex{"[0-9]+"}))
					id = std::stoi(args[1]);
				else
					throw std::runtime_error{fmt::format("Invalid id: {}", args[1])};
			}
			else
			{
				auto jobs = read_out();
				// remove jobs not belong to user or finished
				for (std::size_t i = 0; i < jobs.size(); i++)
				{
					if (jobs[i].user != user || jobs[i].state == job::status::finished)
					{
						jobs.erase(jobs.begin() + i);
						i--;
					}
				}
				if (jobs.empty())
					std::cout << "没有任务可以取消。\n" << std::endl;
				else
				{
					std::cout << "id\tscript_name\tgpu name\tstatus\tpath\n";
					for (auto& job : jobs)
						if (job.user == user)
							std::cout << fmt::format
							(
								"{}\t{}\t{}\t{}\t{}\n", job.id,
								std::vector<std::string>
									{"6.3.0-std", "6.3.0-gam", "6.3.1-std", "6.3.1-gam", "custom"}[job.script_id],
									queue_gpu_name()[job.assign_to], nameof::nameof_enum(job.state), job.path
							);
					std::cout << "\n";
					std::cout << "输入你要取消的任务的 id，按 Ctrl+C 取消取消: ";
					std::cin >> id;
					if (std::cin.fail())
						throw std::runtime_error{fmt::format("Invalid id: {}", id)};
					// check if id exist in job
					bool exist = false;
					for (auto& job : jobs)
						if (job.id == id)
						{
							if (job.state == job::status::running)
								throw std::runtime_error{"已经开始运行的任务暂时取消不了，你可以自己去杀。"};
							exist = true;
							break;
						}
					if (!exist)
						throw std::runtime_error{fmt::format("Invalid id: {}", id)};
					append_in({}, {id});
					std::cout << "取消成功。\n" << std::endl;
				}
			}
		}
		else
			throw std::runtime_error{fmt::format("Invalid command: {}", args[0])};
	}
	catch (const std::exception& e)
	{
		std::cerr << e.what() << std::endl;
		return 1;
	}
	catch (...)
	{
		std::cerr << "Unknown error" << std::endl;
		return 1;
	}
}
