# include <iostream>
# include <vector>
# include <filesystem>
# include <string>
# include <map>
# include <regex>
# include <ftxui/dom/elements.hpp>
# include <ftxui/component/component.hpp>
# include <ftxui/component/screen_interactive.hpp>
# include <ftxui/dom/elements.hpp>
# include <ftxui/screen/screen.hpp>
# include <ftxui/screen/string.hpp>
# include <boost/interprocess/sync/file_lock.hpp>
# include <boost/process.hpp>
# include <fmt/format.h>
# include <job.hpp>

void print_help()
{
	std::cerr << R"(
Usage: 
	job {s,submit}
	job {q,query}
	job {c,cancel}
)";
}

std::map<unsigned, std::string> detect_gpu_devices()
// source nvhpc，然后使用 pgaccelinfo -l 列出所有的 GPU 设备。
// 并不是所有的 GPU 都会列出。只有 NVIDIA GeForce RTX 开头的会被列出。
{
	std::map<unsigned, std::string> devices;
	boost::process::ipstream command_output;
	boost::process::child command
        {R"(bash -c ". nvhpc && pgaccelinfo | grep 'Device Name'")", boost::process::std_out > command_output};
	std::string a_single_line;
    unsigned i = 0;
	while (command.running() && std::getline(command_output, a_single_line) && !a_single_line.empty())
	{
		std::regex re{"Device Name:\\s+NVIDIA GeForce RTX (.+)"};
		std::smatch match;
		if (std::regex_search(a_single_line, match, re))
			devices[i] = match[1];
        i++;
	}
	command.wait();
	return devices;
}

inline std::vector<job> read_output_files()
// 读取来自 jobd 的输出文件
{
	boost::interprocess::file_lock out_lock{"/tmp/gpujob/out.lock"};
	out_lock.lock();
	std::vector<job> jobs;
	{
		std::ifstream in{"/tmp/gpujob/out.dat"};
		cereal::JSONInputArchive{in}(jobs);
	}
	return jobs;
}

auto request_new_job_detail_from_user
	(std::vector<std::tuple<std::optional<unsigned>, std::string, unsigned, unsigned>> devices)
// 展示提交任务的界面，并等待用户输入、确认。所有参数使用字符串传回。保证传回的结果已经被检查过，不需要再次检查。
// 传入的参数依次是：每个设备的 id、名称、正在运行的任务数、正在等待的任务数。CPU 的 id 为空。
// 返回的字典中包括以下参数：
//  基本信息：
//      "Canceled": 存在这个键时，表示用户取消了提交任务，这时以下参数都可以不存在。
//      "Program": "Vasp" 或 "Custom"，表明用户选择的程序。
//      "VaspVersion": "6.3.1"，表明用户选择的 VASP 版本。只当用户选择 VASP 时存在。
//      "VaspVariant": "std" 或 "gam" 或 "ncl"，表明用户选择的 VASP 变体。只当用户选择 VASP 时存在。
//      "Device": 用户选择的设备，与传入的参数对应。空 id 则对应空字符串。
//      "MpiThreads": 用户选择的 MPI 线程数，为正整数。只当用户选择 CPU 且选择 VASP 时存在。
//      "OpenmpThreads": 用户选择的 OpenMP 线程数，为正整数。只当用户选择 CPU 且选择 VASP 时存在。
//      "CustomCommand": 用户选择的自定义命令。只当用户选择自定义命令时存在。不为空字符串。
//      "CustomCommandCores": 用户选择的自定义命令的 CPU 核心数，为正整数。只当用户选择自定义命令时存在。
//  高级设置：
//      "CustomPath": 存在这个键时，表示用户选择了自定义路径，这个键对应的值是用户输入的路径。不为空字符串。
//      "CustomOpenmpThreads": 用户选择的 OpenMP 线程数，为正整数。只当用户选择 GPU 且选择 VASP 时存在。
//      "RunNow": 立即开始运行。
{
    auto screen = ftxui::ScreenInteractive::Fullscreen();

	struct
	{
		// 基本信息
		std::vector<std::string> program_names {"VASP", "自定义程序"};
		std::vector<std::string> program_internal_names {"Vasp", "Custom"};
		int program_selected = 0;
		std::vector<std::string> vasp_version_names {"6.3.1"};
		int vasp_version_selected = 0;
		std::vector<std::string> vasp_variant_names {"std", "gam", "ncl"};
		int vasp_variant_selected = 0;
		auto device_names = [&]
		{
			std::vector<std::string> device_names;
			for (auto& [device_id, device_name, device_running, device_pending] : devices)
				device_names.push_back(fmt::format(
					"{}: {} ({}{} 运行, {} 等待)",
					device_id ? "GPU" : "CPU", device_name, device_id ? fmt::format("ID: {}, ", *device_id) : "",
					device_running, device_pending
				));
			return device_names;
		}();
		int device_selected = 0;
		std::string mpi_threads_text = "4";
		std::string openmp_threads_text = "4";
		std::string custom_command_text = "/bin/bash echo hello world";
		std::string custom_command_cores_text = "4";

		// 高级设置
		bool custom_path_checked = false;
		std::string custom_path_text = std::filesystem::current_path();
		bool custom_openmp_threads_checked = false;
		std::string custom_openmp_threads_text = "2";
		bool run_now_checked = false;
		bool run_in_container_checked = false;

		bool canceled = false;
	} config;

    auto layout = ftxui::Container::Vertical
    ({
        // 基本信息
        ftxui::Container::Vertical
        ({
            // 几个 Dropdown
            ftxui::Container::Horizontal
            ({
                ftxui::Dropdown(&config.program_names, &config.program_selected),
                ftxui::Container::Horizontal
                ({
                    ftxui::Dropdown(&config.vasp_version_names, &config.vasp_version_selected),
                    ftxui::Dropdown(&config.vasp_variant_names, &config.vasp_variant_selected)
                }) | ftxui::Maybe([&]{return config.program_internal_names[config.program_selected] == "Vasp";}),
                ftxui::Dropdown(&config.device_names, &config.device_selected)
            }),
            // 其它一些信息
            ftxui::Container::Vertical
            ({
                ftxui::Container::Vertical
                ({
                    ftxui::Input(&config.mpi_threads_text, "") | ftxui::underlined
                        | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3) | ftxui::Renderer([&](ftxui::Element inner)
                            {return ftxui::hbox(ftxui::text("MPI 线程数: "), inner);})
                        | ftxui::flex_shrink,
                    ftxui::Input(&config.openmp_threads_text, "") | ftxui::underlined
                        | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3) | ftxui::Renderer([&](ftxui::Element inner)
                            {return ftxui::hbox(ftxui::text("OpenMP 线程数: "), inner);})
                        | ftxui::flex_shrink
                }) | ftxui::Maybe([&]
                {
                    return config.program_internal_names[config.program_selected] == "Vasp"
                        && std::get<0>(devices[config.device_selected]) == std::nullopt;
                }),
                ftxui::Container::Vertical
                ({
                    ftxui::Input(&config.custom_command_text, "") | ftxui::underlined
                        | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 30) | ftxui::Renderer([&](ftxui::Element inner)
                            {return ftxui::hbox(ftxui::text("自定义命令: "), inner);})
                        | ftxui::flex_shrink,
                    ftxui::Input(&config.custom_command_cores_text, "") | ftxui::underlined
                        | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3) | ftxui::Renderer([&](ftxui::Element inner)
                            {return ftxui::hbox(ftxui::text("占用 CPU 核心数: "), inner);})
                        | ftxui::flex_shrink
                }) | ftxui::Maybe([&]{return config.program_internal_names[config.program_selected] == "Custom";})
            })
        }) | ftxui::Renderer([&](ftxui::Element inner)
            {return ftxui::vbox(ftxui::text("基本信息") | ftxui::bgcolor(ftxui::Color::Blue), inner);}),
        // 高级设置
        ftxui::Container::Vertical
        ({
            ftxui::Container::Horizontal
            ({
                ftxui::Checkbox("自定义路径：", &config.custom_path_checked),
                ftxui::Input(&config.custom_path_text, "") | ftxui::underlined
                    | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 30)
                    | ftxui::flex_shrink | ftxui::Maybe([&]{return config.custom_path_checked;})
            }),
            ftxui::Container::Horizontal
            ({
                ftxui::Checkbox("自定义 OpenMP 线程数：", &config.custom_openmp_threads_checked),
                ftxui::Input(&config.custom_openmp_threads_text, "") | ftxui::underlined
                    | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3)
                    | ftxui::flex_shrink | ftxui::Maybe([&]{return config.custom_openmp_threads_checked;})
            }) | ftxui::Maybe([&]
            {
                return config.program_internal_names[config.program_selected] == "Vasp"
                    && std::get<0>(devices[config.device_selected]) != std::nullopt;
            }),
            ftxui::Checkbox("立即开始运行", &config.run_now_checked),
            ftxui::Checkbox("在 Ubuntu 22.04 容器中运行", &config.run_in_container_checked)
                | ftxui::Maybe([&]{return config.program_internal_names[config.program_selected] == "Custom";})
        }) | ftxui::Renderer([&](ftxui::Element inner)
                {return ftxui::vbox(ftxui::text("高级设置") | ftxui::bgcolor(ftxui::Color::Blue), inner);}),
        // "几个按钮"
        ftxui::Container::Horizontal
		({
			ftxui::Button("提交任务（Enter）", screen.ExitLoopClosure()),
			ftxui::Button("取消", [&]{config.canceled = true; screen.ExitLoopClosure()();})
		})
			| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());})
    }) | ftxui::Renderer([&](ftxui::Element inner){return ftxui::window(ftxui::text("提交新任务"), inner);})
        | ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());})
        | ftxui::Renderer([&](ftxui::Element inner){return ftxui::vbox(inner, ftxui::filler(), ftxui::hbox
            (
                ftxui::filler(), ftxui::text("Code by CHN with "),
                ftxui::text("❤️") | ftxui::color(ftxui::Color::Red),
                ftxui::text(" love.")
            ) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1));});
    screen.Loop(layout);
    return config;
}

int main(int argc, const char** argv)
{
	try
	{
		if (argc == 1 || argc > 2)
		{
			print_help();
			return 1;
		}
		auto action = std::string{argv[1]};
		if (action == "s" || action == "submit")
		{
            std::vector<std::tuple<std::optional<unsigned>, std::string, unsigned, unsigned>> devices;

            // fill device list
            {
                devices.push_back({std::nullopt, "R9-5950X", 0, 0});
                auto gpus = detect_gpu_devices();
                for (auto& gpu : gpus)
                    devices.push_back({gpu.first, gpu.second, 0, 0});
                auto jobs = read_output_files();
                for (auto& job : jobs)
                {
                    auto device = std::find_if(devices.begin(), devices.end(), [&](auto& device)
                    {
                        return std::get<0>(device) == job.device_id;
                    });
                    if (device != devices.end())
                    {
                        if (job.state == job::status::running)
                            std::get<2>(*device) += 1;
                        else if (job.state == job::status::pending)
                            std::get<3>(*device) += 1;
                    }
                }
            }

            // get user input
            auto user_input = request_new_job_detail_from_user(devices);

			if (!user_input.canceled)
			{
				job job;
				job.id = 0;
				job.assign_to = std::get<0>(devices[user_input.device_selected]);
				if (user_input.program_internal_names[user_input.program_selected] == "Vasp")
				{
					auto version = user_input.vasp_version_names[user_input.vasp_version_selected];
					std::erase(version, '.');
					auto variant = user_input.vasp_variant_names[user_input.vasp_variant_selected];
					job.command = fmt::format("vasp_{}_{}_{}.sh", job.assign_to ? "gpu" : "cpu", variant, version);
					job.run_in_container
				}
			}

            
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
				auto [jobs, gpus] = read_out();
				if (gpus.size() != 3)
					throw std::runtime_error{"Invalid number of gpus"};

				std::array<unsigned, 3> runing = {};
				std::array<unsigned, 3> pending = {};
				for (auto& job : jobs)
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
			auto [jobs, gpus] = read_out();
			if (gpus.size() != 3)
				throw std::runtime_error{"Invalid number of gpus"};

			// remove finished jobs
			for (std::size_t i = 0; i < jobs.size(); i++)
			{
				if (jobs[i].state == job::status::finished)
				{
					jobs.erase(jobs.begin() + i);
					i--;
				}
			}
			// sort by assign_to
			std::sort(jobs.begin(), jobs.end(), [](const job& a, const job& b) { return a.assign_to < b.assign_to; });
			if (jobs.empty())
				std::cout << "No job but touching fish.\n" << std::endl;
			else
			{
				std::cout << "id\tuser\tscript_name\tgpu name\tstatus\tpath\n";
				for (auto& job : jobs)
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
				auto [jobs, gpus] = read_out();
				if (gpus.size() != 3)
					throw std::runtime_error{"Invalid number of gpus"};
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
									gpus[job.assign_to], nameof::nameof_enum(job.state), job.path
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
							// if (job.state == job::status::running)
								// throw std::runtime_error{"已经开始运行的任务暂时取消不了，你可以自己去杀。"};
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


int main()
{
    request_new_job_detail_from_user
    ({
        {std::nullopt, "R9 5950X", 0, 0},
        {0, "3090", 1, 2},
        {1, "2080 Ti", 1, 3}
    });
}
