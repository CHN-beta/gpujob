# include <iostream>
# include <vector>
# include <filesystem>
# include <string>
# include <map>
# include <regex>
# include <variant>
# include <fstream>
# include <utility>
# include <deque>
# include <set>
# include <experimental/memory>
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
# include <nameof.hpp>
# include <job.hpp>

using namespace std::literals;

void print_help()
{
	std::cerr << R"(
使用方法:
	job s 或 job submit:
		提交新任务.
	job l 或 job list:
		列出已经提交的任务.
)";
}

std::map<unsigned, std::string> detect_gpu_devices()
// source nvhpc, 然后使用 pgaccelinfo -l 列出所有的 GPU 设备.
// 并不是所有的 GPU 都会列出. 只有 NVIDIA GeForce RTX 开头的会被列出.
{
	std::map<unsigned, std::string> devices;
	boost::process::ipstream command_output;
	boost::process::child command
    {
		R"(bash -c ". /usr/share/Modules/init/bash && module load nvhpc/22.11 && pgaccelinfo | grep 'Device Name'")",
		boost::process::std_out > command_output
	};
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

std::optional<Job_t> request_new_job_detail_from_user()
// 展示提交任务的界面, 并等待用户输入、确认. 保证传回的结果已经被检查过, 不需要再次检查.
// 用户取消时，返回nullopt。否则返回需要提交的任务.
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
	std::vector<std::tuple<std::string, bool, unsigned>> gpu_device_checked;	// 稍后填充
	std::string mpi_threads_text = "4";
	std::string openmp_threads_text = "4";
	std::string lammps_input_text = "lammps.in";
	std::string custom_command_text = "echo hello world";
	std::string custom_command_cores_text = "4";

	// 高级设置
	bool custom_path_checked = false;
	std::string custom_path_text = std::filesystem::current_path();
	bool custom_openmp_threads_checked = false;
	std::string custom_openmp_threads_text = "2";
	bool no_gpu_sf_checked = false;
	bool run_now_checked = false;
	bool run_in_container_checked = false;

	// 初始化 GPU 相关信息
	{
		auto gpu_devices = detect_gpu_devices();
		auto jobd_output = read_out();
		std::map<unsigned, unsigned> gpu_running, gpu_pending;
		for (auto& gpu : gpu_devices)
		{
			gpu_running[gpu.first] = 0;
			gpu_pending[gpu.first] = 0;
		}
		for (auto& job : jobd_output.Jobs)
		{
			if (job.Status == Job_t::Status_t::Running)
			{
				for (auto& gpu : job.UsingGpus)
					if (gpu_running.contains(gpu))
						gpu_running[gpu]++;
			}
			else if (job.Status == Job_t::Status_t::Pending)
				for (auto& gpu : job.UsingGpus)
					if (gpu_pending.contains(gpu))
						gpu_pending[gpu]++;
		}
		for (auto& gpu : gpu_devices)
			gpu_device_checked.emplace_back(fmt::format("{} (ID: {}, {} 运行, {} 等待)",
				gpu.second, gpu.first, gpu_running[gpu.first], gpu_pending[gpu.first]), false, gpu.first);
	}

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
	std::string lammps_input_help_text = "在这里指定 LAMMPS 的输入文件.";
	std::string custom_command_help_text = "在这里输入自定义的命令. 这里输入的内容会被导出为 GPUJOB_CUSTOM_COMMAND 环境变量, "
		"并被 bash 解析执行（bash -c $GPUJOB_CUSTOM_COMMAND）.";
	std::string custom_command_cores_help_text = "在这里输入要占用的 CPU 核心数. "
		"这里输入的内容会被导出为 GPUJOB_CUSTOM_COMMAND_CORES 环境变量, 并据此排队; 但队列系统实际不会限制资源的使用.";
	std::string custom_path_help_text = "自定义程序运行的起始目录. 默认为当前目录. "
		"注意, VASP GPU 版本会在 ubuntu 22.04 容器中运行. 容器中, 宿主机的 /home 被挂载到 /hosthome, 不能访问宿主机的其它目录. ";
	std::string custom_openmp_threads_help_text_lammps = "我会导出 OMP_NUM_THREADS 环境变量, "
		"但不会在命令行中增加 \"-sf omp\", 你需要在输入文件中特定 pair_style 命令中加上 \"/omp\".";
	std::string custom_openmp_threads_help_text_vasp_gpu = "VASP 支持两个层面的并行, 一个叫 MPI, 一个叫 OpenMP. "
		"GPU 版本的 VASP 要求每个 MPI 线程对应一个 GPU, 因此实际占用 CPU 的核心数等于此处设置的 OpenMP 线程数乘以选定的 GPU 个数. "
		"尽管原则上没有限制, 但实际中我发现 OpenMP 线程数取为 2 时性能比 1 略好, 取为 3 或以上时则会慢得多. "
		"因此如果没有特殊需求, 不要修改默认值.";
	std::string no_gpu_sf_help_text = "勾选此选项后, 不会在 LAMMPS 的参数中增加 \"-sf gpu\". "
		"这时你需要手动在恰当的 pair_style 后加上 /gpu.";
	std::string run_now_help_text = "不排队, 立即开始运行. "
		"有时前面有一些比较大的任务, 而这个任务很小, 你就可以勾选此选项, 让它立即开始运行, 不用一直干等着.";
	std::string run_in_container_help_text = "在 ububtu-22.04 容器中运行自定义程序. VASP 的 GPU 版本需要在容器里运行. "
		"宿主机的 /home 在容器中会被挂载为 /hosthome.";
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
	std::optional<Job_t> result;
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
			result->Id = 0;
			result->Status = Job_t::Status_t::Pending;
			result->RunNow = run_now_checked;

			// 提取选定的 gpu 的信息, 这些信息无论任务类型都是用得到的
			std::vector<unsigned> selected_gpus;
			if (gpu_device_use_checked)
			{
				for (auto& [name, checked, index] : gpu_device_checked)
					if (checked)
						selected_gpus.emplace_back(index);
				if (selected_gpus.empty())
					return "请至少选择一个 GPU 设备.";
			}

			// 分任务类别来设置: vasp cpu, vasp gpu, lammps, custom
			// 不在 argument 中设置前后的任务起始和终止时间, cd, 重定向, 这个最后一起加.
			if (program_internal_names[program_selected] == "vasp" && gpu_device_use_checked)
			{
				// 获取 openmp 线程数
				std::optional<unsigned> openmp_threads;
				if (custom_openmp_threads_checked)
				{
					auto openmp_threads = try_to_convert_to_positive_integer(openmp_threads_text);
					if (!openmp_threads)
						return "自定义 OpenMP 线程数必须为正整数.";
				}

				// 获取任务的执行路径
				std::string run_path;
				if (custom_path_checked)
					run_path = custom_path_text;
				else
				{
					run_path = std::filesystem::current_path().string();
					if (std::smatch match; std::regex_match(run_path, match, std::regex("/home(/.*)?")))
						run_path = fmt::format("/hosthome{}", match[1].str());
					else
						return "GPU 版本的 vasp 需要在 ubuntu-22.04 容器中运行, 容器中不能访问宿主机除了 /home 下以外的文件.";
				}

				result->Comment = fmt::format("{} {}", std::getenv("USER"), [&]
				{
					if (std::smatch match; std::regex_match(run_path, match, std::regex("/hosthome/[^/]+/(.*)")))
						return match[1].str();
					else
						return run_path;
				}());
				result->ProgramString = fmt::format
				(
					"cd '{}'; "
					"( "
						"echo start at $(date '+%Y-%m-%d %H:%M:%S') "
						"&& . /etc/profile.d/modules.sh "
						"&& module use /opt/intel/oneapi/modulefiles /opt/nvidia/hpc_sdk/modulefiles "
						"&& module load nvhpc/22.11 mkl/2022.2.1 "
						"&& mpirun -np {} -x OMP_NUM_THREADS={} -x MKL_THREADING_LAYER=INTEL "
							"-x CUDA_DEVICE_ORDER=PCI_BUS_ID -x CUDA_VISIBLE_DEVICES={} vasp_gpu_{}_{} "
						"&& echo end at $(date '+%Y-%m-%d %H:%M:%S') "
					") 2>&1 | tee -a output.txt",
					std::regex_replace(run_path, std::regex("'"), R"('"'"')"),
					selected_gpus.size(), *openmp_threads, fmt::join(selected_gpus, ","), 
					vasp_version_internal_names[vasp_version_selected], vasp_variant_names[vasp_variant_selected]
				);
				result->UsingCores = selected_gpus.size() * *openmp_threads;
				result->UsingGpus = selected_gpus;
				result->RunInContainer = true;
			}
			else if (program_internal_names[program_selected] == "vasp" && !gpu_device_use_checked)
			{
				// 获取 mpi 和 openmp 线程数
				auto mpi_threads = try_to_convert_to_positive_integer(mpi_threads_text);
				if (!mpi_threads)
					return "MPI 线程数必须为正整数.";
				auto openmp_threads = try_to_convert_to_positive_integer(openmp_threads_text);
				if (!openmp_threads)
					return "OpenMP 线程数必须为正整数.";

				// 获取任务的执行路径
				std::string run_path;
				if (custom_path_checked)
					run_path = custom_path_text;
				else
					run_path = std::filesystem::current_path().string();

				result->Comment = fmt::format("{} {}", std::getenv("USER"), [&]
				{
					if (std::smatch match; std::regex_match(run_path, match, std::regex("/home/[^/]+/(.*)")))
						return match[1].str();
					else
						return run_path;
				}());
				result->ProgramString = fmt::format
				(
					"cd '{}'; "
					"( "
						"echo start at $(date '+%Y-%m-%d %H:%M:%S') "
						"&& . /etc/profile.d/modules.sh "
						"&& module use /opt/intel/oneapi/modulefiles "
						"&& module load compiler/2022.2.0 mkl/2022.2.0 mpi/2021.7.0 icc/2022.2.0 "
						"&& mpirun -np {} -genv OMP_NUM_THREADS {} -genv MKL_THREADING_LAYER INTEL vasp_cpu_{}_{} "
						"&& echo end at $(date '+%Y-%m-%d %H:%M:%S') "
					") 2>&1 | tee -a output.txt",
					std::regex_replace(run_path, std::regex("'"), R"('"'"')"),
					*mpi_threads, *openmp_threads,
					vasp_version_internal_names[vasp_version_selected], vasp_variant_names[vasp_variant_selected]
				);
				result->UsingCores = *mpi_threads * *openmp_threads;
				result->RunInContainer = false;
			}
			else if (program_internal_names[program_selected] == "lammps")
			{
				// 获取 mpi 和 openmp 线程数
				auto mpi_threads = try_to_convert_to_positive_integer(mpi_threads_text);
				if (!mpi_threads)
					return "MPI 线程数必须为正整数.";
				std::optional<unsigned> openmp_threads = 1;
				if (custom_openmp_threads_checked)
				{
					openmp_threads = try_to_convert_to_positive_integer(openmp_threads_text);
					if (!openmp_threads)
						return "OpenMP 线程数必须为正整数.";
				}

				// 获取任务的执行路径
				std::string run_path;
				if (custom_path_checked)
					run_path = custom_path_text;
				else
					run_path = std::filesystem::current_path().string();

				result->Comment = fmt::format("{} {}", std::getenv("USER"), [&]
				{
					if (std::smatch match; std::regex_match(run_path, match, std::regex("/home/[^/]+/(.*)")))
						return match[1].str();
					else
						return run_path;
				}());
				result->ProgramString = fmt::format
				(
					"cd '{}'; "
					"( "
						"echo start at $(date '+%Y-%m-%d %H:%M:%S') "
						"&& . /etc/profile.d/lammps.sh "
						"&& mpirun -n {} -x OMP_NUM_THREADS={} {}lmp -in '{}'{}"
						"&& echo end at $(date '+%Y-%m-%d %H:%M:%S') "
					") 2>&1 | tee -a output.txt",
					std::regex_replace(run_path, std::regex("'"), R"('"'"')"),
					*mpi_threads, *openmp_threads,
					gpu_device_use_checked
						? fmt::format("-x CUDA_DEVICE_ORDER=PCI_BUS_ID -x CUDA_VISIBLE_DEVICES={} ",
							fmt::join(selected_gpus, ","))
						: ""s,
					std::regex_replace(lammps_input_text, std::regex("'"), R"('"'"')"),
					gpu_device_use_checked
						? fmt::format("{} -pk gpu {}", no_gpu_sf_checked ? ""s : " -sf gpu"s, selected_gpus.size())
						: ""s
				);
				result->UsingCores = *mpi_threads * *openmp_threads;
				result->UsingGpus = selected_gpus;
				result->RunInContainer = false;
			}
			else if (program_internal_names[program_selected] == "custom")
			{
				// 获取占用的 CPU 核数
				auto cores = try_to_convert_to_positive_integer(custom_command_cores_text);
				if (!cores)
					return "占用的 CPU 核数必须为正整数.";

				// 获取任务的执行路径
				std::string run_path;
				if (custom_path_checked)
					run_path = custom_path_text;
				else
					run_path = std::filesystem::current_path().string();

				result->Comment = fmt::format("{} {}", std::getenv("USER"), [&]
				{
					if
					(
						std::smatch match;
						std::regex_match(run_path, match, std::regex("/(?:hosthome|home)/[^/]+/(.*)"))
					)
						return match[1].str();
					else
						return run_path;
				}());
				result->ProgramString = fmt::format
				(
					"cd '{}'; "
					"( "
						"echo start at $(date '+%Y-%m-%d %H:%M:%S') "
						"&& export GPUJOB_CUSTOM_COMMAND_CORES={} "
						"{}"
						"&& {} "
						"&& echo end at $(date '+%Y-%m-%d %H:%M:%S') "
					") 2>&1 | tee -a output.txt",
					std::regex_replace(run_path, std::regex("'"), R"('"'"')"),
					std::to_string(*cores),
					gpu_device_use_checked ? fmt::format
					(
						"&& export GPUJOB_USE_GPU=1 "
						"&& export CUDA_DEVICE_ORDER=PCI_BUS_ID "
						"&& export CUDA_VISIBLE_DEVICES={} ",
						fmt::join(selected_gpus, ",")
					): ""s,
					custom_command_text
				);
				result->UsingCores = *cores;
				result->UsingGpus = selected_gpus;
				result->RunInContainer = run_in_container_checked;
			}
			else
				std::unreachable();
			return {};
		};
		if (auto message = check_and_set_result())
		{
			result.reset();
			error_dialog_text = *message;
			show_error_dialog = true;
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
						for (auto& [name, checked, index] : gpu_device_checked)
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
					ftxui::Input(&lammps_input_text, "") | ftxui::underlined
						| ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 30)
						| ftxui::Renderer([&](ftxui::Element inner)
							{return ftxui::hbox(ftxui::text("LAMMPS 输入文件: "), inner);})
						| ftxui::Hoverable(set_help_text(std::experimental::make_observer(&lammps_input_help_text)))
						| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());})
						| ftxui::Maybe([&]{return program_internal_names[program_selected] == "lammps";}),
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
				ftxui::Checkbox("不要在命令行中追加 \"-sf gpu\"", &no_gpu_sf_checked)
					| ftxui::Hoverable(set_help_text(std::experimental::make_observer(&no_gpu_sf_help_text)))
					| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());})
					| ftxui::Maybe([&]
						{return program_internal_names[program_selected] == "lammps" && gpu_device_use_checked;}),
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
				ftxui::paragraph(error_dialog_text)
			);}) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 30),
			ftxui::Button("好的", [&]{show_error_dialog = false;})
		}), &show_error_dialog)
		| ftxui::CatchEvent([&](ftxui::Event event)
		{
			if (event == ftxui::Event::Return)
			{
				if (show_error_dialog)
					show_error_dialog = false;
				else if (try_submit())
					screen.ExitLoopClosure()();
			}
			return event == ftxui::Event::Return;
		});
	std::cout << "\x1b[?1000;1006;1015h" << std::endl;
    screen.Loop(layout);
	std::cout << "\x1b[?1000;1006;1015l" << std::endl;
	return result;
}

std::vector<unsigned> request_cancel_job_from_user()
// 请求删除任务, 返回要删除的任务的 id.
{
	bool please_refresh = true;
	std::set<unsigned> checked_jobs;
	while (please_refresh)
	{
		please_refresh = false;

		auto [jobs] = read_out();
		std::deque<bool> selected;
		selected.resize(jobs.size(), false);
		auto detail = ftxui::emptyElement();
		auto gpu_names = detect_gpu_devices();
		auto screen = ftxui::ScreenInteractive::Fullscreen();

		// 对任务排序, 正在运行的最优先, 然后是等待的, 最后是已经完成的
		std::sort(jobs.begin(), jobs.end(), [](auto& a, auto& b)
		{
			if (a.Status == b.Status)
				return a.Id < b.Id;
			std::map<Job_t::Status_t, unsigned> order =
			{
				{Job_t::Status_t::Running, 0},
				{Job_t::Status_t::Pending, 1},
				{Job_t::Status_t::Finished, 2}
			};
			return order[a.Status] < order[b.Status];
		});

		auto layout = ftxui::Container::Vertical
		({
			[&]
			{
				std::vector<ftxui::Component> columns;
				for (std::size_t i = 0; i < jobs.size(); ++i)
					columns.push_back(ftxui::Container::Horizontal
					({
						(
							(jobs[i].User == getenv("USER") && jobs[i].Status != Job_t::Status_t::Finished)
								? ftxui::Checkbox("", &selected[i])
								: ftxui::Renderer([]{return ftxui::emptyElement();})
						) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 2),
						ftxui::Button
						(
							fmt::format("{} {} {}", jobs[i].Id, nameof::nameof_enum(jobs[i].Status), jobs[i].Comment),
							[&, i]
							{
								detail = ftxui::vbox
								(
									ftxui::hbox(ftxui::text("ID: "), ftxui::paragraph(std::to_string(jobs[i].Id))),
									ftxui::hbox(ftxui::text("User: "), ftxui::paragraph(jobs[i].User)),
									ftxui::hbox(ftxui::text("ProgramString: "),
										ftxui::paragraph(jobs[i].ProgramString)),
									ftxui::hbox(ftxui::text("Comment: "), ftxui::paragraph(jobs[i].Comment)),
									ftxui::hbox(ftxui::text("UsingCores: "),
										ftxui::paragraph(std::to_string(jobs[i].UsingCores))),
									ftxui::hbox(ftxui::text("UsingGpus: "), [&]
									{
										std::vector<ftxui::Element> gpus;
										for (auto& gpu : jobs[i].UsingGpus)
											gpus.push_back(ftxui::paragraph(fmt::format("{}: {}",
												gpu, gpu_names.contains(gpu) ? gpu_names[gpu] : "Unknown")));
										return ftxui::vbox(gpus);
									}()),
									ftxui::hbox(ftxui::text("Status: "),
										ftxui::paragraph(std::string{nameof::nameof_enum(jobs[i].Status)})),
									ftxui::hbox(ftxui::text("RunInContainer: "),
										ftxui::paragraph(fmt::format("{}", jobs[i].RunInContainer))),
									ftxui::hbox(ftxui::text("RunNow: "),
										ftxui::paragraph(fmt::format("{}", jobs[i].RunNow)))
								);
							},
							ftxui::ButtonOption::Ascii()
						)
					}));
				return ftxui::Container::Vertical(columns);
			}() | ftxui::vscroll_indicator | ftxui::frame
				| ftxui::Renderer([&](ftxui::Element inner){return ftxui::window(ftxui::text("任务列表"), inner);}),
			ftxui::Container::Horizontal
			({
				ftxui::Button("刷新", [&]
				{
					please_refresh = true;
					checked_jobs.clear();
					for (std::size_t i = 0; i < selected.size(); i++)
						if (selected[i])
							checked_jobs.insert(jobs[i].Id);
					screen.ExitLoopClosure()();
				}),
				ftxui::Button("结束选中的任务并退出", [&]
				{
					checked_jobs.clear();
					for (std::size_t i = 0; i < selected.size(); i++)
						if (selected[i])
							checked_jobs.insert(jobs[i].Id);
					screen.ExitLoopClosure()();
				}),
				ftxui::Button("直接退出", [&]
				{
					checked_jobs.clear();
					screen.ExitLoopClosure()();
				})
			}) | ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());}),
			ftxui::Renderer([&]{return ftxui::window(ftxui::text("详细信息"), detail);})
		});

		std::cout << "\x1b[?1000;1006;1015h" << std::endl;
		screen.Loop(layout);
		std::cout << "\x1b[?1000;1006;1015h" << std::endl;
	}
	return {checked_jobs.begin(), checked_jobs.end()};
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
			auto result = request_new_job_detail_from_user();
			if (result)
				write_in({{*result}, {}});
		}
		else if (action == "l" || action == "list")
		{
			auto jobs = request_cancel_job_from_user();
			if (!jobs.empty())
			{
				std::vector<std::pair<unsigned, std::string>> jobs_to_cancel;
				for (auto& job : jobs)
					jobs_to_cancel.emplace_back(job, "");
				write_in({{}, jobs_to_cancel});
			}
		}
		else
		{
			print_help();
			return 1;
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
