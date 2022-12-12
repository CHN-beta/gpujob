# include <set>
# include <job.hpp>
# include <cxxopts.hpp>
# include <fmt/format.h>
# include <nameof.hpp>

using namespace std::literals;

int main(int argc, const char** argv)
{
	try
	{
		cxxopts::Options options("job-cli", "A command line interface for the simple job scheduler.");
		options.add_options()
			("action", "Action to do (\"submit\", \"list\", \"query\" or \"cancel\"). "
				"Use \"list\" to print all submitted jobs, no more arguments is needed. "
				"Use \"query\" to query detail information of a job, only job id (\"-id\", see below) is needed. "
				"Use \"cancel\" to cancel a submitted job, only job id (\"-id\", see below) is needed. "
				"For \"submit\", all the other arguments are needed.", cxxopts::value<std::string>())
			("id", "Job id, need to be provided only when query or cancel a job.", cxxopts::value<unsigned>())
			("program", "Program to run (\"vasp\", \"lammps\" or \"custom\").",
				cxxopts::value<std::string>()->default_value(""))
			("vasp-version", "VASP version (\"6.3.1\"), need to be provided only when running VASP.",
				cxxopts::value<std::string>()->default_value(""))
			("vasp-variant", "VASP Variant (\"std\", \"gam\" or \"ncl\"), "
				"need to be provided only when running VASP.",
				cxxopts::value<std::string>()->default_value(""))
			("gpu", "GPU id to use, separated by comma (for example, \"0,1\").",
				cxxopts::value<std::vector<unsigned>>()->default_value(""))
			("mpi-threads", "Number of MPI threads to use. "
				"Need to be provided only when running VASP on cpu or LAMMPS.",
				cxxopts::value<unsigned>()->default_value("0"))
			("openmp-threads", "Number of OpenMP threads to use. "
				"Need to be provided only when running VASP on CPU. "
				"Optional when running VASP on GPU (default to 2) or LAMMPS (default to 1).",
				cxxopts::value<unsigned>()->default_value("0"))
			("lammps-input", "File path of LAMMPS input script. Need to be provided only when running LAMMPS.",
				cxxopts::value<std::string>()->default_value(""))
			("custom-command", "Custom command to run, need to be provided only when select to run custom program.",
				cxxopts::value<std::string>()->default_value(""))
			("custom-command-cores",
				"Number of cores to use, need to be provided only when select to run custom program.",
				cxxopts::value<unsigned>()->default_value("0"))
			("run-path", "Run in custom directory (default is in current directory).",
				cxxopts::value<std::string>()->default_value(""))
			("no-gpu-sf", "Do not append \"-sf gpu\" in LAMMPS commandline.",
				cxxopts::value<bool>()->default_value("false"))
			("run-now", "Run the job immediately.", cxxopts::value<bool>()->default_value("false"))
			("run-in-container", "Run the job in ubuntu-22.04 container.",
				cxxopts::value<bool>()->default_value("false"));
		
		auto args = options.parse(argc, argv);

		if (args["action"].as<std::string>() == "submit")
		{
			Job_t job;
			job.Id = 0;
			job.Status = Job_t::Status_t::Pending;
			job.RunNow = args["run-now"].as<bool>();

			auto gpu = args["gpu"].as<std::vector<unsigned>>();

			if (args["program"].as<std::string>() == "vasp" && gpu.size())
			{
				auto run_path = args["run-path"].as<std::string>();
				if (run_path.empty())
				{
					run_path = std::filesystem::current_path();
					if (std::smatch match; std::regex_match(run_path, match, std::regex("/home(/.*)")))
						run_path = "/hosthome" + match[1].str();
				}
				auto openmp_threads = args["openmp-threads"].as<unsigned>();
				if (!openmp_threads)
					openmp_threads = 2;
				std::map<std::string, std::string> vasp_version_map {{"6.3.1", "631"}};
				auto vasp_version = args["vasp-version"].as<std::string>();
				if (!vasp_version_map.contains(vasp_version))
					throw std::invalid_argument{fmt::format("Invalid VASP version: {}", vasp_version)};
				vasp_version = vasp_version_map[vasp_version];
				std::set<std::string> vasp_variant_set {"std", "gam", "ncl"};
				auto vasp_variant = args["vasp-variant"].as<std::string>();
				if (!vasp_variant_set.contains(vasp_variant))
					throw std::invalid_argument{fmt::format("Invalid VASP variant: {}", vasp_variant)};

				job.Comment = fmt::format("{} {}", std::getenv("USER"), [&]
				{
					if (std::smatch match; std::regex_match(run_path, match, std::regex("/hosthome/[^/]+/(.*)")))
						return match[1].str();
					else
						return run_path;
				}());
				job.ProgramString = fmt::format
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
					gpu.size(), openmp_threads, fmt::join(gpu, ","), vasp_version, vasp_variant
				);
				job.UsingCores = gpu.size() * openmp_threads;
				job.UsingGpus = gpu;
				job.RunInContainer = true;
			}
			else if (args["program"].as<std::string>() == "vasp" && !gpu.size())
			{
				auto mpi_threads = args["mpi-threads"].as<unsigned>();
				if (!mpi_threads)
					throw std::invalid_argument{"MPI 线程数必须为正整数."};
				auto openmp_threads = args["openmp-threads"].as<unsigned>();
				if (!openmp_threads)
					throw std::invalid_argument{"OpenMP 线程数必须为正整数."};
				auto run_path = args["run-path"].as<std::string>();
				if (run_path.empty())
					run_path = std::filesystem::current_path();
				std::map<std::string, std::string> vasp_version_map {{"6.3.1", "631"}};
				auto vasp_version = args["vasp-version"].as<std::string>();
				if (!vasp_version_map.contains(vasp_version))
					throw std::invalid_argument{fmt::format("Invalid VASP version: {}", vasp_version)};
				vasp_version = vasp_version_map[vasp_version];
				std::set<std::string> vasp_variant_set {"std", "gam", "ncl"};
				auto vasp_variant = args["vasp-variant"].as<std::string>();
				if (!vasp_variant_set.contains(vasp_variant))
					throw std::invalid_argument{fmt::format("Invalid VASP variant: {}", vasp_variant)};

				job.Comment = fmt::format("{} {}", std::getenv("USER"), [&]
				{
					if (std::smatch match; std::regex_match(run_path, match, std::regex("/home/[^/]+/(.*)")))
						return match[1].str();
					else
						return run_path;
				}());
				job.ProgramString = fmt::format
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
					mpi_threads, openmp_threads, vasp_version, vasp_variant
				);
				job.UsingCores = mpi_threads * openmp_threads;
				job.RunInContainer = false;
			}
			else if (args["program"].as<std::string>() == "lammps")
			{
				auto mpi_threads = args["mpi-threads"].as<unsigned>();
				if (!mpi_threads)
					throw std::invalid_argument{"MPI 线程数必须为正整数."};
				auto openmp_threads = args["openmp-threads"].as<unsigned>();
				if (!openmp_threads)
					openmp_threads = 1;
				auto run_path = args["run-path"].as<std::string>();
				if (run_path.empty())
					run_path = std::filesystem::current_path();

				job.Comment = fmt::format("{} {}", std::getenv("USER"), [&]
				{
					if (std::smatch match; std::regex_match(run_path, match, std::regex("/home/[^/]+/(.*)")))
						return match[1].str();
					else
						return run_path;
				}());
				job.ProgramString = fmt::format
				(
					"cd '{}'; "
					"( "
						"echo start at $(date '+%Y-%m-%d %H:%M:%S') "
						"&& . /etc/profile.d/lammps.sh "
						"&& mpirun -n {} -x OMP_NUM_THREADS={} {}lmp -in '{}'{}"
						"&& echo end at $(date '+%Y-%m-%d %H:%M:%S') "
					") 2>&1 | tee -a output.txt",
					std::regex_replace(run_path, std::regex("'"), R"('"'"')"),
					mpi_threads, openmp_threads,
					gpu.size() ? fmt::format("-x CUDA_DEVICE_ORDER=PCI_BUS_ID -x CUDA_VISIBLE_DEVICES={} ",
						fmt::join(gpu, ",")) : ""s,
					std::regex_replace(args["lammps-input"].as<std::string>(), std::regex("'"), R"('"'"')"),
					gpu.size()
						? fmt::format("{} -pk gpu {}", args["no-gpu-sf"].as<bool>() ? ""s
							: " -sf gpu"s, gpu.size()) : ""s
				);
				job.UsingCores = mpi_threads * openmp_threads;
				job.UsingGpus = gpu;
				job.RunInContainer = false;
			}
			else if (args["program"].as<std::string>() == "custom")
			{
				auto cores = args["custom-command-cores"].as<unsigned>();
				if (!cores)
					throw std::invalid_argument{"核心数必须为正整数."};
				auto run_path = args["run-path"].as<std::string>();
				if (run_path.empty())
					run_path = std::filesystem::current_path();

				job.Comment = fmt::format("{} {}", std::getenv("USER"), [&]
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
				job.ProgramString = fmt::format
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
					std::to_string(cores),
					gpu.size() ? fmt::format
					(
						"&& export GPUJOB_USE_GPU=1 "
						"&& export CUDA_DEVICE_ORDER=PCI_BUS_ID "
						"&& export CUDA_VISIBLE_DEVICES={} ",
						fmt::join(gpu, ",")
					): ""s,
					args["custom-command"].as<std::string>()
				);
				job.UsingCores = cores;
				job.UsingGpus = gpu;
				job.RunInContainer = args["run-in-container"].as<bool>();
			}
			else
				throw std::invalid_argument
					{fmt::format("program '{}' not recognized.", args["program"].as<std::string>())};
		}
		else if (args["action"].as<std::string>() == "list")
		{
			auto [jobs] = read_out();
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
			for (auto& job : jobs)
				std::cout << fmt::format("{} {} {}\n", job.Id, nameof::nameof_enum(job.Status), job.Comment);
		}
		else if (args["action"].as<std::string>() == "query")
		{
			auto id = args["id"].as<unsigned>();
			auto [jobs] = read_out();
			auto it = std::find_if(jobs.begin(), jobs.end(), [&](auto& job)
			{
				return job.Id == id;
			});
			if (it == jobs.end())
				throw std::invalid_argument{fmt::format("id {} not found.", id)};
			std::cout << fmt::format
			(
				"ID: {}\nUser: {}\nProgramString: {}\nComment: {}\nUsingCores: {}\nUsingGpus: {}\nStatus: {}\n"
					"RunInContainer: {}\nRunNow: {}\n",
				it->Id, it->User, it->ProgramString, it->Comment, it->UsingCores, fmt::join(it->UsingGpus, ","),
				nameof::nameof_enum(it->Status), it->RunInContainer, it->RunNow
			);
		}
		else if (args["action"].as<std::string>() == "cancel")
		{
			auto id = args["id"].as<unsigned>();
			write_in({{}, {{id, ""}}});
		}
		else
			throw std::invalid_argument{fmt::format("action {} not recognized.", args["action"].as<std::string>())};
	}
	catch (const std::exception& e)
	{
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}
	catch (...)
	{
		std::cerr << "Unknown error." << std::endl;
		return 1;
	}
}