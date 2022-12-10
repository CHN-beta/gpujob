# include <iostream>
# include <vector>
# include <filesystem>
# include <string>
# include <map>
# include <regex>
# include <variant>
# include <fstream>
# include <utility>
# include <experimental/memory>
# include <ftxui/dom/elements.hpp>
# include <ftxui/component/component.hpp>
# include <ftxui/component/screen_interactive.hpp>
# include <ftxui/dom/elements.hpp>
# include <ftxui/screen/screen.hpp>
# include <ftxui/screen/string.hpp>
# include <boost/interprocess/sync/file_lock.hpp>
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
// source nvhpc, 然后使用 pgaccelinfo -l 列出所有的 GPU 设备.
// 并不是所有的 GPU 都会列出. 只有 NVIDIA GeForce RTX 开头的会被列出.
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

std::optional<std::map<std::string, std::variant<std::string, unsigned, bool, std::vector<unsigned>>>>
	request_new_job_detail_from_user(std::vector<std::tuple<unsigned, std::string, unsigned, unsigned>> gpu_devices)
// 展示提交任务的界面, 并等待用户输入、确认. 所有参数使用字符串传回. 保证传回的结果已经被检查过, 不需要再次检查.
// 传入的参数依次是：每个 GPU 设备的 id、名称、正在运行的任务数、正在等待的任务数.
// 用户取消时，返回nullopt。否则返回的字典中包括以下参数：
// 	"Program": 字符串值, "vasp" 或 "lammps" 或 "custom", 表明用户选择的程序.
// 	"VaspVersion": 字符串值, "620-vtst" 或 "631", 表明用户选择的 VASP 版本. 只当 "Program" 为 "vasp" 时存在.
// 	"VaspVariant": 字符串值, "std" 或 "gam" 或 "ncl", 表明用户选择的 VASP 变体. 只当 "Program" 为 "vasp" 时存在.
// 	"GpuDevices": 数组, 用户选择的 GPU 设备的 id, 与传入的参数对应.
// 	"MpiThreads": 正整数, 用户选择的 MPI 线程数. 只当 "Program" 为 "vasp" 或 "lammps" 时存在.
// 	"OpenmpThreads": 正整数, 用户选择的 OpenMP 线程数. 只当 "Program" 为 "vasp" 或 "lammps" 时存在.
// 	"CustomCommand": 字符串值, 用户输入的自定义命令. 只当 "Program" 为 "custom" 时存在.
// 	"CustomCommandCores": 正整数, 用户选择的自定义命令的 CPU 核心数. 只当 "Program" 为 "custom" 时存在.
// 	"Path": 字符串值, 表示运行的路径.
// 	"RunNow": 布尔值, 表示不排队、立即开始运行.
//  "RunInContainer": 布尔值, 在 ubuntu-22.04 容器中运行.

// Todo: Path 说明在容器中运行时，会被修改到特定路径下
// todo: 自定义任务说明
{
    auto screen = ftxui::ScreenInteractive::Fullscreen();

	// 基本信息
	std::vector<std::string> program_names {"VASP", "LAMMPS", "自定义程序"};
	std::vector<std::string> program_internal_names {"vasp", "lammps", "custom"};
	int program_selected = 0;
	std::vector<std::string> vasp_version_names {"6.3.1"};
	std::vector<std::string> vasp_version_internal_names {"631"};
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

	// 帮助文本
	std::string original_help_text = "拿起鼠标, 哪里需要点哪里. 这里会给出提示信息.";
	std::string help_text = original_help_text;
	std::string program_help_text = "选择你要运行的程序.";
	std::string vasp_version_help_text = "选择你要运行的 VASP 版本. "
		"如果是新的项目, 建议用最新的版本; 如果是继续之前的项目, 之前用什么版本现在还用什么就可以了.";
	std::string vasp_variant_help_text = "选择你要运行的 VASP 版本. 通常使用 std 即可. "
		"当 k 点只有 gamma 点时, 使用 gam 计算更快. ncl 我没有用过, 似乎与自旋-轨道耦合有关.";
	std::string gpu_device_use_help_text_vasp = "是否使用 GPU 版本的 VASP. "
		"对于 VASP, 不推荐使用多个 GPU, 你会发现速度反而变慢; 但选了多个 GPU 也不会出错.";
	std::string gpu_device_use_help_text_lammps = "是否使用 GPU 加速 LAMMPS 的运行. "
		"一般勾选此项和下面的一个或多个 GPU, 即可要求 LAMMPS 使用 GPU. "
		"队列系统会在命令行中追加 \"-sf gpu -pk gpu n\" 来要求 LAMMPS 使用 GPU. "
		"如果你希望手动指定哪些 pair_style 使用 GPU 而哪些不使用, 可以在高级设置中勾选 \"不在命令行中增加 '-sf gpu'\", "
		"然后手动在输入文件中的一部分 pair_style 命令中加上 \"/gpu\".";
	std::string gpu_device_use_help_text_custom = "是否使用 GPU 加速自定义程序的运行. "
		"勾选后, 队列系统会导出环境变量 GPUJOB_USE_GPU=1 和 CUDA_DEVICE_ORDER=PCI_BUS_ID, "
		"以及 CUDA_VISIBLE_DEVICES（其值为逗号分隔的 id, 详细见嘤伟达的文档）. 即使不勾选, 队列系统也不会真的限制程序对 GPU 的访问.";
	std::string mpi_openmp_threads_help_text_vasp_cpu = "VASP 支持两个层面的并行, 一个叫 MPI, 一个叫 OpenMP, "
		"实际占用 CPU 的核心数等于 MPI 线程数乘以 OpenMP 线程数. 将两者取为相近的数值, 性能更好. "
		"例如, 将 MPI 线程数和 OpenMP 线程数都设置为 4, 性能比 MPI 线程数取为 16、OpenMP 线程数取为 1 的性能要好, "
		"尽管两种设置都占用了 16 个核心.";
	std::string mpi_threads_help_text_lammps = "LAMMPS 通常只使用 MPI 层面的并行, 即此处设置的值就是实际占用 CPU 的核心数. "
		"如果你确实需要使用 OpenMP 层面的并行, 可以在高级设置中勾选 \"使用 OpenMP 并行\".";
	std::string custom_command_help_text = "在这里输入自定义的命令. 这里输入的内容会被导出为 GPUJOB_CUSTOM_COMMAND 环境变量, "
		"并被 bash 解析执行（bash -c $GPUJOB_CUSTOM_COMMAND）.";
	std::string custom_command_cores_help_text = "在这里输入要占用的 CPU 核心数. "
		"这里输入的内容会被导出为 GPUJOB_CUSTOM_COMMAND_CORES 环境变量, 但队列系统实际不会限制资源的使用.";
	std::string custom_path_help_text = "自定义程序运行的起始目录. 默认为当前目录.";
	std::string custom_openmp_threads_help_text_lammps = "我会导出 OMP_NUM_THREADS 环境变量, "
		"但不会在命令行中增加 \"-sf omp\", 你需要在输入文件中特定 pair_style 命令中加上 \"/omp\".";
	std::string custom_openmp_threads_help_text_vasp_gpu = "VASP 支持两个层面的并行, 一个叫 MPI, 一个叫 OpenMP. "
		"GPU 版本的 VASP 要求每个 MPI 线程对应一个 GPU, 因此实际占用 CPU 的核心数等于此处设置的 OpenMP 线程数乘以选定的 GPU 个数. "
		"尽管原则上没有限制, 但实际中我发现 OpenMP 线程数取为 2 时性能比 1 略好, 取为 3 或以上时则会慢得多. "
		"因此如果没有特殊需求, 不要修改默认值.";
	std::string run_now_help_text = "不排队, 立即开始运行. "
		"有时前面有一些比较大的任务, 而这个任务很小, 你就可以勾选此选项, 让它立即开始运行, 不用一直干等着.";
	std::string run_in_container_help_text = "在 ububtu-22.04 容器中运行自定义程序. VASP 的 GPU 版本需要在容器里运行. "
		"/home 在容器中会被挂载为 /hosthome.";
	auto set_help_text = [&](std::experimental::observer_ptr<const std::string> content)
	{
		static std::map<std::experimental::observer_ptr<const std::string>, unsigned> enabled_help;
		return [&, content](bool set_or_unset)
		{
			if (set_or_unset)
			{
				if (enabled_help.contains(content))
					enabled_help[content]++;
				else
				{
					enabled_help[content] = 1;
					help_text = *content;
				}
			}
			else if (enabled_help.contains(content))
			{
				enabled_help[content]--;
				if (enabled_help[content] == 0)
				{
					enabled_help.erase(content);
					if (enabled_help.empty())
						help_text = original_help_text;
					else
						help_text = *enabled_help.begin()->first;
				}
			}
			else
				std::unreachable();
		};
	};

	// 提交任务按钮相关
	bool modal_shown = false;
	std::string modal_text;
	std::optional<std::map<std::string, std::variant<std::string, unsigned, bool, std::vector<unsigned>>>> result;
	bool show_error_dialog = false;
	std::string error_dialog_text;
	auto try_submit = [&] -> bool
	{
		auto try_to_convert_to_positive_integer = [](const std::string& str) -> std::optional<unsigned>
		{
			if (!std::regex_match(str, std::regex("[0-9]+")))
				return std::nullopt;
			unsigned result;
			try
			{
				result = std::stoi(str);
			}
			catch (const std::invalid_argument&)
			{
				return std::nullopt;
			}
			if (result > 0)
				return result;
			else
				return std::nullopt;
		};
		auto check_and_set_result = [&] -> std::optional<std::string>
		{
			result.emplace();
			(*result)["Program"] = program_internal_names[program_selected];
			if (program_internal_names[program_selected] == "vasp")
			{
				(*result)["VaspVersion"] = vasp_version_internal_names[vasp_version_selected];
				(*result)["VaspVariant"] = vasp_variant_names[vasp_variant_selected];
			}
			if (gpu_device_use_checked)
			{
				std::vector<unsigned> gpu_devices;
				for (unsigned i = 0; i < gpu_device_checked.size(); i++)
					if (gpu_device_checked[i].second)
						gpu_devices.emplace_back(i);
				if (gpu_devices.empty())
					return "请至少选择一个 GPU 设备.";
				(*result)["GpuDevices"] = gpu_devices;
			}
			if (program_internal_names[program_selected] == "vasp")
			{
				if (gpu_device_use_checked)
				{
					(*result)["MpiThreads"]
						= static_cast<unsigned>(std::get<std::vector<unsigned>>((*result)["GpuDevices"]).size());
					if (custom_openmp_threads_checked)
					{
						if
						(
							auto omp_threads = try_to_convert_to_positive_integer(custom_openmp_threads_text);
							omp_threads
						)
							(*result)["OpenmpThreads"] = *omp_threads;
						else
							return "OpenMP 线程数必须为正整数.";
					}
					else
						(*result)["OpenmpThreads"] = 2u;
				}
				else
				{
					if (auto mpi_threads = try_to_convert_to_positive_integer(mpi_threads_text); mpi_threads)
						(*result)["MpiThreads"] = *mpi_threads;
					else
						return "MPI 线程数必须为正整数.";
					if (auto omp_threads = try_to_convert_to_positive_integer(openmp_threads_text); omp_threads)
						(*result)["OpenmpThreads"] = *omp_threads;
					else
						return "OpenMP 线程数必须为正整数.";
				}
			}
			else if (program_internal_names[program_selected] == "lammps")
			{
				if (auto mpi_threads = try_to_convert_to_positive_integer(mpi_threads_text); mpi_threads)
					(*result)["MpiThreads"] = *mpi_threads;
				else
					return "MPI 线程数必须为正整数.";
				if (custom_openmp_threads_checked)
				{
					if
					(
						auto omp_threads = try_to_convert_to_positive_integer(custom_openmp_threads_text);
						omp_threads
					)
						(*result)["OpenmpThreads"] = *omp_threads;
					else
						return "OpenMP 线程数必须为正整数.";
				}
				else
					(*result)["OpenmpThreads"] = 1u;
			}
			else if (program_internal_names[program_selected] == "custom")
			{
				if (custom_command_text == "")
					return "自定义命令不能为空.";
				(*result)["CustomCommand"] = custom_command_text;
				if
				(
					auto custom_command_cores = try_to_convert_to_positive_integer(custom_command_cores_text);
					custom_command_cores
				)
					(*result)["CustomCommandCores"] = *custom_command_cores;
				else
					return "占用的 CPU 核心数必须为正整数.";
			}
			else
				std::unreachable();
			if (custom_path_checked)
			{
				if (custom_path_text == "")
					return "自定义路径不能为空.";
				(*result)["Path"] = custom_path_text;
			}
			else if (program_internal_names[program_selected] == "vasp" && gpu_device_use_checked)
			{
				if
				(
					auto [path, match] = std::tuple{std::filesystem::current_path().string(), std::smatch{}};
					std::regex_match(path, match, std::regex("^/home(/.*)?$"))
				)
					(*result)["Path"] = "/hosthome" + match[1].str();
				else
					return "VASP GPU 版本必须在 ubuntu-22.04 容器中运行, 容器中无法访问宿主机中除了 /home 以外的目录.";
			}
			else
				(*result)["Path"] = std::filesystem::current_path().string();
			(*result)["RunNow"] = run_now_checked;
			if (program_internal_names[program_selected] == "vasp" && gpu_device_use_checked)
				(*result)["RunInContainer"] = true;
			else if (program_internal_names[program_selected] == "custom")
				(*result)["RunInContainer"] = run_in_container_checked;
			else
				(*result)["RunInContainer"] = false;
			return {};
		};
		if (auto message = check_and_set_result())
		{
			result.reset();
			modal_text = *message;
			modal_shown = true;
			return false;
		}
		else
			return true;
	};
	auto submit_button = ftxui::Button("提交任务 (Enter)", [&]{if (try_submit()) screen.ExitLoopClosure()();});

    auto layout = ftxui::Container::Vertical
	({
		// 提交新任务
		ftxui::Container::Vertical
		({
			// 基本信息
			ftxui::Container::Vertical
			({
				// 几个 Dropdown
				ftxui::Container::Horizontal
				({
					ftxui::Dropdown(&program_names, &program_selected)
						| ftxui::Hoverable(set_help_text(std::experimental::make_observer(&program_help_text))),
					ftxui::Container::Horizontal
					({
						ftxui::Dropdown(&vasp_version_names, &vasp_version_selected)
							| ftxui::Hoverable(set_help_text
								(std::experimental::make_observer(&vasp_version_help_text))),
						ftxui::Dropdown(&vasp_variant_names, &vasp_variant_selected)
							| ftxui::Hoverable(set_help_text(std::experimental::make_observer(&vasp_variant_help_text)))
					}) | ftxui::Maybe([&]{return program_internal_names[program_selected] == "vasp";})
				}),
				// GPU 加速
				ftxui::Container::Vertical
				({
					ftxui::Checkbox("使用 GPU 加速", &gpu_device_use_checked) | ftxui::Hoverable([&](bool set_or_unset)
					{
						if (program_internal_names[program_selected] == "vasp")
							set_help_text(std::experimental::make_observer(&gpu_device_use_help_text_vasp))
								(set_or_unset);
						else if (program_internal_names[program_selected] == "lammps")
							set_help_text(std::experimental::make_observer(&gpu_device_use_help_text_lammps))
								(set_or_unset);
						else if (program_internal_names[program_selected] == "custom")
							set_help_text(std::experimental::make_observer(&gpu_device_use_help_text_custom))
								(set_or_unset);
						else
							std::unreachable();
					}) | ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());}),
					[&]
					{
						auto gpus = ftxui::Container::Vertical({});
						for (auto& [name, checked] : gpu_device_checked)
							gpus->Add(ftxui::Checkbox(name, &checked)
								| ftxui::Renderer([&](ftxui::Element inner)
									{return ftxui::hbox(ftxui::text("  "), inner);})
								| ftxui::Hoverable([&](bool set_or_unset)
								{
									if (program_internal_names[program_selected] == "vasp")
										set_help_text(std::experimental::make_observer(&gpu_device_use_help_text_vasp))
											(set_or_unset);
									else if (program_internal_names[program_selected] == "lammps")
										set_help_text(std::experimental::make_observer
											(&gpu_device_use_help_text_lammps))(set_or_unset);
									else if (program_internal_names[program_selected] == "custom")
										set_help_text(std::experimental::make_observer
											(&gpu_device_use_help_text_custom))(set_or_unset);
									else
										std::unreachable();
								}));
						return gpus;
					}() | ftxui::Maybe(&gpu_device_use_checked)
				}),
				// 其它一些信息
				ftxui::Container::Vertical
				({
					ftxui::Container::Vertical
					({
						ftxui::Input(&mpi_threads_text, "") | ftxui::underlined
							| ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3)
							| ftxui::Renderer([&](ftxui::Element inner)
								{return ftxui::hbox(ftxui::text("MPI 线程数: "), inner);})
							| ftxui::Hoverable([&](bool set_or_unset)
							{
								if (program_internal_names[program_selected] == "vasp" && !gpu_device_use_checked)
									set_help_text(std::experimental::make_observer
										(&mpi_openmp_threads_help_text_vasp_cpu))(set_or_unset);
								else if (program_internal_names[program_selected] == "lammps")
									set_help_text(std::experimental::make_observer(&mpi_threads_help_text_lammps))
										(set_or_unset);
								else
									std::unreachable();
							})
							| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());})
							| ftxui::Maybe([&]
							{
								return (program_internal_names[program_selected] == "vasp" && !gpu_device_use_checked)
									|| program_internal_names[program_selected] == "lammps";
							}),
						ftxui::Input(&openmp_threads_text, "") | ftxui::underlined
							| ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3)
							| ftxui::Renderer([&](ftxui::Element inner)
								{return ftxui::hbox(ftxui::text("OpenMP 线程数: "), inner);})
							| ftxui::Hoverable(set_help_text(std::experimental::make_observer
								(&mpi_openmp_threads_help_text_vasp_cpu)))
							| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());})
							| ftxui::Maybe([&]
								{return program_internal_names[program_selected] == "vasp" && !gpu_device_use_checked;})
					}),
					ftxui::Container::Vertical
					({
						ftxui::Input(&custom_command_text, "") | ftxui::underlined
							| ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 30)
							| ftxui::Renderer([&](ftxui::Element inner)
								{return ftxui::hbox(ftxui::text("自定义命令: "), inner);})
							| ftxui::flex_shrink
							| ftxui::Hoverable(set_help_text(std::experimental::make_observer
								(&custom_command_help_text))),
						ftxui::Input(&custom_command_cores_text, "") | ftxui::underlined
							| ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3)
							| ftxui::Renderer([&](ftxui::Element inner)
								{return ftxui::hbox(ftxui::text("占用 CPU 核心数: "), inner);})
							| ftxui::flex_shrink
							| ftxui::Hoverable(set_help_text(std::experimental::make_observer
								(&custom_command_cores_help_text)))
					}) | ftxui::Maybe([&]{return program_internal_names[program_selected] == "custom";})
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
						| ftxui::flex_shrink
						| ftxui::Maybe([&]{return custom_path_checked;})
				}) | ftxui::Hoverable(set_help_text(std::experimental::make_observer(&custom_path_help_text)))
					| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());}),
				ftxui::Container::Horizontal
				({
					ftxui::Checkbox("自定义 OpenMP 线程数：", &custom_openmp_threads_checked),
					ftxui::Input(&custom_openmp_threads_text, "") | ftxui::underlined
						| ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3)
						| ftxui::flex_shrink | ftxui::Maybe([&]{return custom_openmp_threads_checked;})
				}) | ftxui::Hoverable([&](bool set_or_unset)
				{
					if (program_internal_names[program_selected] == "vasp" && gpu_device_use_checked)
						set_help_text(std::experimental::make_observer(&custom_openmp_threads_help_text_vasp_gpu))
							(set_or_unset);
					else if (program_internal_names[program_selected] == "lammps")
						set_help_text(std::experimental::make_observer(&custom_openmp_threads_help_text_lammps))
							(set_or_unset);
					else
						std::unreachable();
				}) | ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());})
					| ftxui::Maybe([&]
					{
						return (program_internal_names[program_selected] == "vasp" && gpu_device_use_checked)
							|| program_internal_names[program_selected] == "lammps";
					}),
				ftxui::Checkbox("立即开始运行", &run_now_checked)
					| ftxui::Hoverable(set_help_text(std::experimental::make_observer(&run_now_help_text)))
					| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());}),
				ftxui::Checkbox("在 Ubuntu 22.04 容器中运行", &run_in_container_checked)
					| ftxui::Hoverable(set_help_text(std::experimental::make_observer(&run_in_container_help_text)))
					| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());})
					| ftxui::Maybe([&]{return program_internal_names[program_selected] == "custom";})
			}) | ftxui::Renderer([&](ftxui::Element inner)
					{return ftxui::vbox(ftxui::text("高级设置") | ftxui::bgcolor(ftxui::Color::Blue), inner);}),
			// "几个按钮"
			ftxui::Container::Horizontal
			({
				submit_button,
				ftxui::Button("取消", screen.ExitLoopClosure())
			})
				| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());})
		}) | ftxui::Renderer([&](ftxui::Element inner){return ftxui::window(ftxui::text("提交新任务"), inner);}),
		ftxui::Renderer([&]{return ftxui::window(ftxui::text("帮助信息"), ftxui::paragraph(help_text));})
	}) | ftxui::Renderer([&](ftxui::Element inner){return ftxui::vbox(inner, ftxui::filler(), ftxui::hbox
		(
			ftxui::filler(), ftxui::text("Code by CHN with "),
			ftxui::text("❤️") | ftxui::color(ftxui::Color::Red),
			ftxui::text(" love.")
		) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1));})
		| ftxui::Modal(ftxui::Container::Vertical
		({
			ftxui::Renderer([&]{return ftxui::vbox
			(
				ftxui::text("布盒里的参数") | ftxui::bgcolor(ftxui::Color::RedLight),
				ftxui::paragraph(modal_text)
			);}) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 30),
			ftxui::Button("好的", [&]{modal_shown = false;})
		}), &modal_shown)
		| ftxui::CatchEvent([&](ftxui::Event event)
		{
			if (event == ftxui::Event::Return)
			{
				if (modal_shown)
					modal_shown = false;
				else
					screen.ExitLoopClosure()();
			}
			return event == ftxui::Event::Return;
		});
    screen.Loop(layout);
	return result;
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
