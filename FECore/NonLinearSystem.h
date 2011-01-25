#pragma once
#include <vector>
#include "SparseMatrix.h"

namespace FECore {

//-----------------------------------------------------------------------------
// The NonLinearSystem describes the interface for all non-linear systems.
// Each non-linear system has three responsibilities:
// 1. Evaluate its current state
// 2. Calculate its current jacobian
// 3. Update its current state given a solution increment
class NonLinearSystem
{
public:
	NonLinearSystem(void);
	virtual ~NonLinearSystem(void);

	// overide function to evaluate current state
	virtual void Evaluate(std::vector<double>& F) = 0;

	// override function to calculate jacobian matrix
	virtual void Jacobian(SparseMatrix& K) = 0;

	// override function to update state of system
	virtual bool Update(std::vector<double>& u) = 0;
};

}
