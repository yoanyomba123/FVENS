/** \file threads_async_tests.cpp
 * \brief Implementation of tests related to multi-threaded asynchronous preconditioning
 * \author Aditya Kashi
 * \date 2018-03
 */

#include <iostream>
#include <iomanip>
#include <string>
#include <omp.h>
#include <petscksp.h>

#include "utilities/afactory.hpp"
#include "utilities/aerrorhandling.hpp"
#include "linalg/alinalg.hpp"
#include "ode/aodesolver.hpp"
#include "mesh/ameshutils.hpp"
#include "utilities/casesolvers.hpp"

#include <blasted_petsc.h>

#include "threads_async_tests.hpp"

namespace perftest {

// Field width in the report file
const int field_width = 11;

using namespace fvens;

/// Set -blasted_async_sweeps in the default Petsc options database and throw if not successful
static void set_blasted_sweeps(const int nbswp, const int naswp);

static void writeHeaderToFile(std::ofstream& outf, const int width)
{
	outf << '#' << std::setw(width) << "threads"
	     << std::setw(width) << "b&a-sweeps"
	     << std::setw(width+5) << "factor-speedup" << std::setw(width+5) << "apply-speedup"
	     << std::setw(width+5) << "total-speedup" << std::setw(width+5) << "total-deviate"
	     << std::setw(width) << "cpu-time" << std::setw(width+6) << "total-lin-iters"
	     << std::setw(width+4) << "avg-lin-iters"
	     << std::setw(width+1) << "time-steps"<< std::setw(width/2+1) << "conv?"
	     << std::setw(width) << "nl-speedup"
	     << "\n#---\n";
}

static void writeTimingToFile(std::ofstream& outf, const int w, const bool comment,
                              const TimingData& tdata, const int numthreads,
                              const int nbswps, const int naswps,
                              const double factorspeedup, const double applyspeedup,
                              const double precspeedup, const double precdeviate,
                              const double preccputime, const double fvens_wall_spdp)
{
	outf << (comment ? '#' : ' ') << std::setw(w) << numthreads
	     << std::setw(w/2) << nbswps << std::setw(w/2) << naswps
	     << std::setw(w+5) << factorspeedup << std::setw(w+5) << applyspeedup
	     << std::setw(w+5) << precspeedup << std::setw(w+5) << precdeviate
	     << std::setw(w) << preccputime
	     << std::setw(w+6) << tdata.total_lin_iters << std::setw(w+4) << tdata.avg_lin_iters
	     << std::setw(w+1) << tdata.num_timesteps
	     << std::setw(w/2+1) << (tdata.converged ? 1 : 0)
	     << std::setw(w) << fvens_wall_spdp
	     << (comment ? "\n#---\n" : "\n") << std::flush;
}

static double std_deviation(const double *const vals, const double avg, const int N) {
	double deviate = 0;
	for(int j = 0; j < N; j++)
		deviate += (vals[j]-avg)*(vals[j]-avg);
	deviate = std::sqrt(deviate/(double)N);
	return deviate;
}

/** Carries out a run of the 'main' solve for one factor- and apply-sweeps setting and one thread
 * setting.
 * The run is regarded as converged only if each repetition converged.
 * \return An arrray containing, in order, the factor walltime, apply wall time and ODE wall time.
 */
static std::array<double,3>
runSweepThreads(const Vec u, const FlowCase& flowcase, const Spatial<a_real,NVARS> *const prob,
                const int nbswps, const int naswps, const int numthreads, const int numrepeat,
                const double factor_basewtime, const double apply_basewtime, const double ode_basewtime,
                const bool basecase, const std::string perflogfile, std::ofstream& perftestout)
{
	TimingData tdata = {0,0,0,0,0,0,0,0,0,true,0,0,0};
	double precwalltime = 0, preccputime = 0, factorwalltime = 0, applywalltime = 0;
	std::vector<double> precwalltimearr(numrepeat);

	std::ofstream convout(perflogfile, std::ofstream::app);
	if(basecase)
		convout << "# Base case: ";
	else
		convout << "# Case: ";
	convout << "Sweeps=(" << nbswps << "," << naswps << "), threads=" << numthreads << "\n";
	convout.close();

	omp_set_num_threads(numthreads);
	set_blasted_sweeps(nbswps,naswps);

	int irpt;
	for(irpt = 0; irpt < numrepeat; irpt++) 
	{
		convout.open(perflogfile, std::ofstream::app);
		convout << "#  Run " << irpt << "\n";
		convout.close();

		Vec ut;
		int ierr = VecDuplicate(u, &ut); petsc_throw(ierr, "Vec duplicate");
		ierr = VecCopy(u, ut); petsc_throw(ierr, "Vec copy");

		TimingData td = flowcase.execute_main(prob, ut);

		ierr = VecDestroy(&ut); petsc_throw(ierr, "Vec destroy");

		if(!td.converged) {
			tdata.converged = false;
			break;
		}

		tdata.nelem = td.nelem;
		tdata.num_threads = td.num_threads;
		tdata.lin_walltime += td.lin_walltime;
		tdata.lin_cputime +=  td.lin_cputime;
		tdata.ode_walltime += td.ode_walltime;
		tdata.ode_cputime +=  td.ode_cputime;
		tdata.total_lin_iters += td.total_lin_iters;
		tdata.num_timesteps += td.num_timesteps;
		tdata.converged = tdata.converged && td.converged;
		factorwalltime += td.precsetup_walltime;
		applywalltime += td.precapply_walltime;
		precwalltime += td.precsetup_walltime + td.precapply_walltime;
		preccputime += td.prec_cputime;
		precwalltimearr[irpt] = td.precsetup_walltime + td.precapply_walltime;
	}

	tdata.lin_walltime /= (double)irpt;
	tdata.lin_cputime /= (double)irpt;
	tdata.ode_walltime /= (double)irpt;
	tdata.ode_cputime /= (double)irpt;
	tdata.num_timesteps = (int) std::round(tdata.num_timesteps / (double)irpt);
	tdata.total_lin_iters = (int) std::round(tdata.total_lin_iters / (double)irpt);
	tdata.avg_lin_iters = (int)std::round(tdata.total_lin_iters/(double)tdata.num_timesteps);
	factorwalltime /= (double)irpt;
	applywalltime /= (double)irpt;
	precwalltime /= (double)irpt;
	preccputime /= (double)irpt;

	const double precdeviate = std_deviation(&precwalltimearr[0], precwalltime, irpt)/precwalltime;

	int mpirank;
	MPI_Comm_rank(PETSC_COMM_WORLD, &mpirank);
	if(mpirank == 0)
	{
		if(basecase) {
			perftestout << "# Base preconditioner wall time = " << precwalltime << "; factor time = "
			     << factorwalltime << ", apply time = "<< applywalltime
			     << ", nonlinear solve time = " << tdata.ode_walltime << "\n#---\n";

			writeHeaderToFile(perftestout, field_width);
			writeTimingToFile(perftestout, field_width, true,tdata, 1, 1,1, 1.0, 1.0, 1.0,
			                  precdeviate, preccputime, 1.0);
		}
		else {
			const double prec_basewtime = factor_basewtime + apply_basewtime;
			writeTimingToFile(perftestout, field_width, false,tdata, numthreads, nbswps, naswps,
			                  factor_basewtime/factorwalltime,apply_basewtime/applywalltime,
			                  prec_basewtime/precwalltime, precdeviate, preccputime,
			                  ode_basewtime/tdata.ode_walltime);
		}
	}

	return {factorwalltime, applywalltime, tdata.ode_walltime};
}

StatusCode test_speedup_sweeps(const FlowParserOptions& opts, const FlowCase& flowcase,
                               const int numrepeat, const int baserepeats,
                               const SpeedupSweepsConfig config,
                               std::ofstream& outf)
{
	StatusCode ierr = 0;

	// Set up mesh
	const UMesh2dh<a_real> m = constructMesh(opts,"");
	const Spatial<a_real,NVARS> *const prob = createFlowSpatial(opts, m);

	// Prepate convergence historu file
	const std::string perflogfile = opts.logfile + ".conv";
	std::ofstream convout(perflogfile);
	convout << "# Perftest: sweeps and threads for async preconditioner from BLASTed\n";
	convout << "# Number of repeats per run = " << numrepeat << "\n";

	// solution vector
	Vec u;
	ierr = VecCreateSeq(PETSC_COMM_SELF, m.gnelem()*NVARS, &u);
	prob->initializeUnknowns(u);

	// starting computation

	omp_set_num_threads(1);
	set_blasted_sweeps(1,1);

	convout << "# Startup solve: Sweeps=(1,1), threads=1\n";
	convout.close();

	flowcase.execute_starter(prob, u);

	// Base run

	int mpirank;
	MPI_Comm_rank(PETSC_COMM_WORLD, &mpirank);
	if(mpirank == 0) {
		outf << "# Preconditioner wall times #\n# num-cells = " << m.gnelem() << "\n";
	}

	const std::array<double,3> basetimes =
		runSweepThreads(u, flowcase, prob, config.basebuildsweeps, config.baseapplysweeps,
		                config.basethreads, baserepeats, 1,1,1, true, perflogfile, outf);

	const double factor_basewtime = basetimes[0];
	const double apply_basewtime = basetimes[1];
	const double ode_basewtime = basetimes[2];

	// Perftesting runs

	// Carry out multi-thread runs
	for (size_t i = 0; i < config.buildSwpSeq.size(); i++)
	{
		set_blasted_sweeps(config.buildSwpSeq[i], config.applySwpSeq[i]);

		for(int numthreads : config.threadSeq)
		{
			runSweepThreads(u, flowcase, prob, config.buildSwpSeq[i], config.applySwpSeq[i], numthreads,
			                numrepeat, factor_basewtime, apply_basewtime, ode_basewtime,
			                false, perflogfile, outf);
		}
	}

	delete prob;
	ierr = VecDestroy(&u); CHKERRQ(ierr);
	
	return ierr;
}

void set_blasted_sweeps(const int nbswp, const int naswp)
{
	// add option
	std::string value = std::to_string(nbswp) + "," + std::to_string(naswp);
	int ierr = PetscOptionsSetValue(NULL, "-blasted_async_sweeps", value.c_str());
	petsc_throw(ierr, "Couldn't set PETSc option for BLASTed async sweeps");

	// Check
	int checksweeps[2];
	int nmax = 2;
	PetscBool set = PETSC_FALSE;
	ierr = PetscOptionsGetIntArray(NULL,NULL,"-blasted_async_sweeps",checksweeps,&nmax,&set);
	petsc_throw(ierr, "Could not get int array!");
	fvens_throw(checksweeps[0] != nbswp || checksweeps[1] != naswp, 
			"Async sweeps not set properly!");
}

}

