#include "stdafx.h"
#include "FEBiphasicSoluteSolver.h"
#include "FEBiphasicSoluteDomain.h"
#include "FEAnalysisStep.h"
#include "Interrupt.h"
#include "FEBioLib/log.h"

#ifdef WIN32
	#include <float.h>
	#define ISNAN(x) _isnan(x)
#endif

#ifdef LINUX
	#include <math.h>
	#define ISNAN(x) isnan(x)
#endif

#ifdef __APPLE__
#include <math.h>
#define ISNAN(x) isnan(x)
#endif

//-----------------------------------------------------------------------------
FEBiphasicSoluteSolver::FEBiphasicSoluteSolver(FEModel& fem) : FEBiphasicSolver(fem)
{
	m_Ctol = 0.01;
}

//-----------------------------------------------------------------------------
//! Allocates and initializes the data structures.
//
bool FEBiphasicSoluteSolver::Init()
{
	// initialize base class
	if (FEBiphasicSolver::Init() == false) return false;

	// allocate concentration-vectors
	assert (m_nceq[0] > 0);
	m_ci[0].assign(m_nceq[0], 0);
	m_Ci[0].assign(m_nceq[0], 0);
	
	// we need to fill the total displacement vector m_Ut
	// TODO: I need to find an easier way to do this
	FEMesh& mesh = m_fem.m_mesh;
	int i, n;
	for (i=0; i<mesh.Nodes(); ++i)
	{
		FENode& node = mesh.Node(i);

		// concentration dofs
		n = node.m_ID[DOF_C]; if (n >= 0) m_Ut[n] = node.m_ct[0];
	}

	return true;
}

//-----------------------------------------------------------------------------
//! Prepares the data for the first QN iteration. 
//!
void FEBiphasicSoluteSolver::PrepStep(double time)
{
	zero(m_Ci[0]);
	FEBiphasicSolver::PrepStep(time);
}

//-----------------------------------------------------------------------------
//! Implements the BFGS algorithm to solve the nonlinear FE equations.
//! The details of this implementation of the BFGS method can be found in:
//!   "Finite Element Procedures", K.J. Bathe, p759 and following
//!
bool FEBiphasicSoluteSolver::Quasin(double time)
{
	int i;
	double s;

	// convergence norms
	double	normR1;		// residual norm
	double	normE1;		// energy norm
	double	normD;		// displacement norm
	double	normd;		// displacement increment norm
	double	normRi;		// initial residual norm
	double	normEi;		// initial energy norm
	double	normEm;		// max energy norm
	double	normDi;		// initial displacement norm

	// poro convergence norms data
	double	normPi;		// initial pressure norm
	double	normP;		// current pressure norm
	double	normp;		// incremement pressure norm

	// solute convergence data
	double	normCi;	// initial concentration norm
	double	normC;	// current concentration norm
	double	normc;	// incremement concentration norm

	// initialize flags
	bool bconv = false;		// convergence flag
	bool breform = false;	// reformation flag

	// get the current step
	FEM& fem = dynamic_cast<FEM&>(m_fem);
	FEAnalysisStep* pstep = dynamic_cast<FEAnalysisStep*>(fem.GetCurrentStep());

	// make sure this is poro-solute problem
	assert(pstep->m_nModule == FE_POROSOLUTE);

	// prepare for the first iteration
	PrepStep(time);

	// check for CTRL+C interruption before we do any work
	if (fem.m_bInterruptable)
	{
		Interruption itr;
		if (itr.m_bsig)
		{
			itr.m_bsig = false;
			itr.interrupt();
		}
	}

	// calculate initial stiffness matrix
	if (ReformStiffness() == false) return false;

	// calculate initial residual
	if (Residual(m_bfgs.m_R0) == false) return false;

	m_bfgs.m_R0 += m_Fd;

	// TODO: I can check here if the residual is zero.
	// If it is than there is probably no force acting on the system
	// if (m_R0*m_R0 < eps) bconv = true;

//	double r0 = m_R0*m_R0;

	Logfile::MODE oldmode;

	clog.printf("\n===== beginning time step %d : %lg =====\n", pstep->m_ntimesteps+1, fem.m_ftime);

	// loop until converged or when max nr of reformations reached
	do
	{
		oldmode = clog.GetMode();
		if ((fem.GetCurrentStep()->GetPrintLevel() <= FE_PRINT_MAJOR_ITRS) &&
			(fem.GetCurrentStep()->GetPrintLevel() != FE_PRINT_NEVER)) clog.SetMode(Logfile::FILE_ONLY);

		clog.printf(" %d\n", m_niter+1);
		clog.SetMode(oldmode);

		// assume we'll converge. 
		bconv = true;

		// solve the equations
		m_SolverTime.start();
		{
			m_bfgs.SolveEquations(m_bfgs.m_ui, m_bfgs.m_R0);
		}
		m_SolverTime.stop();

		// check for nans
		if (fem.GetDebugFlag())
		{
			double du = m_bfgs.m_ui*m_bfgs.m_ui;
			if (ISNAN(du)) throw NANDetected();
		}

		// extract the pressure increments
		GetDisplacementData(m_di, m_bfgs.m_ui);

		// set initial convergence norms
		if (m_niter == 0)
		{
			normRi = fabs(m_bfgs.m_R0*m_bfgs.m_R0);
			normEi = fabs(m_bfgs.m_ui*m_bfgs.m_R0);
			normDi = fabs(m_di*m_di);
			normEm = normEi;
		}

		// perform a linesearch
		// the geometry is also updated in the line search
		if (m_bfgs.m_LStol > 0) s = m_bfgs.LineSearch();
		else
		{
			s = 1;

			// Update geometry
			Update(m_bfgs.m_ui);

			// calculate residual at this point
			Residual(m_bfgs.m_R1);
		}

		// update all degrees of freedom
		for (i=0; i<m_neq; ++i) m_Ui[i] += s*m_bfgs.m_ui[i];

		// update displacements
		for (i=0; i<m_ndeq; ++i) m_Di[i] += s*m_di[i];

		// calculate norms
		normR1 = m_bfgs.m_R1*m_bfgs.m_R1;
		normd  = (m_di*m_di)*(s*s);
		normD  = m_Di*m_Di;
		normE1 = s*fabs(m_bfgs.m_ui*m_bfgs.m_R1);

		// check residual norm
		if ((m_Rtol > 0) && (normR1 > m_Rtol*normRi)) bconv = false;	

		// check displacement norm
		if ((m_Dtol > 0) && (normd  > (m_Dtol*m_Dtol)*normD )) bconv = false;

		// check energy norm
		if ((m_Etol > 0) && (normE1 > m_Etol*normEi)) bconv = false;

		// check linestep size
		if ((m_bfgs.m_LStol > 0) && (s < m_bfgs.m_LSmin)) bconv = false;

		// check energy divergence
		if (normE1 > normEm) bconv = false;

		// check poroelastic convergence
		{
			// extract the pressure increments
			GetPressureData(m_pi, m_bfgs.m_ui);

			// set initial norm
			if (m_niter == 0) normPi = fabs(m_pi*m_pi);

			// update total pressure
			for (i=0; i<m_npeq; ++i) m_Pi[i] += s*m_pi[i];

			// calculate norms
			normP = m_Pi*m_Pi;
			normp = (m_pi*m_pi)*(s*s);

			// check convergence
			if ((m_Ptol > 0) && (normp > (m_Ptol*m_Ptol)*normP)) bconv = false;
		}

		// check solute convergence
		{
			// extract the pressure increments
			GetConcentrationData(m_ci[0], m_bfgs.m_ui);
			
			// set initial norm
			if (m_niter == 0) normCi = fabs(m_ci[0]*m_ci[0]);
			
			// update total pressure
			for (i=0; i<m_nceq[0]; ++i) m_Ci[0][i] += s*m_ci[0][i];
			
			// calculate norms
			normC = m_Ci[0]*m_Ci[0];
			normc = (m_ci[0]*m_ci[0])*(s*s);
			
			// check convergence
			if ((m_Ctol > 0) && (normc > (m_Ctol*m_Ctol)*normC)) bconv = false;
		}

		// print convergence summary
		oldmode = clog.GetMode();
		if ((fem.GetCurrentStep()->GetPrintLevel() <= FE_PRINT_MAJOR_ITRS) &&
			(fem.GetCurrentStep()->GetPrintLevel() != FE_PRINT_NEVER)) clog.SetMode(Logfile::FILE_ONLY);

		clog.printf(" Nonlinear solution status: time= %lg\n", time); 
		clog.printf("\tstiffness updates             = %d\n", m_bfgs.m_nups);
		clog.printf("\tright hand side evaluations   = %d\n", m_nrhs);
		clog.printf("\tstiffness matrix reformations = %d\n", m_nref);
		if (m_bfgs.m_LStol > 0) clog.printf("\tstep from line search         = %lf\n", s);
		clog.printf("\tconvergence norms :        INITIAL         CURRENT         REQUIRED\n");
		clog.printf("\t residual             %15le %15le %15le \n", normRi, normR1, m_Rtol*normRi);
		clog.printf("\t energy               %15le %15le %15le \n", normEi, normE1, m_Etol*normEi);
		clog.printf("\t displacement         %15le %15le %15le \n", normDi, normd ,(m_Dtol*m_Dtol)*normD );
		clog.printf("\t fluid pressure       %15le %15le %15le \n", normPi, normp ,(m_Ptol*m_Ptol)*normP );
		clog.printf("\t solute concentration %15le %15le %15le \n", normCi, normc ,(m_Ctol*m_Ctol)*normC );

		clog.SetMode(oldmode);

		// check if we have converged. 
		// If not, calculate the BFGS update vectors
		if (bconv == false)
		{
			if ((normR1 < m_Rmin))
			{
				// check for almost zero-residual on the first iteration
				// this might be an indication that there is no force on the system
				clog.printbox("WARNING", "No force acting on the system.");
				bconv = true;
			}
			else if (s < m_bfgs.m_LSmin)
			{
				// check for zero linestep size
				clog.printbox("WARNING", "Zero linestep size. Stiffness matrix will now be reformed");
				breform = true;
			}
			else if (normE1 > normEm)
			{
				// check for diverging
				clog.printbox("WARNING", "Problem is diverging. Stiffness matrix will now be reformed");
				normEm = normE1;
				normEi = normE1;
				normRi = normR1;
				normDi = normd;
				normPi = normp;
				normCi = normc;
				breform = true;
			}
			else
			{
				// If we havn't reached max nr of BFGS updates
				// do an update
				if (!breform)
				{
					if (m_bfgs.m_nups < m_bfgs.m_maxups-1)
					{
						if (m_bfgs.Update(s, m_bfgs.m_ui, m_bfgs.m_R0, m_bfgs.m_R1) == false)
						{
							// Stiffness update has failed.
							// this might be due a too large condition number
							// or the update was no longer positive definite.
							clog.printbox("WARNING", "The BFGS update has failed.\nStiffness matrix will now be reformed.");
							breform = true;
						}
					}
					else
					{
						// we've reached the max nr of BFGS updates, so
						// we need to do a stiffness reformation
						breform = true;

						// print a warning only if the user did not intent full-Newton
						if (m_bfgs.m_maxups > 0)
							clog.printbox("WARNING", "Max nr of iterations reached.\nStiffness matrix will now be reformed.");

					}
				}
			}	

			// zero displacement increments
			// we must set this to zero before the reformation
			// because we assume that the prescribed displacements are stored 
			// in the m_ui vector.
			zero(m_bfgs.m_ui);

			// reform stiffness matrices if necessary
			if (breform)
			{
				clog.printf("Reforming stiffness matrix: reformation #%d\n\n", m_nref);

				// reform the matrix
				if (ReformStiffness() == false) break;
	
				// reset reformation flag
				breform = false;
			}

			// copy last calculated residual
			m_bfgs.m_R0 = m_bfgs.m_R1;
		}
		else if (pstep->m_baugment)
		{
			// we have converged, so let's see if the augmentations have converged as well

			clog.printf("\n........................ augmentation # %d\n", m_naug+1);

			// do the augmentations
			bconv = Augment();

			// update counter
			++m_naug;

			// we reset the reformations counter
			m_nref = 0;
	
			// If we havn't converged we prepare for the next iteration
			if (!bconv) 
			{
				// Since the Lagrange multipliers have changed, we can't just copy 
				// the last residual but have to recalculate the residual
				// we also recalculate the stresses in case we are doing augmentations
				// for incompressible materials
				UpdateStresses();
				Residual(m_bfgs.m_R0);

				// reform the matrix if we are using full-Newton
				FEAnalysisStep* pstep = dynamic_cast<FEAnalysisStep*>(fem.GetCurrentStep());
				if (pstep->m_psolver->m_bfgs.m_maxups == 0)
				{
					clog.printf("Reforming stiffness matrix: reformation #%d\n\n", m_nref);
					if (ReformStiffness() == false) break;
				}
			}
		}
	
		// increase iteration number
		m_niter++;

		// let's flush the logfile to make sure the last output will not get lost
		clog.flush();

		// check for CTRL+C interruption
		if (fem.m_bInterruptable)
		{
			Interruption itr;
			if (itr.m_bsig)
			{
				itr.m_bsig = false;
				itr.interrupt();
			}
		}
	}
	while (bconv == false);

	// when converged, 
	// print a convergence summary to the clog file
	if (bconv)
	{
		Logfile::MODE mode = clog.SetMode(Logfile::FILE_ONLY);
		if (mode != Logfile::NEVER)
		{
			clog.printf("\nconvergence summary\n");
			clog.printf("    number of iterations   : %d\n", m_niter);
			clog.printf("    number of reformations : %d\n", m_nref);
		}
		clog.SetMode(mode);
	}

	// if converged we update the total displacements
	if (bconv)
	{
		m_Ut += m_Ui;
	}

	return bconv;
}

//-----------------------------------------------------------------------------
//! calculates the residual vector
//! Note that the concentrated nodal forces are not calculated here.
//! This is because they do not depend on the geometry 
//! so we only calculate them once (in Quasin) and then add them here.

bool FEBiphasicSoluteSolver::Residual(vector<double>& R)
{
	int i;
	FEM& fem = dynamic_cast<FEM&>(m_fem);
	double dt = fem.GetCurrentStep()->m_dt;

	// initialize residual with concentrated nodal loads
	R = m_Fn;

	// zero nodal reaction forces
	zero(m_Fr);

	// zero rigid body reaction forces
	int NRB = fem.m_RB.size();
	for (i=0; i<NRB; ++i)
	{
		FERigidBody& RB = fem.m_RB[i];
		RB.m_Fr = RB.m_Mr = vec3d(0,0,0);
	}

	// get the mesh
	FEMesh& mesh = m_fem.m_mesh;

/*	// loop over all domains
	for (i=0; i<mesh.Domains(); ++i) 
	{
		FEElasticDomain& dom = dynamic_cast<FEElasticDomain&>(mesh.Domain(i));
		dom.Residual(this, R);
	}
*/

	// internal stress work
	for (i=0; i<mesh.Domains(); ++i) 
	{
		FEElasticDomain& dom = dynamic_cast<FEElasticDomain&>(mesh.Domain(i));
		dom.InternalForces(this, R);
	}

	if (m_fem.GetCurrentStep()->m_nanalysis == FE_STEADY_STATE)
	{
		// internal fluid work
		for (i=0; i<mesh.Domains(); ++i) 
		{
			FEBiphasicSoluteDomain& dom = dynamic_cast<FEBiphasicSoluteDomain&>(mesh.Domain(i));
			dom.InternalFluidWorkSS(this, R, dt);
		}

		// internal solute work
		for (i=0; i<mesh.Domains(); ++i) 
		{
			FEBiphasicSoluteDomain& dom = dynamic_cast<FEBiphasicSoluteDomain&>(mesh.Domain(i));
			dom.InternalSoluteWorkSS(this, R, dt);
		}
	}
	else
	{
		// internal fluid work
		for (i=0; i<mesh.Domains(); ++i) 
		{
			FEBiphasicSoluteDomain& dom = dynamic_cast<FEBiphasicSoluteDomain&>(mesh.Domain(i));
			dom.InternalFluidWork(this, R, dt);
		}

		// internal solute work
		for (i=0; i<mesh.Domains(); ++i) 
		{
			FEBiphasicSoluteDomain& dom = dynamic_cast<FEBiphasicSoluteDomain&>(mesh.Domain(i));
			dom.InternalSoluteWork(this, R, dt);
		}
	}

	// calculate forces due to surface loads
	int nsl = (int) fem.m_SL.size();
	for (i=0; i<nsl; ++i)
	{
		FESurfaceLoad* psl = fem.m_SL[i];
		if (psl->IsActive()) psl->Residual(this, R);
	}

	// rigid joint forces
	if (!fem.m_RJ.empty())
	{
		for (int i=0; i<(int) fem.m_RJ.size(); ++i) fem.m_RJ[i]->JointForces(R);
	}

	// calculate contact forces
	if (m_fem.ContactInterfaces() > 0)
	{
		ContactForces(R);
	}

	// calculate linear constraint forces
	// note that these are the linear constraints
	// enforced using the augmented lagrangian
	LinearConstraintForces(R);

	// forces due to point constraints
	for (i=0; i<(int) fem.m_PC.size(); ++i) fem.m_PC[i].Residual(R);

	// set the nodal reaction forces
	// TODO: Is this a good place to do this?
	for (i=0; i<mesh.Nodes(); ++i)
	{
		FENode& node = mesh.Node(i);
		node.m_Fr = vec3d(0,0,0);

		int n;
		if ((n = -node.m_ID[DOF_X]-2) >= 0) node.m_Fr.x = -m_Fr[n];
		if ((n = -node.m_ID[DOF_Y]-2) >= 0) node.m_Fr.y = -m_Fr[n];
		if ((n = -node.m_ID[DOF_Z]-2) >= 0) node.m_Fr.z = -m_Fr[n];
	}

	// increase RHS counter
	m_nrhs++;

	return true;
}

//-----------------------------------------------------------------------------
void FEBiphasicSoluteSolver::GetConcentrationData(vector<double> &ci, vector<double> &ui)
{
	int N = m_fem.m_mesh.Nodes(), nid, m = 0;
	zero(ci);
	for (int i=0; i<N; ++i)
	{
		FENode& n = m_fem.m_mesh.Node(i);
		nid = n.m_ID[DOF_C];
		if (nid != -1)
		{
			nid = (nid < -1 ? -nid-2 : nid);
			ci[m++] = ui[nid];
			assert(m <= (int) ci.size());
		}
	}
}


//-----------------------------------------------------------------------------
//! Save data to dump file

void FEBiphasicSoluteSolver::Serialize(DumpFile& ar)
{
	FEBiphasicSolver::Serialize(ar);

	if (ar.IsSaving())
	{
		ar << m_Ctol;
	}
	else
	{
		ar >> m_Ctol;
	}
}
