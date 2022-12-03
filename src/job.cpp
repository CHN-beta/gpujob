# include <iostream>
# include <vector>
# include <filesystem>
# include <string>
# include <map>
# include <regex>
# include <variant>
# include <fstream>
# include <ftxui/dom/elements.hpp>
# include <ftxui/component/component.hpp>
# include <ftxui/component/screen_interactive.hpp>
# include <ftxui/dom/elements.hpp>
# include <ftxui/screen/screen.hpp>
# include <ftxui/screen/string.hpp>
# include <boost/interprocess/sync/file_lock.hpp>
# include <boost/process.hpp>
# include <fmt/format.h>
# include <cereal/archives/json.hpp>
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

std::map<std::string, std::variant<std::string, unsigned, bool, std::vector<unsigned>>>
	request_new_job_detail_from_user(std::vector<std::tuple<unsigned, std::string, unsigned, unsigned>> gpu_devices)
// 展示提交任务的界面，并等待用户输入、确认。所有参数使用字符串传回。保证传回的结果已经被检查过，不需要再次检查。
// 传入的参数依次是：每个 GPU 设备的 id、名称、正在运行的任务数、正在等待的任务数。
// 返回的字典中包括以下参数：
// 	"Canceled": 布尔值，表示用户取消了提交任务，这时以下参数都可以不存在。
// 	"Program": 字符串值，"vasp" 或 "lammps" 或 "custom"，表明用户选择的程序。
// 	"VaspVersion": 字符串值，"620-vtst" 或 "631"，表明用户选择的 VASP 版本。只当 "Program" 为 "vasp" 时存在。
// 	"VaspVariant": 字符串值，"std" 或 "gam" 或 "ncl"，表明用户选择的 VASP 变体。只当 "Program" 为 "vasp" 时存在。
// 	"GpuDevice": 数组，用户选择的 GPU 设备的 id，与传入的参数对应。
// 	"MpiThreads": 正整数，用户选择的 MPI 线程数。只当 "Program" 为 "vasp" 或 "lammps" 时存在。
// 	"OpenmpThreads": 正整数，用户选择的 OpenMP 线程数。只当 "Program" 为 "vasp" 或 "lammps" 时存在。
// 	"CustomCommand": 字符串值，用户输入的自定义命令。只当 "Program" 为 "custom" 时存在。
// 	"CustomCommandCores": 正整数，用户选择的自定义命令的 CPU 核心数。只当 "Program" 为 "custom" 时存在。
// 	"Path": 字符串值，表示运行的路径。
// 	"RunNow": 布尔值，表示不排队、立即开始运行。
{
    auto screen = ftxui::ScreenInteractive::Fullscreen();

	// 基本信息
	std::vector<std::string> program_names {"VASP", "LAMMPS", "自定义程序"};
	std::vector<std::string> program_internal_names {"vasp", "lammps", "custom"};
	int program_selected = 0;
	std::vector<std::string> vasp_version_names {"6.3.1"};
	int vasp_version_selected = 0;
	std::vector<std::string> vasp_variant_names {"std", "gam", "ncl"};
	int vasp_variant_selected = 0;
	bool gpu_device_use_checked = false;
	std::vector<std::pair<std::string, bool>> gpu_device_checked;
	for (auto& [device_id, device_name, device_running, device_pending] : gpu_devices)
		gpu_device_checked.emplace_back(fmt::format("{} (ID: {}, {} 运行, {} 等待)",
			device_name, device_id, device_running, device_pending), false);
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

    auto layout = ftxui::Container::Vertical
    ({
        // 基本信息
        ftxui::Container::Vertical
        ({
            // 几个 Dropdown
            ftxui::Container::Horizontal
            ({
                ftxui::Dropdown(&program_names, &program_selected),
                ftxui::Container::Horizontal
                ({
                    ftxui::Dropdown(&vasp_version_names, &vasp_version_selected),
                    ftxui::Dropdown(&vasp_variant_names, &vasp_variant_selected)
                }) | ftxui::Maybe([&]{return program_internal_names[program_selected] == "vasp";})
            }),
			// GPU 加速
			ftxui::Container::Vertical
			({
				ftxui::Checkbox("使用 GPU 加速", &gpu_device_use_checked),
				[&]
				{
					auto gpus = ftxui::Container::Vertical({});
					for (auto& [name, checked] : gpu_device_checked)
						gpus->Add(ftxui::Checkbox(name, &checked) | ftxui::Renderer([&](ftxui::Element inner)
							{return ftxui::hbox(ftxui::text("  "), inner);}));
					return gpus;
				}() | ftxui::Maybe(&gpu_device_use_checked)
			}),
            // 其它一些信息
            ftxui::Container::Vertical
            ({
                ftxui::Container::Vertical
                ({
                    ftxui::Input(&mpi_threads_text, "") | ftxui::underlined
						| ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3) | ftxui::Renderer([&](ftxui::Element inner)
                            {return ftxui::hbox(ftxui::text("MPI 线程数: "), inner);})
                        | ftxui::flex_shrink | ftxui::Maybe([&]
						{
							return (program_internal_names[program_selected] == "vasp" && !gpu_device_use_checked)
								|| program_internal_names[program_selected] == "lammps";
						}),
                    ftxui::Input(&openmp_threads_text, "") | ftxui::underlined
                        | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3) | ftxui::Renderer([&](ftxui::Element inner)
                            {return ftxui::hbox(ftxui::text("OpenMP 线程数: "), inner);})
                        | ftxui::flex_shrink | ftxui::Maybe([&]
							{return program_internal_names[program_selected] == "vasp" && !gpu_device_use_checked;})
                }),
                ftxui::Container::Vertical
                ({
                    ftxui::Input(&custom_command_text, "") | ftxui::underlined
                        | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 30) | ftxui::Renderer([&](ftxui::Element inner)
                            {return ftxui::hbox(ftxui::text("自定义命令: "), inner);})
                        | ftxui::flex_shrink,
                    ftxui::Input(&custom_command_cores_text, "") | ftxui::underlined
                        | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3) | ftxui::Renderer([&](ftxui::Element inner)
                            {return ftxui::hbox(ftxui::text("占用 CPU 核心数: "), inner);})
                        | ftxui::flex_shrink
                }) | ftxui::Maybe([&]{return program_internal_names[program_selected] == "Custom";})
            })
        }) | ftxui::Renderer([&](ftxui::Element inner)
            {return ftxui::vbox(ftxui::text("基本信息") | ftxui::bgcolor(ftxui::Color::Blue), inner);}),
        // 高级设置
        ftxui::Container::Vertical
        ({
            ftxui::Container::Horizontal
            ({
                ftxui::Checkbox("自定义路径：", &custom_path_checked),
                ftxui::Input(&custom_path_text, "") | ftxui::underlined
                    | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 30)
                    | ftxui::flex_shrink | ftxui::Maybe([&]{return custom_path_checked;})
            }),
            ftxui::Container::Horizontal
            ({
                ftxui::Checkbox("自定义 OpenMP 线程数：", &custom_openmp_threads_checked),
                ftxui::Input(&custom_openmp_threads_text, "") | ftxui::underlined
                    | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3)
                    | ftxui::flex_shrink | ftxui::Maybe([&]{return custom_openmp_threads_checked;})
            }) | ftxui::Maybe([&]
            {
                return (program_internal_names[program_selected] == "vasp" && gpu_device_use_checked)
					|| program_internal_names[program_selected] == "lammps";
            }),
            ftxui::Checkbox("立即开始运行", &run_now_checked),
            ftxui::Checkbox("在 Ubuntu 22.04 容器中运行", &run_in_container_checked)
                | ftxui::Maybe([&]{return program_internal_names[program_selected] == "custom";})
        }) | ftxui::Renderer([&](ftxui::Element inner)
                {return ftxui::vbox(ftxui::text("高级设置") | ftxui::bgcolor(ftxui::Color::Blue), inner);}),
        // "几个按钮"
        ftxui::Container::Horizontal
		({
			ftxui::Button("提交任务(Enter)", screen.ExitLoopClosure()),
			ftxui::Button("取消", [&]{canceled = true; screen.ExitLoopClosure()();})
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
    return {};
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
            std::vector<std::tuple<unsigned, std::string, unsigned, unsigned>> devices
			{
				{0, "3090", 1, 2},
				{1, "2080 Ti", 2, 1}
			};

			auto result = request_new_job_detail_from_user(devices);
		}
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
