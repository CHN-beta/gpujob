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

std::map<std::string, std::string> request_new_job_detail_from_user
    (std::vector<std::tuple<std::optional<unsigned>, std::string, unsigned, unsigned>> devices)
// 展示提交任务的界面，并等待用户输入、确认。所有参数使用字符串传回。保证传回的结果已经被检查过，不需要再次检查。
// 传入的参数依次是：每个设备的 id、名称、正在运行的任务数、正在等待的任务数。CPU 的 id 为空。
// 返回的字典中包括以下参数：
//  基本信息：
//      "Canceled": 存在这个键时，表示用户取消了提交任务，这时以下参数都可以不存在。
//      "Program": "Vasp" 或 "Lammps" 或 "Custom"，表明用户选择的程序。
//      "VaspVersion": "6.3.0" 或 "6.3.1"，表明用户选择的 VASP 版本。只当用户选择 VASP 时存在。
//      "VaspVariant": "std" 或 "gam" 或 "ncl"，表明用户选择的 VASP 变体。只当用户选择 VASP 时存在。
//      "Device": 用户选择的设备，与传入的参数对应。空 id 则对应空字符串。
//      "LammpsScript": 表明用户选择的 LAMMPS 输入脚本。只当用户选择 LAMMPS 时存在。不为空字符串
//          可能有空格之类等奇怪字符，随后使用 bash 调用时记得加单引号。
//      "MpiThreads": 用户选择的 MPI 线程数，为正整数。只当用户选择 CPU 且选择 VASP 或 LAMMPS 时存在。
//      "OpenmpThreads": 用户选择的 OpenMP 线程数，为正整数。只当用户选择 CPU 且选择 VASP 或 LAMMPS 时存在。
//      "CustomCommand": 用户选择的自定义命令。只当用户选择自定义命令时存在。不为空字符串。
//      "CustomCommandCores": 用户选择的自定义命令的 CPU 核心数，为正整数。只当用户选择自定义命令时存在。
//  高级设置：
//      "CustomPath": 存在这个键时，表示用户选择了自定义路径，这个键对应的值是用户输入的路径。不为空字符串。
//      "CustomOpenmpThreads": 用户选择的 OpenMP 线程数，为正整数。只当用户选择 GPU 且选择 VASP 或 LAMMPS 时存在。
//      "RunNow": 立即开始运行。
{
    auto screen = ftxui::ScreenInteractive::Fullscreen();

    std::vector<std::string> program_names {"VASP", "LAMMPS", "自定义程序"};
    std::vector<std::string> program_internal_names {"Vasp", "Lammps", "Custom"};
    int program_selected = 0;
    auto program_dropdown = ftxui::Dropdown(&program_names, &program_selected);

    std::vector<std::string> vasp_version_names {"6.3.0", "6.3.1"};
    int vasp_version_selected {0};
    auto vasp_version_dropdown = ftxui::Dropdown(&vasp_version_names, &vasp_version_selected);

    std::vector<std::string> vasp_variant_names {"std", "gam", "ncl"};
    int vasp_variant_selected = 0;
    auto vasp_variant_dropdown = ftxui::Dropdown(&vasp_variant_names, &vasp_variant_selected);

    std::vector<std::string> device_names;
    for (auto& [device_id, device_name, device_running, device_pending] : devices)
        device_names.push_back(fmt::format(
            "{}: {} ({}{} 运行, {} 等待)",
            device_id ? "GPU" : "CPU", device_name, device_id ? fmt::format("ID: {}, ", *device_id) : "",
            device_running, device_pending
        ));
    int device_selected = 0;
    auto device_dropdown = ftxui::Dropdown(&device_names, &device_selected);

    std::string lammps_script_text = "in.lammps";
    auto lammps_script_input = ftxui::Input(&lammps_script_text, "") | ftxui::flex_shrink;

    std::string mpi_threads_text = "4";
    auto mpi_threads_input = ftxui::Input(&mpi_threads_text, "");

    std::string openmp_threads_text = "4";
    auto openmp_threads_input = ftxui::Input(&openmp_threads_text, "");

    std::string custom_command_text;
    auto custom_command_input = ftxui::Input(&custom_command_text, "");

    std::string custom_command_cores_text = "4";
    auto custom_command_cores_input = ftxui::Input(&custom_command_cores_text, "");

    bool custom_path_checked = false;
    std::string custom_path_text = "/home/username";
    auto custom_path_checkbox = ftxui::Checkbox("自定义路径：", &custom_path_checked);
    auto custom_path_input = ftxui::Input(&custom_path_text, "");
        

    bool custom_openmp_threads_checked = false;
    std::string custom_openmp_threads_text = "2";
    auto custom_openmp_threads_checkbox = ftxui::Checkbox("自定义 OpenMP 线程数：", &custom_openmp_threads_checked);
    auto custom_openmp_threads_input = ftxui::Input(&custom_openmp_threads_text, "");

    bool run_now_checked = false;
    auto run_now_checkbox = ftxui::Checkbox("立即开始运行", &run_now_checked);

    auto submit_button = ftxui::Button("提交任务", screen.ExitLoopClosure());
    auto quit_button = ftxui::Button("取消", screen.ExitLoopClosure());

    auto layout = ftxui::Container::Vertical
    ({
        ftxui::Container::Horizontal
        ({
            program_dropdown,
            ftxui::Container::Horizontal({vasp_version_dropdown, vasp_variant_dropdown})
                | ftxui::Maybe([&]{return program_internal_names[program_selected] == "Vasp";}),
            device_dropdown
        }),
        ftxui::Container::Vertical
        ({
            lammps_script_input | ftxui::Maybe([&]{return program_internal_names[program_selected] == "Lammps";}),
            ftxui::Container::Horizontal({mpi_threads_input, openmp_threads_input}) | ftxui::Maybe([&]
            {
                return (program_internal_names[program_selected] == "Vasp"
                    || program_internal_names[program_selected] == "Lammps")
                    && std::get<0>(devices[device_selected]) == std::nullopt;
            }),
            ftxui::Container::Horizontal({custom_command_input, custom_command_cores_input})
                | ftxui::Maybe([&]{return program_internal_names[program_selected] == "Custom";})
        }),
        ftxui::Container::Vertical
        ({
            ftxui::Container::Horizontal
            ({
                custom_path_checkbox,
                custom_path_input | ftxui::Maybe([&]{return custom_path_checked;})
            }),
            ftxui::Container::Horizontal
            ({
                custom_openmp_threads_checkbox,
                custom_openmp_threads_input | ftxui::Maybe([&]{return custom_openmp_threads_checked;})
            }) | ftxui::Maybe([&]
            {
                return (program_internal_names[program_selected] == "Vasp"
                    || program_internal_names[program_selected] == "Lammps")
                    && std::get<0>(devices[device_selected]) != std::nullopt;
            }),
            run_now_checkbox
        }),
        submit_button
    });

    auto renderer = ftxui::Renderer
    (
        layout,
        [&]
        {
            ftxui::Elements line1;  // 第一行的内容，包括几个 dropdown，横向排版，最后加一个 filler
            ftxui::Elements line2;  // 第二行的内容，包括几个 input，纵向排版，每一个都需要加一个 filler
            ftxui::Elements line3;  // 第三行的内容（高级设置），同样每一行加一个 filler
            
            line1.push_back(program_dropdown->Render());
            if (program_internal_names[program_selected] == "Vasp")
            {
                line1.push_back(vasp_version_dropdown->Render());
                line1.push_back(vasp_variant_dropdown->Render());
            }
            line1.push_back(device_dropdown->Render());
            line1.push_back(ftxui::filler());

            if (program_internal_names[program_selected] == "Lammps")
                line2.push_back(ftxui::hbox
                (
                    ftxui::text(L"LAMMPS 输入脚本文件: "),
                    lammps_script_input->Render()
                        | ftxui::underlined | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 15)
                ));
            if
            (
                (program_internal_names[program_selected] == "Vasp"
                || program_internal_names[program_selected] == "Lammps")
                && std::get<0>(devices[device_selected]) == std::nullopt
            )
            {
                line2.push_back(ftxui::hbox
                (
                    ftxui::text("MPI 线程数: "),
                    mpi_threads_input->Render() | ftxui::underlined | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3),
                    ftxui::filler()
                ));
                line2.push_back(ftxui::hbox
                (
                    ftxui::text("OpenMP 线程数: "),
                    openmp_threads_input->Render()
                        | ftxui::underlined | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3),
                    ftxui::filler()
                ));
            }
            if (program_internal_names[program_selected] == "Custom")
            {
                line2.push_back(ftxui::hbox
                (
                    ftxui::text("自定义命令: "),
                    custom_command_input->Render()
                        | ftxui::underlined | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 30),
                    ftxui::filler()
                ));
                line2.push_back(ftxui::hbox
                (
                    ftxui::text("占用 CPU 核心数: "),
                    custom_command_cores_input->Render()
                        | ftxui::underlined | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3),
                    ftxui::filler()
                ));
            }

            if (custom_path_checked)
                line3.push_back(ftxui::hbox
                (
                    custom_path_checkbox->Render(),
                    custom_path_input->Render()
                        | ftxui::underlined | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 30),
                    ftxui::filler()
                ));
            else
                line3.push_back(ftxui::hbox
                (
                    custom_path_checkbox->Render(),
                    custom_path_input->Render() | ftxui::underlined
                        | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 30) | ftxui::color(ftxui::Color::GrayDark),
                    ftxui::filler()
                ));

            if
            (
                (program_internal_names[program_selected] == "Vasp"
                || program_internal_names[program_selected] == "Lammps")
                && std::get<0>(devices[device_selected]) != std::nullopt
            )
            {
                if (custom_openmp_threads_checked)
                    line3.push_back(ftxui::hbox
                    (
                        custom_openmp_threads_checkbox->Render(),
                        custom_openmp_threads_input->Render()
                            | ftxui::underlined | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3),
                        ftxui::filler()
                    ));
                else
                    line3.push_back(ftxui::hbox
                    (
                        custom_openmp_threads_checkbox->Render(),
                        custom_openmp_threads_input->Render() | ftxui::underlined
                            | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 3) | ftxui::color(ftxui::Color::GrayDark),
                        ftxui::filler()
                    ));
            }
            line3.push_back(ftxui::hbox(run_now_checkbox->Render(), ftxui::filler()));

            return ftxui::vbox(ftxui::hbox(ftxui::window(ftxui::text("提交新任务"), ftxui::vbox
            (
                ftxui::text("基本信息：") | ftxui::bgcolor(ftxui::Color::Blue),
                ftxui::hbox(line1, ftxui::filler),
                ftxui::vbox(line2),
                ftxui::separatorLight(),
                ftxui::text("高级设置：") | ftxui::bgcolor(ftxui::Color::Blue),
                ftxui::hbox(ftxui::vbox(line3), ftxui::filler),
                ftxui::hbox(submit_button->Render(), ftxui::filler)
            )), ftxui::filler), ftxui::filler);
        }
    );
    screen.Loop(renderer);
    return {};
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
