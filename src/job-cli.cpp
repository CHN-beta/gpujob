# include <job.hpp>
# include <cxxopts.hpp>

int main(int argc, const char** argv)
{
	try
	{
		cxxopts::Options options("job-cli", "A command line interface for the simple job scheduler.");
		options.add_options()
			("a,action", "Action to do (\"submit\", \"list\" or \"cancel\"). "
				"For \"list\", no more arguments is needed. "
				"For \"cancel\", only job id (\"-id\", see below) is needed. "
				"For \"submit\", all the other arguments are needed.", cxxopts::value<std::string>())
			("p,program", "Program to run (\"vasp\", \"lammps\" or \"custom\").",
				cxxopts::value<std::string>()->default_value(""))
			("vve,vasp-version", "VASP version (\"6.3.1\"), need to be provided only when running VASP.",
				cxxopts::value<std::string>()->default_value(""))
			("vva,vasp-variant", "VASP Variant (\"std\", \"gam\" or \"ncl\"), "
				"need to be provided only when running VASP.",
				cxxopts::value<std::string>()->default_value(""))
			("g,gpu", "GPU id to use, separated by comma (for example, \"0,1\").",
				cxxopts::value<std::vector<unsigned>>()->default_value({}))
			("mpit,mpi-threads", "Number of MPI threads to use. "
				"Need to be provided only when running VASP on cpu or LAMMPS.",
				cxxopts::value<unsigned>()->default_value(0))
			("omp,openmp-threads", "Number of OpenMP threads to use. "
				"Need to be provided only when running VASP on CPU. "
				"Optional when running VASP on GPU (default to 2) or LAMMPS (no default).",
				cxxopts::value<unsigned>()->default_value(0))
			("cc,custom-command", "Custom command to run, need to be provided only when select to run custom program.",
				cxxopts::value<std::string>()->default_value(""))
			("ccc,custom-command-cores",
				"Number of cores to use, need to be provided only when select to run custom program.",
				cxxopts::value<unsigned>()->default_value(0))
			("cp,custom-path", "Run in custom directory (default is in current directory).",
				cxxopts::value<std::string>()->default_value(""))
			("rn,run-now", "Run the job immediately.", cxxopts::value<bool>()->default_value("false"))
			("ric,run-in-container", "Run the job in ubuntu-22.04 container.",
				cxxopts::value<bool>()->default_value("false"));
		
		auto args = options.parse(argc, argv);

		if (args["action"].as<std::string>() == "submit")
		{
			Job_t job;
			job.Id = 0;

			// set Program and Environment
			if (args["program"].as<std::string>() == "vasp")
			{
				job.Program = "vasp";
				if (auto gpu = args["gpu"].as<std::vector<unsigned>>(); gpu.size())
				{
					job.Environment["GPUJOB_USING_GPU"] = "1";
					job.Environment["GPUJOB_GPUS"] = fmt::format("{}", fmt::join(gpu, ","));
					job.Environment["GPUJOB_MPI_THREADS"] = fmt::format("{}", gpu.size());
					if (auto omp = args["openmp-threads"].as<unsigned>())
						job.Environment["GPUJOB_OPENMP_THREADS"] = fmt::format("{}", omp);
					else
						job.Environment["GPUJOB_OPENMP_THREADS"] = fmt::format("{}", 2);
				}
				else
				{
					job.Environment["GPUJOB_USING_GPU"] = "0";
					if (!args["mpi-threads"].as<unsigned>())
						throw std::invalid_argument{"unrecognized number of MPI threads."};
					job.Environment["GPUJOB_MPI_THREADS"] = fmt::format("{}", args["mpi-threads"].as<unsigned>());
					if (!args["openmp-threads"].as<unsigned>())
						throw std::invalid_argument{"unrecognized number of OpenMP threads."};
					job.Environment["GPUJOB_OPENMP_THREADS"] = fmt::format("{}", args["openmp-threads"].as<unsigned>());
				}
				if (!std::set<std::string>{"6.3.1"}.contains(args["vasp-version"].as<std::string>()))
					throw std::invalid_argument("Unsupported VASP version.");
				job.Environment["GPUJOB_VASP_VERSION"] = args["vasp-version"].as<std::string>();
				if (!std::set<std::string>{"std", "gam", "ncl"}.contains(args["vasp-variant"].as<std::string>()))
					throw std::invalid_argument("Unsupported VASP variant.");
				job.Environment["GPUJOB_VASP_VARIANT"] = args["vasp-variant"].as<std::string>();
			}
			else if (args["program"].as<std::string>() == "lammps")
			{
				job.Program = "lammps";
				if (auto gpu = args["gpu"].as<std::vector<unsigned>>(); gpu.size())
				{
					job.Environment["GPUJOB_USING_GPU"] = "1";
					job.Environment["GPUJOB_GPUS"] = fmt::format("{}", fmt::join(gpu, ","));
					job.Environment["GPUJOB_LAMMPS_GPUSF"] = "1";
				}
				else
					job.Environment["GPUJOB_USING_GPU"] = "0";
				if (!args["mpi-threads"].as<unsigned>())
					throw std::invalid_argument{"unrecognized number of MPI threads."};
				job.Environment["GPUJOB_MPI_THREADS"] = fmt::format("{}", args["mpi-threads"].as<unsigned>());
				if (auto omp = args["openmp-threads"].as<unsigned>())
				{
					job.Environment["GPUJOB_LAMMPS_OPENMP"] = "1";
					job.Environment["GPUJOB_OPENMP_THREADS"] = fmt::format("{}", omp);
				}
				else
					job.Environment["GPUJOB_LAMMPS_OPENMP"] = "0";
				job.Environment["GPUJOB_LAMMPS_INPUT"] = "lammps.in";
			}
			else if (args["program"].as<std::string>() == "custom")
			{
				if (!args["custom-command"].as<std::string>().size())
					throw std::invalid_argument("custom-command not specified.");
				job.Program = args["custom-command"].as<std::string>();
				if (!args["custom-command-cores"].as<unsigned>())
					throw std::invalid_argument("custom-command-cores not specified.");
				job.Environment["GPUJOB_MPI_THREADS"] = fmt::format("{}", args["custom-command-cores"].as<unsigned>())
			}
			else
				throw std::invalid_argument("program not recognized.");
			
		}
		else if (args["action"].as<std::string>() == "list")
		{

		}
		else if (args["action"].as<std::string>() == "cancel")
		{

		}
		else
			throw std::invalid_argument("action not recognized.");
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