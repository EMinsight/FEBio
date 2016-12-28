#include "FEHeatSolver.h"
#include "FEHeatFlux.h"
#include "FEConvectiveHeatFlux.h"
#include "FEHeatTransferMaterial.h"
#include "FEHeatSource.h"
#include "FECore/FEModel.h"
#include "FECore/FEAnalysis.h"
#include "FECore/BC.h"

//-----------------------------------------------------------------------------
// define the parameter list
BEGIN_PARAMETER_LIST(FEHeatSolver, FELinearSolver)
END_PARAMETER_LIST();

//-----------------------------------------------------------------------------
//! constructor for the class
FEHeatSolver::FEHeatSolver(FEModel* pfem) : FELinearSolver(pfem)
{
	m_brhs = false;
	m_ntotref = 0;
	m_niter = 0;
	m_nrhs = 0;

	// Allocate degrees of freedom
	DOFS& dofs = pfem->GetDOFS();
	int varT = dofs.AddVariable("temperature");
	dofs.SetDOFName(varT, 0, "T");

	// set the active degrees of freedom for this solver
	const int dof_T = dofs.GetDOF("T");
	vector<int> dof;
	dof.push_back(dof_T);
	SetDOF(dof);
}

//-----------------------------------------------------------------------------
FEHeatSolver::~FEHeatSolver()
{
}

//-----------------------------------------------------------------------------
//! Do one-time initialization for data
bool FEHeatSolver::Init()
{
	// Call base class first
	if (FELinearSolver::Init() == false) return false;

	const int dof_T = m_fem.GetDOFS().GetDOF("T");
	if (dof_T == -1) { assert(false); return false; }

	// allocate data structures
	int neq = NumberOfEquations();
	m_Tp.assign(neq, 0);

	// set initial temperatures
	FEMesh& mesh = m_fem.GetMesh();
	for (int i=0; i<mesh.Nodes(); ++i)
	{
		FENode& node = mesh.Node(i);
		int nid = node.m_ID[dof_T];
		if (nid >= 0) m_Tp[nid] = node.get(dof_T);
	}

	// Identify the heat-transfer domains
	// TODO: I want this to be done automatically
	//       e.g. while the input file is being read
	FEAnalysis* pstep = m_fem.GetCurrentStep();
	pstep->ClearDomains();
	for (int nd=0; nd<mesh.Domains(); ++nd)
	{
		FEHeatSolidDomain* pd = dynamic_cast<FEHeatSolidDomain*>(&mesh.Domain(nd));
		if (pd) pstep->AddDomain(nd);
	}
	assert(pstep->Domains() != 0);

	return true;
}

//-----------------------------------------------------------------------------
//! update solution
void FEHeatSolver::Update(vector<double>& u)
{
	// call base class. 
	// This updates nodal variables and calls FEDomain::Update
	FELinearSolver::Update(u);

	// copy new temperatures to old temperature
	m_Tp = u;
}

//-----------------------------------------------------------------------------
//! Calculate the residual
void FEHeatSolver::ForceVector(FEGlobalVector& R)
{
	// Add nodal flux contributions
	NodalFluxes(R);

	// add surface fluxes
	SurfaceFluxes(R);

	// heat sources
	HeatSources(R);
}

//-----------------------------------------------------------------------------
//! Add nodal fluxes to residual
void FEHeatSolver::NodalFluxes(FEGlobalVector& R)
{
	// get the FE mesh
	FEMesh& mesh = m_fem.GetMesh();
	const int dof_T = m_fem.GetDOFS().GetDOF("T");
	if (dof_T == -1) { assert(false); return; }

	// loop over nodal loads
	int ncnf = m_fem.NodalLoads();
	for (int i=0; i<ncnf; ++i)
	{
		const FENodalLoad& fc = *m_fem.NodalLoad(i);
		int dof = fc.GetDOF();
		if (fc.IsActive() && (dof == dof_T))
		{
			int N = fc.Nodes();
			for (int j=0; j<N; ++j)
			{
				int nid	 = fc.NodeID(j);
				FENode& node = mesh.Node(nid);
				int n = node.m_ID[dof];
				if (n >= 0)
				{
					R[n] = fc.NodeValue(j);
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------
//! Calculate heat surface flux contribution to residual.
void FEHeatSolver::SurfaceFluxes(FEGlobalVector& R)
{
	// get the time information
	FETimeInfo tp = m_fem.GetTime();

	int nsl = m_fem.SurfaceLoads();
	for (int i=0; i<nsl; ++i)
	{
		// heat flux
		FEHeatFlux* phf = dynamic_cast<FEHeatFlux*>(m_fem.SurfaceLoad(i));
		if (phf && phf->IsActive()) phf->Residual(tp, R);

		// convective heat flux
		FEConvectiveHeatFlux* pchf = dynamic_cast<FEConvectiveHeatFlux*>(m_fem.SurfaceLoad(i));
		if (pchf && pchf->IsActive()) pchf->Residual(tp, R);
	}
}

//-----------------------------------------------------------------------------
//! Calculate the heat generation from heat sources
void FEHeatSolver::HeatSources(FEGlobalVector& R)
{
	int nbl = m_fem.BodyLoads();
	for (int i=0; i<nbl; ++i)
	{
		FEHeatSource* psh = dynamic_cast<FEHeatSource*>(m_fem.GetBodyLoad(i));
		if (psh) psh->Residual(R);
	}
}

//-----------------------------------------------------------------------------
//! Calculate the global stiffness matrix. This function simply calls 
//! HeatStiffnessMatrix() for each domain which will calculate the
//! contribution to the global stiffness matrix from each domain.
bool FEHeatSolver::StiffnessMatrix()
{
	FEAnalysis* pstep = m_fem.GetCurrentStep();

	// see if this is a dynamic problem
	bool bdyn = (pstep->m_nanalysis == FE_DYNAMIC);

	// get the time information
	FETimeInfo tp = m_fem.GetTime();

	// Add stiffness contribution from all domains
	for (int i=0; i<pstep->Domains(); ++i)
	{
		FEHeatDomain& bd = dynamic_cast<FEHeatDomain&>(*pstep->Domain(i));

		// add the conduction stiffness
		m_brhs = false;
		bd.ConductionMatrix(this);

		// for a dynamic analysis add the capacitance matrix
		if (bdyn) 
		{
			m_brhs = true;
			bd.CapacitanceMatrix(this, tp.timeIncrement);
		}
	}

	// add convective heat fluxes
	m_brhs = false;
	for (int i=0; i<m_fem.SurfaceLoads(); ++i)
	{
		FEConvectiveHeatFlux* pbc = dynamic_cast<FEConvectiveHeatFlux*>(m_fem.SurfaceLoad(i));
		if (pbc && pbc->IsActive()) pbc->StiffnessMatrix(tp, this);
	}

	return true;
}

//-----------------------------------------------------------------------------
//! Assembles the element stiffness matrix into the global stiffness matrix. 
//! This function is modified from the base class for including capacitance matrix
void FEHeatSolver::AssembleStiffness(vector<int>& en, vector<int>& lm, matrix& ke)
{
	// Call base class
	FELinearSolver::AssembleStiffness(en, lm, ke);

	// see if we need to modify the RHS
	// (This is needed for the capacitance matrix)
	if (m_brhs)
	{
		int ne = (int) lm.size();
		for (int j=0; j<ne; ++j)
		{
			if (lm[j] >= 0)
			{
				double q = 0;
				for (int k=0; k<ne; ++k)
				{
					if (lm[k] >= 0) q += ke[j][k]*m_Tp[lm[k]];
					else if (-lm[k]-2 >= 0) q += ke[j][k]*m_Tp[-lm[k]-2];
				}
				m_R[lm[j]] += q;
			}
		}
	}
}

//-----------------------------------------------------------------------------
//! Serializes data to the archive.
//! Still need to implement this.
//!
void FEHeatSolver::Serialize(DumpStream &ar)
{
	FELinearSolver::Serialize(ar);

	if (ar.IsSaving())
	{
		ar << m_Tp;
		ar << m_brhs;
	}
	else
	{
		ar >> m_Tp;
		ar >> m_brhs;
	}
}
