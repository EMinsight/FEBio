#include "stdafx.h"
#include "FEScanOptimizeMethod.h"
#include "FEOptimizeData.h"
#include "FECore/log.h"

BEGIN_PARAMETER_LIST(FEScanOptimizeMethod, FEOptimizeMethod)
	ADD_PARAMETERV(m_inc, FE_PARAM_DOUBLE, 32, "inc");
END_PARAMETER_LIST();

//-----------------------------------------------------------------------------
// FEScanOptimizeMethod
//-----------------------------------------------------------------------------

bool fecb(FEModel* pfem, unsigned int nwhen, void* pd);

bool FEScanOptimizeMethod::Solve(FEOptimizeData* pOpt)
{
	m_pOpt = pOpt;
	FEOptimizeData& opt = *pOpt;

	// set the variables
	int ma = opt.InputParameters();
	vector<double> a(ma);
	for (int i=0; i<ma; ++i)
	{
		FEInputParameter* var = opt.GetInputParameter(i);
		a[i] = var->MinValue();
	}

	// set the FEM callback function
	FEModel& fem = opt.GetFEM();
	fem.AddCallback(fecb, CB_MAJOR_ITERS | CB_INIT, &opt);

	// set the data
	FEObjectiveFunction& obj = opt.GetObjective();
	FELoadCurve& lc = obj.GetLoadCurve(obj.m_nlc);
	int ndata = lc.Points();
	vector<double> x(ndata), y0(ndata), y(ndata);
	for (int i=0; i<ndata; ++i) 
	{
		x[i] = lc.LoadPoint(i).time;
		y0[i] = lc.LoadPoint(i).value;
	}

	opt.m_niter = 0;

	bool bdone = false;
	double fmin = 0.0;
	vector<double> amin;
	do
	{
		// solve the problem
		if (FESolve(x, a, y) == false) return false;

		// calculate objective function
		double fobj = 0.0;
		for (int i=0; i<ndata; ++i)
		{
			double dy = y[i] - y0[i];
			fobj += dy*dy;
		}
		felog.printf("Objective value: %lg\n", fobj);

		if ((fmin == 0.0) || (fobj < fmin))
		{
			fmin = fobj;
			amin = a;
		}

		// adjust indices
		for (int i=0; i<ma; ++i)
		{
			FEInputParameter& vi = *opt.GetInputParameter(i);
			if (a[i] >= vi.MaxValue())
			{
				if (i<ma-1)
				{
					a[i+1] += m_inc[i+1];
					a[i  ] = vi.MinValue();
				}
				else bdone = true;
			}
			else 
			{
				a[i] += m_inc[i];
				break;
			}
		}
	}
	while (!bdone);

	felog.printf("\n-------------------------------------------------------\n");
	for (int i=0; i<ma; ++i) 
	{
		FEInputParameter& var = *opt.GetInputParameter(i);
		string name = var.GetName();
		felog.printf("%-15s = %lg\n", name.c_str(), amin[i]);
	}
	felog.printf("Objective value: %lg\n", fmin);

	return true;
}

//-----------------------------------------------------------------------------
bool FEScanOptimizeMethod::FESolve(vector<double> &x, vector<double> &a, vector<double> &y)
{
	// get the optimization data
	FEOptimizeData& opt = *m_pOpt;

	// increase iterator counter
	opt.m_niter++;

	// get the FEM data
	FEModel& fem = opt.GetFEM();

	// reset reaction force data
	FEObjectiveFunction& obj = opt.GetObjective();
	FELoadCurve& lc = obj.ReactionLoad();
	lc.Clear();

	// set the input parameters
	int nvar = opt.InputParameters();
	for (int i=0; i<nvar; ++i)
	{
		FEInputParameter& var = *opt.GetInputParameter(i);
		var.SetValue(a[i]);
	}

	// reset the FEM data
	fem.Reset();

	felog.SetMode(Logfile::LOG_FILE_AND_SCREEN);
	felog.printf("\n----- Iteration: %d -----\n", opt.m_niter);
	for (int i=0; i<nvar; ++i) 
	{
		FEInputParameter& var = *opt.GetInputParameter(i);
		string name = var.GetName();
		felog.printf("%-15s = %lg\n", name.c_str(), var.GetValue());
	}

	// solve the FE problem
	felog.SetMode(Logfile::LOG_NEVER);

	bool bret = m_pOpt->RunTask();

	felog.SetMode(Logfile::LOG_FILE_AND_SCREEN);
	if (bret)
	{
		FEObjectiveFunction& obj = opt.GetObjective();
		FELoadCurve& rlc = obj.ReactionLoad();
		int ndata = (int)x.size();
		if (m_print_level == PRINT_VERBOSE) felog.printf("               CURRENT        REQUIRED      DIFFERENCE\n");
		for (int i=0; i<ndata; ++i) 
		{
			y[i] = rlc.Value(x[i]);
//			if (m_print_level == PRINT_VERBOSE) felog.printf("%5d: %15.10lg %15.10lg %15lg\n", i+1, y[i], m_y0[i], fabs(y[i] - m_y0[i]));
		}
	}

	return bret;
}

