#pragma once
#include "FECore/FESolidDomain.h"
#include "FECore/FESolver.h"
#include "FEHeatTransferMaterial.h"

class FEModel;

//-----------------------------------------------------------------------------
class FEHeatDomain
{
public:
	FEHeatDomain(FEModel* pfem) : m_pfem(pfem) {}
	virtual ~FEHeatDomain(){}
	virtual void ConductionMatrix (FESolver* pnls) = 0;
	virtual void CapacitanceMatrix(FESolver* pnls, double dt) = 0;

	FEModel* GetFEModel() { return m_pfem; }

protected:
	FEModel* m_pfem;
};

//-----------------------------------------------------------------------------
//! domain class for 3D heat elements
class FEHeatSolidDomain : public FESolidDomain, public FEHeatDomain
{
public:
	//! constructor
	FEHeatSolidDomain(FEModel* pfem);

	//! activate
	void Activate();

	//! Unpack solid element data
	void UnpackLM(FEElement& el, vector<int>& lm);

	//! get the material (overridden from FEDomain)
	FEMaterial* GetMaterial() { return m_pMat; }

	//! set the material
	void SetMaterial(FEMaterial* pmat);

public: // overloaded from FEHeatDomain

	//! Calculate the conduction stiffness 
	void ConductionMatrix(FESolver* psolver);

	//! Calculate capacitance stiffness matrix
	void CapacitanceMatrix(FESolver* psolver, double dt);

protected:
	//! calculate the conductive element stiffness matrix
	void ElementConduction(FESolidElement& el, matrix& ke);

	//! calculate the capacitance element stiffness matrix
	void ElementCapacitance(FESolidElement& el, matrix& ke, double dt);

protected:
	FEHeatTransferMaterial*	m_pMat;
};
