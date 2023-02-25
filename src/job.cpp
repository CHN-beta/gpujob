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
		boost::process::search_path("bash"), "-c",
		". /usr/share/Modules/init/bash && module use /opt/nvidia/hpc_sdk/modulefiles "
			"&& module load nvhpc/22.11 && pgaccelinfo | grep 'Device Name'",
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
	std::vector<std::string> program_names {"VASP", "LAMMPS", "Custom Command"};
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
			gpu_device_checked.emplace_back(fmt::format("{} (ID: {}, {} running, {} pending)",
				gpu.second, gpu.first, gpu_running[gpu.first], gpu_pending[gpu.first]), false, gpu.first);
	}

	// 帮助文本
	std::string original_help_text = "Move the mouse cursor to the desired position, and a help message will be displayed. If you're using an outdated terminal like Putty (that doesn't report real-time mouse position), the help message won't appear until you click on it. The help text is in English instead of Chinese, as the width of Chinese characters are not rendered well in some cases like in Putty.";
	std::string help_text = original_help_text;
	std::string program_help_text = "Choose the program you want to run.";
	std::string vasp_version_help_text = "Choose the version of VASP you want to run. For new projects, it is recommended to use the latest version; for continuing old projects, use the same version as before.";
	std::string vasp_variant_help_text = "Choose the variant of VASP you want to run. Usually \"std\" is enough. When k points are only gamma point, \"gam\" is faster. I haven't used \"ncl\", which seems to be related to spin-orbit coupling, only use it if you know what it is for.";
	std::string gpu_device_use_help_text_vasp = "Whether to use GPU version of VASP. For VASP, it is not recommended to use multiple GPUs, you will find that the speed is actually slower; but selecting multiple GPUs will not cause errors.";
	std::string gpu_device_use_help_text_lammps = "Whether to use GPU acceleration for LAMMPS. Generally, check this and one or more of the following GPUs, and LAMMPS will use GPU. The queue system will add \"-sf gpu -pk gpu n\" to the command line to request LAMMPS to use GPU. If you want to manually specify which pair_style uses GPU and which does not, you can check \"Do not add '-sf gpu' to the command line\", and then manually add \"/gpu\" to some pair_style commands in the input file.";
	std::string gpu_device_use_help_text_custom = "Whether to use GPU acceleration for custom programs. If checked, the queue system will export environment variables GPUJOB_USE_GPU=1 and CUDA_DEVICE_ORDER=PCI_BUS_ID, and CUDA_VISIBLE_DEVICES (its value is a comma-separated list of ids, see NVIDIA's documentation for details). Even if not checked, the queue system will not really limit the program's access to GPU.";
	std::string mpi_openmp_threads_help_text_vasp_cpu = "VASP supports two levels of parallelism, one called MPI, the other called OpenMP, the actual number of CPU cores occupied is the product of the MPI thread number and the OpenMP thread number. According to my test, taking the two as close as possible, the performance is better. For example, if the MPI thread number and the OpenMP thread number are both set to 4, the performance is better than the MPI thread number is 16, the OpenMP thread number is 1, although both settings occupy 16 cores.";
	std::string mpi_threads_help_text_lammps = "LAMMPS usually only uses MPI-level parallelism, i.e. the value set here is the actual number of CPU cores occupied. If you really need to use OpenMP-level parallelism, you can check \"Use OpenMP parallel\" in the advanced settings.";
	std::string lammps_input_help_text = "Specify the input file for LAMMPS here.";
	std::string custom_command_help_text = "Input the custom command here. The content entered here will be exported as the GPUJOB_CUSTOM_COMMAND environment variable, and will be parsed and executed by bash (\"bash -c $GPUJOB_CUSTOM_COMMAND\").";
	std::string custom_command_cores_help_text = "Input the number of CPU cores to be occupied here. The content entered here will be exported as the GPUJOB_CUSTOM_COMMAND_CORES environment variable, and will be parsed and queued according to it; but the queue system will not actually limit the use of resources.";
	std::string custom_path_help_text = "Specify the starting directory of the custom program. The default is the current directory. Note that the GPU version of VASP will run in a ubuntu 22.04 container. In the container, the host's /home is mounted to /hosthome, and cannot access other directories on the host.";
	std::string custom_openmp_threads_help_text_lammps = "I will export OMP_NUM_THREADS environment variable, but will not add \"-sf omp\" to the command line, you need to add \"/omp\" to the pair_style command in the input file.";
	std::string custom_openmp_threads_help_text_vasp_gpu = "VASP supports two levels of parallelism, one called MPI, the other called OpenMP. The GPU version of VASP requires one MPI thread to correspond to one GPU, so the actual number of CPU cores occupied is the product of the OpenMP thread number and the number of GPUs selected. Although there is no limit in principle, in practice I found that the performance is slightly better when the OpenMP thread number is 2 than it is 1, and when the OpenMP thread number is 3 or more, it will be much slower. Therefore, if there is no special need, do not modify the default value.";
	std::string no_gpu_sf_help_text = "Check this option to not add \"-sf gpu\" to the command line. You need to manually add \"/gpu\" to some pair_style commands in the input file.";
	std::string run_now_help_text = "Run the task immediately without queuing. Sometimes there are some big tasks in front of you, and this task is very small, you can check this option, let it run immediately, without waiting.";
	std::string run_in_container_help_text = "Run the task in a container. The GPU version of VASP will run in a ubuntu 22.04 container. In the container, the host's /home is mounted to /hosthome, and cannot access other directories on the host.";
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
					return "Select at least one GPU.";
			}

			// 分任务类别来设置: vasp cpu, vasp gpu, lammps, custom
			if (program_internal_names[program_selected] == "vasp" && gpu_device_use_checked)
			{
				// 获取 openmp 线程数
				std::optional<unsigned> openmp_threads;
				if (custom_openmp_threads_checked)
				{
					auto openmp_threads = try_to_convert_to_positive_integer(openmp_threads_text);
					if (!openmp_threads)
						return "Custom OpenMP threads must be a positive integer.";
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
						return "GPU version of vasp can only be run in a container, and the container cannot access files on the host except for files in /home.";
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
					return "MPI threads number must be a positive integer.";
				auto openmp_threads = try_to_convert_to_positive_integer(openmp_threads_text);
				if (!openmp_threads)
					return "OpenMP threads number must be a positive integer.";

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
					return "MPI threads number must be a positive integer.";
				std::optional<unsigned> openmp_threads = 1;
				if (custom_openmp_threads_checked)
				{
					openmp_threads = try_to_convert_to_positive_integer(openmp_threads_text);
					if (!openmp_threads)
						return "OpenMP threads number must be a positive integer.";
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
					return "Occupied CPU cores number must be a positive integer.";

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
	auto submit_button = ftxui::Button("Submit (Enter)", [&]{if (try_submit()) screen.ExitLoopClosure()();});

	// 为了putty可以正常显示，需要将 ▣/☐ 换为 [ ]/[*]
	auto checkbox_option = ftxui::CheckboxOption::Simple();
	checkbox_option.transform = [](const ftxui::EntryState& s)
	{
    	auto prefix = ftxui::text(s.state ? "[X] " : "[ ] ");
    	auto t = ftxui::text(s.label);
    	if (s.active) t |= ftxui::bold;
    	if (s.focused) t |= ftxui::inverted;
    	return ftxui::hbox({prefix, t});
  	};

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
					ftxui::Checkbox("Use GPU acceleration", &gpu_device_use_checked, checkbox_option)
					| ftxui::Hoverable([&](bool set_or_unset)
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
							gpus->Add(ftxui::Checkbox(name, &checked, checkbox_option)
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
								{return ftxui::hbox(ftxui::text("MPI thread number: "), inner);})
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
								{return ftxui::hbox(ftxui::text("OpenMP thread number: "), inner);})
							| ftxui::Hoverable(set_help_text(std::experimental::make_observer
								(&mpi_openmp_threads_help_text_vasp_cpu)))
							| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());})
							| ftxui::Maybe([&]
								{return program_internal_names[program_selected] == "vasp" && !gpu_device_use_checked;})
					}),
					ftxui::Input(&lammps_input_text, "") | ftxui::underlined
						| ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 30)
						| ftxui::Renderer([&](ftxui::Element inner)
							{return ftxui::hbox(ftxui::text("LAMMPS input file: "), inner);})
						| ftxui::Hoverable(set_help_text(std::experimental::make_observer(&lammps_input_help_text)))
						| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());})
						| ftxui::Maybe([&]{return program_internal_names[program_selected] == "lammps";}),
					ftxui::Container::Vertical
					({
						ftxui::Input(&custom_command_text, "") | ftxui::underlined
							| ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 30)
							| ftxui::Renderer([&](ftxui::Element inner)
								{return ftxui::hbox(ftxui::text("Custom command: "), inner);})
							| ftxui::flex_shrink
							| ftxui::Hoverable(set_help_text(std::experimental::make_observer
								(&custom_command_help_text))),
						ftxui::Input(&custom_command_cores_text, "") | ftxui::underlined
							| ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3)
							| ftxui::Renderer([&](ftxui::Element inner)
								{return ftxui::hbox(ftxui::text("Occupied CPU cores number: "), inner);})
							| ftxui::flex_shrink
							| ftxui::Hoverable(set_help_text(std::experimental::make_observer
								(&custom_command_cores_help_text)))
					}) | ftxui::Maybe([&]{return program_internal_names[program_selected] == "custom";})
				})
			}) | ftxui::Renderer([&](ftxui::Element inner)
				{return ftxui::vbox(ftxui::text("Basic settings") | ftxui::bgcolor(ftxui::Color::Blue), inner);}),
			// 高级设置
			ftxui::Container::Vertical
			({
				ftxui::Container::Horizontal
				({
					ftxui::Checkbox("Custom path: ", &custom_path_checked, checkbox_option),
					ftxui::Input(&custom_path_text, "") | ftxui::underlined
						| ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 30)
						| ftxui::flex_shrink
						| ftxui::Maybe([&]{return custom_path_checked;})
				}) | ftxui::Hoverable(set_help_text(std::experimental::make_observer(&custom_path_help_text)))
					| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());}),
				ftxui::Container::Horizontal
				({
					ftxui::Checkbox("Custom OpenMP threads number: ", &custom_openmp_threads_checked, checkbox_option),
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
				ftxui::Checkbox("Do not append \"-sf gpu\" to the command line", &no_gpu_sf_checked, checkbox_option)
					| ftxui::Hoverable(set_help_text(std::experimental::make_observer(&no_gpu_sf_help_text)))
					| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());})
					| ftxui::Maybe([&]
						{return program_internal_names[program_selected] == "lammps" && gpu_device_use_checked;}),
				ftxui::Checkbox("Run immeditally", &run_now_checked, checkbox_option)
					| ftxui::Hoverable(set_help_text(std::experimental::make_observer(&run_now_help_text)))
					| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());}),
				ftxui::Checkbox("Run in Ubuntu 22.04 container", &run_in_container_checked, checkbox_option)
					| ftxui::Hoverable(set_help_text(std::experimental::make_observer(&run_in_container_help_text)))
					| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());})
					| ftxui::Maybe([&]{return program_internal_names[program_selected] == "custom";})
			}) | ftxui::Renderer([&](ftxui::Element inner)
				{return ftxui::vbox(ftxui::text("Advanced settings, change it only if you know what you are doing") | ftxui::bgcolor(ftxui::Color::Blue), inner);}),
			// "几个按钮"
			ftxui::Container::Horizontal
			({
				submit_button,
				ftxui::Button("Cancel", screen.ExitLoopClosure())
			})
				| ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());})
		}) | ftxui::Renderer([&](ftxui::Element inner){return ftxui::window(ftxui::text("Submit new job"), inner);}),
		ftxui::Renderer([&]{return ftxui::window(ftxui::text("Help text"), ftxui::paragraph(help_text));})
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
				ftxui::text("Unacceptibal parameters") | ftxui::bgcolor(ftxui::Color::RedLight),
				ftxui::paragraph(error_dialog_text)
			);}) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 30),
			ftxui::Button("OK, I know", [&]{show_error_dialog = false;})
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

		// 为了putty可以正常显示，需要将 ▣/☐ 换为 [ ]/[*]
		auto checkbox_option = ftxui::CheckboxOption::Simple();
		checkbox_option.transform = [](const ftxui::EntryState& s)
		{
			auto prefix = ftxui::text(s.state ? "[X] " : "[ ] ");
			auto t = ftxui::text(s.label);
			if (s.active) t |= ftxui::bold;
			if (s.focused) t |= ftxui::inverted;
			return ftxui::hbox({prefix, t});
		};

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
								? ftxui::Checkbox("", &selected[i], checkbox_option)
								: ftxui::Renderer([]{return ftxui::emptyElement();})
						) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 4),
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
			}() | ftxui::vscroll_indicator | ftxui::frame | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 10)
				| ftxui::Renderer([&](ftxui::Element inner){return ftxui::window(ftxui::text("Job list"), inner);}),
			ftxui::Container::Horizontal
			({
				ftxui::Button("Refresh", [&]
				{
					please_refresh = true;
					checked_jobs.clear();
					for (std::size_t i = 0; i < selected.size(); i++)
						if (selected[i])
							checked_jobs.insert(jobs[i].Id);
					screen.ExitLoopClosure()();
				}),
				ftxui::Button("Cancel selected jobs and exit", [&]
				{
					checked_jobs.clear();
					for (std::size_t i = 0; i < selected.size(); i++)
						if (selected[i])
							checked_jobs.insert(jobs[i].Id);
					screen.ExitLoopClosure()();
				}),
				ftxui::Button("Exit without cancel any job", [&]
				{
					checked_jobs.clear();
					screen.ExitLoopClosure()();
				})
			}) | ftxui::Renderer([&](ftxui::Element inner){return ftxui::hbox(inner, ftxui::filler());}),
			ftxui::Renderer([&]{return ftxui::window(ftxui::text("Detail information"), detail);})
		});

		std::cout << "\x1b[?1000;1006;1015h" << std::endl;
		screen.Loop(layout);
		std::cout << "\x1b[?1000;1006;1015l" << std::endl;
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
