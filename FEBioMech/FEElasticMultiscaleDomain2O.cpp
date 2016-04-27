#include "stdafx.h"
#include "FEElasticMultiscaleDomain2O.h"
#include "FEMicroMaterial2O.h"
#include "FECore/mat3d.h"
#include "FECore/tens6d.h"

//-----------------------------------------------------------------------------
// helper function for comparing two facets
bool compare_facets(int* na, int* nb, int nodes)
{
	switch (nodes)
	{
	case 3:
	case 6:
	case 7:
		if ((na[0]!=nb[0])&&(na[0]!=nb[1])&&(na[0]!=nb[2])) return false;
		if ((na[1]!=nb[0])&&(na[1]!=nb[1])&&(na[1]!=nb[2])) return false;
		if ((na[2]!=nb[0])&&(na[2]!=nb[1])&&(na[2]!=nb[2])) return false;
		break;
	default:
		return false;
	}
	return true;
}

//-----------------------------------------------------------------------------
// helper function for mapping surface facet coordinates to element facet coordinates
vec2d map_facet_to_facet(int* na, int* nb, int nodes, double r, double s)
{
	switch (nodes)
	{
	case 3:
	case 6:
	case 7:
		if ((na[0] == nb[0])&&(na[1] == nb[1])&&(na[2] == nb[2])) return vec2d(  r  ,     s);
		if ((na[0] == nb[2])&&(na[1] == nb[0])&&(na[2] == nb[1])) return vec2d(    s, 1-r-s);
		if ((na[0] == nb[1])&&(na[1] == nb[2])&&(na[2] == nb[0])) return vec2d(1-r-s,   r  );
		if ((na[0] == nb[0])&&(na[1] == nb[2])&&(na[2] == nb[1])) return vec2d(    s,   r  );
		if ((na[0] == nb[2])&&(na[1] == nb[1])&&(na[2] == nb[0])) return vec2d(  r  , 1-r-s);
		if ((na[0] == nb[1])&&(na[1] == nb[0])&&(na[2] == nb[2])) return vec2d(1-r-s,     s);
		break;
	}

	// we shouldn't get here
	assert(false);
	return vec2d(0,0);
}

//-----------------------------------------------------------------------------
// helper function that maps natural coordinates from facets to elements
vec3d map_facet_to_solid(FEMesh& mesh, FESurfaceElement& face, FESolidElement& el, double r, double s)
{
	int fn[FEElement::MAX_NODES];
	int nfaces = mesh.Faces(el);
	for (int i=0; i<nfaces; ++i)
	{
		mesh.GetFace(el, i, fn);
		if (compare_facets(&face.m_node[0], fn, face.Nodes()))
		{
			// map the facet coordinates to the element's facet coordinates
			// (faces can be rotated or inverted w.r.t. the element's face)
			vec2d b = map_facet_to_facet(&face.m_node[0], fn, face.Nodes(), r, s);
			double h1 = b.x(), h2 = b.y();

			// convert facet coordinates to volume coordinates
			// TODO: this is hard coded for tets! Need to generalize for hexes
			double g1, g2, g3;
			switch (i)
			{
			case 0: g1 = h1; g2 = h2; g3 = 0.; break;
			case 1: g1 = h1; g2 = 0.; g3 = h2; break;
			case 2: g1 = 0.; g2 = h1; g3 = h2; break;
			case 3: g1 = 1.0-h1-h2; g2 = h1; g3 = h2; break;
			}

			return vec3d(g1, g2, g3);
		}
	}

	// we shouldn't get here
	assert(false);
	return vec3d(0,0,0);
}

//-----------------------------------------------------------------------------
FEElasticMultiscaleDomain2O::FEInternalSurface2O::FEInternalSurface2O()
{
}

bool FEElasticMultiscaleDomain2O::FEInternalSurface2O::Initialize(FEElasticMultiscaleDomain2O* dom)
{
	// get the material and make sure it is correct
	FEMicroMaterial2O* pmat = dynamic_cast<FEMicroMaterial2O*>(dom->GetMaterial());
	if (pmat == 0) return false;

	// build the inside surface
	FEMesh& mesh = *dom->GetMesh();
	m_ps = mesh.ElementBoundarySurface(false, true);
	if (m_ps == 0) return false;

	// allocate data
	int NF = m_ps->Elements();
	int nnf = 0;
	for (int i=0; i<NF; ++i)
	{
		FESurfaceElement& el = m_ps->Element(i);
		nnf += el.GaussPoints();
	}
	m_data.resize(nnf);

	// allocate material point data
	nnf = 0;
	for (int i=0; i<NF; ++i)
	{
		FESurfaceElement& face = m_ps->Element(i);
		int nint = face.GaussPoints();
		for (int n=0; n<nint; ++n, ++nnf)
		{
			m_data[nnf].m_pt[0] = pmat->CreateMaterialPointData();
			m_data[nnf].m_pt[1] = pmat->CreateMaterialPointData();

			m_data[nnf].m_pt[0]->Init(true);
			m_data[nnf].m_pt[1]->Init(true);

			// iso-parametric coordinates in surface element
			double h1 = face.gr(n);
			double h2 = face.gs(n);

			for (int k=0; k<2; ++k)
			{
				// get the adjacent solid element
				FESolidElement& ek = dom->Element(face.m_elem[k]);

				// map the iso-parametric coordinates from the facet to the solid element
				m_data[nnf].ksi[k] = map_facet_to_solid(mesh, face, ek, h1, h2);
			}
		}
	}

	return true;
}

//=============================================================================

//-----------------------------------------------------------------------------
//! constructor
FEElasticMultiscaleDomain2O::FEElasticMultiscaleDomain2O(FEModel* pfem) : FEElasticSolidDomain(pfem)
{
}

//-----------------------------------------------------------------------------
//! Initialize element data
bool FEElasticMultiscaleDomain2O::Initialize(FEModel& fem)
{
	if (FEElasticSolidDomain::Initialize(fem) == false) return false;

	const int NE = FEElement::MAX_NODES;
	vec3d x0[NE], xt[NE], r0, rt;
	FEMesh& m = *GetMesh();
		
	FEMicroMaterial2O* pmat = dynamic_cast<FEMicroMaterial2O*>(m_pMat);
	FEModel& rve = pmat->m_mrve;
			
	for (size_t i=0; i<m_Elem.size(); ++i)
	{
		FESolidElement& el = m_Elem[i];
		int neln = el.Nodes();
		for (int i=0; i<neln; ++i)
		{
			x0[i] = m.Node(el.m_node[i]).m_r0;
			xt[i] = m.Node(el.m_node[i]).m_rt;
		}

		int n = el.GaussPoints();
		for (int j=0; j<n; ++j) 
		{
			r0 = el.Evaluate(x0, j);
			rt = el.Evaluate(xt, j);

			FEMaterialPoint& mp = *el.GetMaterialPoint(j);
			FEElasticMaterialPoint& pt = *mp.ExtractData<FEElasticMaterialPoint>();
			FEMicroMaterialPoint2O& mmpt2O = *mp.ExtractData<FEMicroMaterialPoint2O>();
			pt.m_r0 = r0;
			pt.m_rt = rt;

			pt.m_J = defgrad(el, pt.m_F, j);
			defhess(el, j, mmpt2O.m_G);

			mmpt2O.m_F_prev = pt.m_F;	//-> I don't think this does anything since Initialize is only called once
			mmpt2O.m_G_prev = mmpt2O.m_G;
			mmpt2O.m_rve.CopyFrom(rve);
			mmpt2O.m_rve.Init();
		}
	}

	// initialize the internal surface data
	if (m_surf.Initialize(this) == false) return false;

	// initialize surface RVEs
	int nnf = 0;
	int NF = m_surf.Elements();
	for (int i=0; i<NF; ++i)
	{
		FESurfaceElement& face = m_surf.Element(i);
		int nint = face.GaussPoints();
		for (int n=0; n<nint; ++n, ++nnf)
		{
			for (int k=0; k<2; ++k)
			{
				FEMaterialPoint& mp = *m_surf.GetData(i).m_pt[0];
				FEMicroMaterialPoint2O& mmpt2O = *mp.ExtractData<FEMicroMaterialPoint2O>();

				mmpt2O.m_rve.CopyFrom(rve);
				mmpt2O.m_rve.Init();
			}
		}
	}

	// create the probes
	int NP = pmat->Probes();
	for (int i=0; i<NP; ++i)
	{
		FEMicroProbe& p = pmat->Probe(i);
		FEElement* pel = FindElementFromID(p.m_neid);
		if (pel)
		{
			int nint = pel->GaussPoints();
			int ngp = p.m_ngp - 1;
			if ((ngp>=0)&&(ngp<nint))
			{
				FEMaterialPoint& mp = *pel->GetMaterialPoint(ngp);
				FEMicroMaterialPoint2O& mmpt = *mp.ExtractData<FEMicroMaterialPoint2O>();
				FERVEProbe* prve = new FERVEProbe(fem, mmpt.m_rve, p.m_szfile);
			}
			else return fecore_error("Invalid gausspt number for micro-probe %d in material %d (%s)", i+1, m_pMat->GetID(), m_pMat->GetName());
		}
		else return fecore_error("Invalid Element ID for micro probe %d in material %d (%s)", i+1, m_pMat->GetID(), m_pMat->GetName());
	}

	return true;
}

//-----------------------------------------------------------------------------
void FEElasticMultiscaleDomain2O::InitElements()
{
	FEElasticSolidDomain::InitElements();

	int NF = m_surf.Elements(), nd = 0;
	for (int i=0; i<NF; ++i)
	{
		FESurfaceElement& el = m_surf.Element(i);
		int nint = el.GaussPoints();
		for (int n=0; n<nint; ++n, ++nd)
		{
			FEInternalSurface2O::Data& data = m_surf.GetData(nd);
			data.m_pt[0]->Init(false);
			data.m_pt[1]->Init(false);
		}
	}
}

//-----------------------------------------------------------------------------
void FEElasticMultiscaleDomain2O::InternalForces(FEGlobalVector& R)
{
	// call base class first
	FEElasticSolidDomain::InternalForces(R);

	// add the discrete-Galerkin contribution
	InternalForcesDG1(R);
	InternalForcesDG2(R);
}

//-----------------------------------------------------------------------------
//! Evaluate contribution of discrete-Galerkin enforcement of stress flux.
void FEElasticMultiscaleDomain2O::InternalForcesDG1(FEGlobalVector& R)
{
	FEMesh& mesh = *GetMesh();

	mat3d Ji;
	double Gr[FEElement::MAX_NODES];
	double Gs[FEElement::MAX_NODES];
	double Gt[FEElement::MAX_NODES];
	vec3d r0[FEElement::MAX_NODES];

	vector<double> fe;
	vector<int> lm;

	// loop over all internal surfaces elements
	int nd = 0;
	int NF = m_surf.Elements();
	for (int i=0; i<NF; ++i)
	{
		// get the next surface
		FESurfaceElement& face = m_surf.Element(i);
		int nfn = face.Nodes();
		int nint = face.GaussPoints();

		// get the reference nodal coordinates
		for (int j=0; j<nfn; ++j)
		{
			FENode& node = mesh.Node(face.m_node[j]);
			r0[j] = node.m_r0;
		}

		// loop over both sides
		for (int m=0; m<2; ++m)
		{
			// evaluate the spatial gradient of shape functions
			FESolidElement& el = Element(face.m_elem[m]);
			int neln = el.Nodes();

			// sign
			double sgn = (m==0 ? 1.0 : -1.0);

			// allocate force vector
			int ndof = neln*3;
			fe.resize(ndof, 0.0);

			// loop over all the integration points
			double* gw = face.GaussWeights();
			for (int n=0; n<nint; ++n)
			{
				// get facet data
				FEInternalSurface2O::Data& data = m_surf.GetData(nd + n);

				// average stress across interface
				tens3drs& Qavg = data.Qavg;

				// calculate jacobian and surface normal
				double* Hr = face.Gr(n);
				double* Hs = face.Gs(n);
				vec3d r1(0,0,0), r2(0,0,0);
				for (int k=0; k<nfn; ++k)
				{
					r1 += r0[k]*Hr[k];
					r2 += r0[k]*Hs[k];
				}	
				vec3d nu = r1^r2;
				double J = nu.unit();

				// evaluate element Jacobian and shape function derivatives
				// at this integration point
				vec3d& ksi = data.ksi[m];
				invjac0(el, ksi.x, ksi.y, ksi.z, Ji);
				el.shape_deriv(Gr, Gs, Gt, ksi.x, ksi.y, ksi.z);

				// loop over element nodes
				for (int j=0; j<neln; ++j)
				{
					// calculate global gradient of shape functions
					// note that we need the transposed of Ji, not Ji itself !
					double G[3];
					G[0] = Ji[0][0]*Gr[j]+Ji[1][0]*Gs[j]+Ji[2][0]*Gt[j];
					G[1] = Ji[0][1]*Gr[j]+Ji[1][1]*Gs[j]+Ji[2][1]*Gt[j];
					G[2] = Ji[0][2]*Gr[j]+Ji[1][2]*Gs[j]+Ji[2][2]*Gt[j];

					// put it all together
					double f[3] = {0}, Nu[3] = {nu.x, nu.y, nu.z};
					for (int k=0; k<3; ++k)
						for (int l=0; l<3; ++l)
						{
							f[0] += Qavg(0,k,l)*G[k]*Nu[l];
							f[1] += Qavg(1,k,l)*G[k]*Nu[l];
							f[2] += Qavg(2,k,l)*G[k]*Nu[l];
						}

					// the negative sign is because we need to subtract the internal forces
					// from the residual
					fe[3*j  ] -= f[0]*gw[n]*J*sgn;
					fe[3*j+1] -= f[1]*gw[n]*J*sgn;
					fe[3*j+2] -= f[2]*gw[n]*J*sgn;
				}
			}

			// unpack the LM values
			UnpackLM(el, lm);

			// assemble 
			R.Assemble(el.m_node, lm, fe);
		}

		// don't forgot to increment data counter
		nd += nint;
	}
}

//-----------------------------------------------------------------------------
//! Evaluate contribution of discrete-Galerkin enforcement of stress flux.
void FEElasticMultiscaleDomain2O::InternalForcesDG2(FEGlobalVector& R)
{
	FEMesh& mesh = *GetMesh();

	mat3d Ji;
	double Gr[FEElement::MAX_NODES];
	double Gs[FEElement::MAX_NODES];
	double Gt[FEElement::MAX_NODES];
	vec3d r0[FEElement::MAX_NODES];

	vector<double> fe;
	vector<int> lm;

	// loop over all internal surfaces elements
	int nd = 0;
	int NF = m_surf.Elements();
	for (int i=0; i<NF; ++i)
	{
		// get the next surface
		FESurfaceElement& face = m_surf.Element(i);
		int nfn = face.Nodes();
		int nint = face.GaussPoints();

		// get the reference nodal coordinates
		for (int j=0; j<nfn; ++j)
		{
			FENode& node = mesh.Node(face.m_node[j]);
			r0[j] = node.m_r0;
		}

		// loop over both sides
		for (int m=0; m<2; ++m)
		{
			// evaluate the spatial gradient of shape functions
			FESolidElement& el = Element(face.m_elem[m]);
			int neln = el.Nodes();

			// sign
			double sgn = (m==0 ? 1.0 : -1.0);

			// allocate force vector
			int ndof = neln*3;
			fe.resize(ndof, 0.0);

			// loop over all the integration points
			double* gw = face.GaussWeights();
			for (int n=0; n<nint; ++n)
			{
				// get facet data
				FEInternalSurface2O::Data& data = m_surf.GetData(nd + n);

				// average stiffness across interface
				tens6ds& J0avg = data.J0avg;

				// displacement gradient jump
				const mat3d& Du = data.DgradU;

				// calculate jacobian and surface normal
				double* Hr = face.Gr(n);
				double* Hs = face.Gs(n);
				vec3d r1(0,0,0), r2(0,0,0);
				for (int k=0; k<nfn; ++k)
				{
					r1 += r0[k]*Hr[k];
					r2 += r0[k]*Hs[k];
				}	
				vec3d nu = r1^r2;
				double J = nu.unit();

				// evaluate element Jacobian and shape function derivatives
				// at this integration point
				vec3d& ksi = data.ksi[m];
				invjac0(el, ksi.x, ksi.y, ksi.z, Ji);
				el.shape_deriv(Gr, Gs, Gt, ksi.x, ksi.y, ksi.z);

				// loop over element nodes
				for (int j=0; j<neln; ++j)
				{
					// calculate global gradient of shape functions
					// note that we need the transposed of Ji, not Ji itself !
					double G[3];
					G[0] = Ji[0][0]*Gr[j]+Ji[1][0]*Gs[j]+Ji[2][0]*Gt[j];
					G[1] = Ji[0][1]*Gr[j]+Ji[1][1]*Gs[j]+Ji[2][1]*Gt[j];
					G[2] = Ji[0][2]*Gr[j]+Ji[1][2]*Gs[j]+Ji[2][2]*Gt[j];

					// put it all together
					double f[3] = {0}, Nu[3] = {nu.x, nu.y, nu.z};
					for (int k=0; k<3; ++k)
						for (int l=0; l<3; ++l)
							for (int p=0; p<3; ++p)
								for (int q=0; q<3; ++q)
									for (int r=0; r<3; ++r)
									{
										f[0] += Du(k,l)*Nu[p]*J0avg(k,l,p,0,q,r)*G[q]*Nu[r];
										f[1] += Du(k,l)*Nu[p]*J0avg(k,l,p,1,q,r)*G[q]*Nu[r];
										f[2] += Du(k,l)*Nu[p]*J0avg(k,l,p,2,q,r)*G[q]*Nu[r];
									}

					// the negative sign is because we need to subtract the internal forces
					// from the residual
					fe[3*j  ] -= f[0]*gw[n]*J*sgn;
					fe[3*j+1] -= f[1]*gw[n]*J*sgn;
					fe[3*j+2] -= f[2]*gw[n]*J*sgn;
				}
			}

			// unpack the LM values
			UnpackLM(el, lm);

			// assemble 
			R.Assemble(el.m_node, lm, fe);
		}

		// don't forgot to increment data counter
		nd += nint;
	}
}

//-----------------------------------------------------------------------------
//! calculates the internal equivalent nodal forces for solid elements
void FEElasticMultiscaleDomain2O::ElementInternalForce(FESolidElement& el, vector<double>& fe)
{
	// contribution from stress
	ElementInternalForce_PF(el, fe);

	// contriubtion from higher-order stress
	ElementInternalForce_QG(el, fe);
}

//-----------------------------------------------------------------------------
void FEElasticMultiscaleDomain2O::ElementInternalForce_PF(FESolidElement& el, vector<double>& fe)
{
	double Ji[3][3];
	int nint = el.GaussPoints();
	int neln = el.Nodes();
	double*	gw = el.GaussWeights();
	
	// repeat for all integration points
	for (int n=0; n<nint; ++n)
	{
		FEMaterialPoint& mp = *el.GetMaterialPoint(n);
		FEElasticMaterialPoint& pt = *(mp.ExtractData<FEElasticMaterialPoint>());
		FEMicroMaterialPoint2O& mmpt2O = *(mp.ExtractData<FEMicroMaterialPoint2O>());

		// calculate the jacobian and its derivative
		// (and multiply by integration weight)
		double detJt = invjact(el, Ji, n)*gw[n];

		// get the stress vector for this integration point
		mat3ds& s = pt.m_s;

		double* Gr = el.Gr(n);
		double* Gs = el.Gs(n);
		double* Gt = el.Gt(n);

		// --- first-order contribution ---
		for (int i=0; i<neln; ++i)
		{
			// calculate global gradient of shape functions
			// note that we need the transposed of Ji, not Ji itself !
			double Gx = Ji[0][0]*Gr[i]+Ji[1][0]*Gs[i]+Ji[2][0]*Gt[i];
			double Gy = Ji[0][1]*Gr[i]+Ji[1][1]*Gs[i]+Ji[2][1]*Gt[i];
			double Gz = Ji[0][2]*Gr[i]+Ji[1][2]*Gs[i]+Ji[2][2]*Gt[i];

			// calculate internal force
			// the '-' sign is so that the internal forces get subtracted
			// from the global residual vector
			fe[3*i  ] -=  (Gx*s.xx() + Gy*s.xy() + Gz*s.xz())*detJt;
			fe[3*i+1] -=  (Gx*s.xy() + Gy*s.yy() + Gz*s.yz())*detJt;
			fe[3*i+2] -=  (Gx*s.xz() + Gy*s.yz() + Gz*s.zz())*detJt;
		}
	}
}

//-----------------------------------------------------------------------------
void FEElasticMultiscaleDomain2O::ElementInternalForce_QG(FESolidElement& el, vector<double>& fe)
{
	int nint = el.GaussPoints();
	int neln = el.Nodes();
	double*	gw = el.GaussWeights();

	// inverse of Jacobian matrix
	double Ji[3][3];

	// get the nodal positions
	vec3d X[FEElement::MAX_NODES];
	FEMesh& mesh = *GetMesh();
	for (int i=0; i<neln; ++i) X[i] = mesh.Node(el.m_node[i]).m_r0;

	// repeat for all integration points
	for (int n=0; n<nint; ++n)
	{
		FEMaterialPoint& mp = *el.GetMaterialPoint(n);
		FEElasticMaterialPoint& pt = *(mp.ExtractData<FEElasticMaterialPoint>());
		FEMicroMaterialPoint2O& mmpt2O = *(mp.ExtractData<FEMicroMaterialPoint2O>());

		// get the higher-order stress
		tens3drs& Q = mmpt2O.m_Q;

		// we'll evaluate this term in the material frame
		// so we need the Jacobian with respect to the reference configuration
		double J0 = invjac0(el, Ji, n);

		// shape function derivatives
		double* Gr = el.Gr(n);
		double* Gs = el.Gs(n);
		double* Gt = el.Gt(n);

		double *Grrn = el.Grr(n); double *Grsn = el.Grs(n); double *Grtn = el.Grt(n);
		double *Gsrn = el.Gsr(n); double *Gssn = el.Gss(n); double *Gstn = el.Gst(n);
		double *Gtrn = el.Gtr(n); double *Gtsn = el.Gts(n); double *Gttn = el.Gtt(n);

		// calculate K = dJ/dr
		double K[3][3][3] = {0};
		for (int a=0; a<neln; ++a)
		{
			// second derivatives of shape functions
			double G2[3][3];
			G2[0][0] = Grrn[a]; G2[0][1] = Grsn[a]; G2[0][2] = Grtn[a];
			G2[1][0] = Gsrn[a]; G2[1][1] = Gssn[a]; G2[1][2] = Gstn[a];
			G2[2][0] = Gtrn[a]; G2[2][1] = Gtsn[a]; G2[2][2] = Gttn[a];

			for (int j=0; j<3; ++j)
				for (int k=0; k<3; ++k)
				{
					K[0][j][k] += G2[j][k]*X[a].x;
					K[1][j][k] += G2[j][k]*X[a].y;
					K[2][j][k] += G2[j][k]*X[a].z;
				}
		}

		// calculate A = -J^-1*dJ/drJ^-1
		double A[3][3][3] = {0};
		for (int i=0; i<3; ++i)
			for (int j=0; j<3; ++j)
			{
				for (int p=0; p<3; ++p)
					for (int q=0; q<3; ++q)
					{
						A[i][j][0] -= Ji[j][p]*K[p][q][0]*Ji[q][i];
						A[i][j][1] -= Ji[j][p]*K[p][q][1]*Ji[q][i];
						A[i][j][2] -= Ji[j][p]*K[p][q][2]*Ji[q][i];
					}
			}

		// loop over nodes
		for (int a=0; a<neln; ++a)
		{
			// first derivative of shape functions
			double G1[3];
			G1[0] = Gr[a];
			G1[1] = Gs[a];
			G1[2] = Gt[a];

			// second derivatives of shape functions
			double G2[3][3];
			G2[0][0] = Grrn[a]; G2[0][1] = Grsn[a]; G2[0][2] = Grtn[a];
			G2[1][0] = Gsrn[a]; G2[1][1] = Gssn[a]; G2[1][2] = Gstn[a];
			G2[2][0] = Gtrn[a]; G2[2][1] = Gtsn[a]; G2[2][2] = Gttn[a];

			// calculate dB/dr
			double D[3][3] = {0};
			for (int i=0; i<3; ++i)
				for (int k=0; k<3; ++k)
				{
					for (int j=0; j<3; ++j) D[i][k] += A[i][j][k]*G1[j] + Ji[j][i]*G2[j][k];
				}


			// calculate global gradient of shape functions
			double H[3][3] = {0};
			for (int i=0; i<3; ++i)
				for (int j=0; j<3; ++j)
				{
					H[i][j] += D[i][0]*Ji[0][j] + D[i][1]*Ji[1][j] + D[i][2]*Ji[2][j];
				}

			// calculate internal force
			// the '-' sign is so that the internal forces get subtracted
			// from the global residual vector
			for (int j=0; j<3; ++j)
				for (int k=0; k<3; ++k)
				{
					fe[3*a  ] -=  (Q(0,j,k)*H[j][k])*J0*gw[n];
					fe[3*a+1] -=  (Q(1,j,k)*H[j][k])*J0*gw[n];
					fe[3*a+2] -=  (Q(2,j,k)*H[j][k])*J0*gw[n];
				}
		}
	}
}

//-----------------------------------------------------------------------------
void FEElasticMultiscaleDomain2O::Update()
{
	// call base class first
	// (this will call FEElasticMultiscaleDomain2O::UpdateElementStress)
	FEElasticSolidDomain::Update();

	// update internal surfaces
	UpdateInternalSurfaceStresses();

	// update the kinematic variables
	UpdateKinematics();
}

//-----------------------------------------------------------------------------
// Update some kinematical quantities needed for evaluating the discrete-Galerkin terms
void FEElasticMultiscaleDomain2O::UpdateKinematics()
{
	FEMesh& mesh = *GetMesh();

	// nodal displacements
	vec3d ut[FEElement::MAX_NODES];

	// shape function derivatives
	double Gr[FEElement::MAX_NODES];
	double Gs[FEElement::MAX_NODES];
	double Gt[FEElement::MAX_NODES];

	// loop over all facets
	int nd = 0;
	int NF = m_surf.Elements();
	for (int i=0; i<NF; ++i)
	{
		// get the next facet
		FESurfaceElement& face = m_surf.Element(i);
		int nfn  = face.Nodes();
		int nint = face.GaussPoints();

		// calculate the displacement gradient jump across this facet
		for (int m=0; m<2; ++m)
		{
			// evaluate the spatial gradient of shape functions
			FESolidElement& el = Element(face.m_elem[m]);
			int neln = el.Nodes();

			// get the nodal displacements
			for (int j=0; j<neln; ++j)
			{
				FENode& node = mesh.Node(el.m_node[j]);
				ut[j] = node.m_rt - node.m_r0;
			}

			// loop over all integration points
			for (int n=0; n<nint; ++n)
			{
				// get the integration point data
				FEInternalSurface2O::Data& data = m_surf.GetData(nd + n);

				// evaluate element Jacobian and shape function derivatives
				// at this integration point
				vec3d& ksi = data.ksi[m];
				mat3d Ji;
				invjac0(el, ksi.x, ksi.y, ksi.z, Ji);
				el.shape_deriv(Gr, Gs, Gt, ksi.x, ksi.y, ksi.z);

				mat3d Gu; Gu.zero();
				for (int j=0; j<neln; ++j)
				{
					// calculate global gradient of shape functions
					// note that we need the transposed of Ji, not Ji itself !
					double Gx = Ji[0][0]*Gr[j]+Ji[1][0]*Gs[j]+Ji[2][0]*Gt[j];
					double Gy = Ji[0][1]*Gr[j]+Ji[1][1]*Gs[j]+Ji[2][1]*Gt[j];
					double Gz = Ji[0][2]*Gr[j]+Ji[1][2]*Gs[j]+Ji[2][2]*Gt[j];

					Gu[0][0] += ut[j].x*Gx; Gu[0][1] += ut[j].x*Gy; Gu[0][2] += ut[j].x*Gz;
					Gu[1][0] += ut[j].y*Gx; Gu[1][1] += ut[j].y*Gy; Gu[1][2] += ut[j].y*Gz;
					Gu[2][0] += ut[j].z*Gx; Gu[2][1] += ut[j].z*Gy; Gu[2][2] += ut[j].z*Gz;
				}

				if (m == 0) data.DgradU = Gu;
				else data.DgradU -= Gu;
			}
		}

		// don't forget to increment data counter
		nd += nint;
	}
}

//-----------------------------------------------------------------------------
// This function evaluates the stresses at either side of the internal surface
// facets.
void FEElasticMultiscaleDomain2O::UpdateInternalSurfaceStresses()
{
	FEMesh& mesh = *GetMesh();
	// calculate the material
	FEMicroMaterial2O* pmat = dynamic_cast<FEMicroMaterial2O*>(m_pMat);

	// loop over all the internal surfaces
	int NF = m_surf.Elements(), nd = 0;
	for (int i=0; i<NF; ++i)
	{
		FESurfaceElement& face = m_surf.Element(i);
		int nint = face.GaussPoints();
		for (int n=0; n<nint; ++n, ++nd)
		{
			FEInternalSurface2O::Data& data =  m_surf.GetData(nd);
			data.Qavg.zero();
			data.J0avg.zero();

			// get the deformation gradient and determinant
			for (int k=0; k<2; ++k)
			{
				FEMaterialPoint& mp = *data.m_pt[k];
				FEElasticMaterialPoint& pt = *mp.ExtractData<FEElasticMaterialPoint>();
				FEMicroMaterialPoint2O& pt2O = *mp.ExtractData<FEMicroMaterialPoint2O>();

				vec3d& ksi = data.ksi[k];

				// TODO: Is face.m_elem is a local index?
				FESolidElement& ek = Element(face.m_elem[k]);

				// evaluate deformation gradient, Jacobian and Hessian for this element
				pt.m_J = defgrad(ek, pt.m_F, ksi.x, ksi.y, ksi.z);
				defhess(ek, ksi.x, ksi.y, ksi.z, pt2O.m_G);

				// evaluate stresses at this integration point
				pmat->Stress2O(mp);

				data.Qavg += pt2O.m_Q*0.5;
				data.J0avg += pt2O.m_Ea;		// TODO: I only need to evaluate this on the first iteration
			}
		}
	}
}

//-----------------------------------------------------------------------------
//! Update element state data (mostly stresses, but some other stuff as well)
//! \todo Remove the remodeling solid stuff
void FEElasticMultiscaleDomain2O::UpdateElementStress(int iel, double dt)
{
	// get the solid element
	FESolidElement& el = m_Elem[iel];
	
	// get the number of integration points
	int nint = el.GaussPoints();

	// number of nodes
	int neln = el.Nodes();

	// nodal coordinates
	vec3d r0[FEElement::MAX_NODES];
	vec3d rt[FEElement::MAX_NODES];
	for (int j=0; j<neln; ++j)
	{
		r0[j] = m_pMesh->Node(el.m_node[j]).m_r0;
		rt[j] = m_pMesh->Node(el.m_node[j]).m_rt;
	}

	// get the integration weights
	double* gw = el.GaussWeights();

	// calculate the stress at this material point
	FEMicroMaterial2O* pmat = dynamic_cast<FEMicroMaterial2O*>(m_pMat);

	// loop over the integration points and calculate
	// the stress at the integration point
	for (int n=0; n<nint; ++n)
	{
		FEMaterialPoint& mp = *el.GetMaterialPoint(n);
		FEElasticMaterialPoint& pt = *(mp.ExtractData<FEElasticMaterialPoint>());
		FEMicroMaterialPoint2O& mmpt2O = *(mp.ExtractData<FEMicroMaterialPoint2O>());
		
		// material point coordinates
		// TODO: I'm not entirly happy with this solution
		//		 since the material point coordinates are used by most materials.
		pt.m_r0 = el.Evaluate(r0, n);
		pt.m_rt = el.Evaluate(rt, n);

		// get the deformation gradient and determinant
		pt.m_J = defgrad(el, pt.m_F, n);
		defhess(el, n, mmpt2O.m_G);

		pmat->Stress2O(mp);
	}
}

//-----------------------------------------------------------------------------
//! calculates element's geometrical stiffness component for integration point n
void FEElasticMultiscaleDomain2O::ElementGeometricalStiffness(FESolidElement &el, matrix &ke)
{
	int n, i, j;

	double Gx[FEElement::MAX_NODES];
	double Gy[FEElement::MAX_NODES];
	double Gz[FEElement::MAX_NODES];
	double *Grn, *Gsn, *Gtn;
	double Gr, Gs, Gt;

	// nr of nodes
	int neln = el.Nodes();

	// nr of integration points
	int nint = el.GaussPoints();

	// jacobian
	double Ji[3][3], detJt;

	// weights at gauss points
	const double *gw = el.GaussWeights();

	// stiffness component for the initial stress component of stiffness matrix
	double kab;

	// calculate geometrical element stiffness matrix
	for (n=0; n<nint; ++n)
	{
		// calculate jacobian
		detJt = invjact(el, Ji, n)*gw[n];

		Grn = el.Gr(n);
		Gsn = el.Gs(n);
		Gtn = el.Gt(n);

		//// LTE 
		double *Grrn = el.Grr(n);
		double *Grsn = el.Grs(n);
		double *Grtn = el.Grt(n);

		double *Gsrn = el.Gsr(n);
		double *Gssn = el.Gss(n);
		double *Gstn = el.Gst(n);

		double *Gtrn = el.Gtr(n);
		double *Gtsn = el.Gts(n);
		double *Gttn = el.Gtt(n);


		for (i=0; i<neln; ++i)
		{
			Gr = Grn[i];
			Gs = Gsn[i];
			Gt = Gtn[i];

			// calculate global gradient of shape functions
			// note that we need the transposed of Ji, not Ji itself !
			Gx[i] = Ji[0][0]*Gr+Ji[1][0]*Gs+Ji[2][0]*Gt;
			Gy[i] = Ji[0][1]*Gr+Ji[1][1]*Gs+Ji[2][1]*Gt;
			Gz[i] = Ji[0][2]*Gr+Ji[1][2]*Gs+Ji[2][2]*Gt;
		}

		// get the material point data
		FEMaterialPoint& mp = *el.GetMaterialPoint(n);
		FEElasticMaterialPoint& pt = *(mp.ExtractData<FEElasticMaterialPoint>());
		FEMicroMaterialPoint2O& mmpt2O = *(mp.ExtractData<FEMicroMaterialPoint2O>());
		
		// element's Cauchy-stress tensor at gauss point n
		// s is the voight vector
		mat3ds s = pt.m_s;
		tens3ds tau = mmpt2O.m_tau;

		double Grrj, Grsj, Grtj, Gsrj, Gssj, Gstj, Gtrj, Gtsj, Gttj;

		for (i=0; i<neln; ++i)
			
			for (j=i; j<neln; ++j)
			{
				Grrj = Grrn[j];
				Grsj = Grsn[j];
				Grtj = Grtn[j];

				Gsrj = Gsrn[j];
				Gssj = Gssn[j];
				Gstj = Gstn[j];

				Gtrj = Gtrn[j];
				Gtsj = Gtsn[j];
				Gttj = Gttn[j];

				// calculate global gradient of shape functions
				// note that we need the transposed of Ji, not Ji itself !	
				// Calculate based on Gjk = Ji[l][j]*Glm*Ji[m][k]
				double GXX = Ji[0][0]*Grrj*Ji[0][0] + Ji[0][0]*Grsj*Ji[1][0] + Ji[0][0]*Grtj*Ji[2][0] + Ji[1][0]*Gsrj*Ji[0][0] + Ji[1][0]*Gssj*Ji[1][0] + Ji[1][0]*Gstj*Ji[2][0] + Ji[2][0]*Gtrj*Ji[0][0] + Ji[2][0]*Gtsj*Ji[1][0] + Ji[2][0]*Gttj*Ji[2][0];
				double GXY = Ji[0][0]*Grrj*Ji[0][1] + Ji[0][0]*Grsj*Ji[1][1] + Ji[0][0]*Grtj*Ji[2][1] + Ji[1][0]*Gsrj*Ji[0][1] + Ji[1][0]*Gssj*Ji[1][1] + Ji[1][0]*Gstj*Ji[2][1] + Ji[2][0]*Gtrj*Ji[0][1] + Ji[2][0]*Gtsj*Ji[1][1] + Ji[2][0]*Gttj*Ji[2][1];
				double GXZ = Ji[0][0]*Grrj*Ji[0][2] + Ji[0][0]*Grsj*Ji[1][2] + Ji[0][0]*Grtj*Ji[2][2] + Ji[1][0]*Gsrj*Ji[0][2] + Ji[1][0]*Gssj*Ji[1][2] + Ji[1][0]*Gstj*Ji[2][2] + Ji[2][0]*Gtrj*Ji[0][2] + Ji[2][0]*Gtsj*Ji[1][2] + Ji[2][0]*Gttj*Ji[2][2];
	
				double GYX = Ji[0][1]*Grrj*Ji[0][0] + Ji[0][1]*Grsj*Ji[1][0] + Ji[0][1]*Grtj*Ji[2][0] + Ji[1][1]*Gsrj*Ji[0][0] + Ji[1][1]*Gssj*Ji[1][0] + Ji[1][1]*Gstj*Ji[2][0] + Ji[2][1]*Gtrj*Ji[0][0] + Ji[2][1]*Gtsj*Ji[1][0] + Ji[2][1]*Gttj*Ji[2][0];
				double GYY = Ji[0][1]*Grrj*Ji[0][1] + Ji[0][1]*Grsj*Ji[1][1] + Ji[0][1]*Grtj*Ji[2][1] + Ji[1][1]*Gsrj*Ji[0][1] + Ji[1][1]*Gssj*Ji[1][1] + Ji[1][1]*Gstj*Ji[2][1] + Ji[2][1]*Gtrj*Ji[0][1] + Ji[2][1]*Gtsj*Ji[1][1] + Ji[2][1]*Gttj*Ji[2][1];
				double GYZ = Ji[0][1]*Grrj*Ji[0][2] + Ji[0][1]*Grsj*Ji[1][2] + Ji[0][1]*Grtj*Ji[2][2] + Ji[1][1]*Gsrj*Ji[0][2] + Ji[1][1]*Gssj*Ji[1][2] + Ji[1][1]*Gstj*Ji[2][2] + Ji[2][1]*Gtrj*Ji[0][2] + Ji[2][1]*Gtsj*Ji[1][2] + Ji[2][1]*Gttj*Ji[2][2];

				double GZX = Ji[0][2]*Grrj*Ji[0][0] + Ji[0][2]*Grsj*Ji[1][0] + Ji[0][2]*Grtj*Ji[2][0] + Ji[1][2]*Gsrj*Ji[0][0] + Ji[1][2]*Gssj*Ji[1][0] + Ji[1][2]*Gstj*Ji[2][0] + Ji[2][2]*Gtrj*Ji[0][0] + Ji[2][2]*Gtsj*Ji[1][0] + Ji[2][2]*Gttj*Ji[2][0];
				double GZY = Ji[0][2]*Grrj*Ji[0][1] + Ji[0][2]*Grsj*Ji[1][1] + Ji[0][2]*Grtj*Ji[2][1] + Ji[1][2]*Gsrj*Ji[0][1] + Ji[1][2]*Gssj*Ji[1][1] + Ji[1][2]*Gstj*Ji[2][1] + Ji[2][2]*Gtrj*Ji[0][1] + Ji[2][2]*Gtsj*Ji[1][1] + Ji[2][2]*Gttj*Ji[2][1];
				double GZZ = Ji[0][2]*Grrj*Ji[0][2] + Ji[0][2]*Grsj*Ji[1][2] + Ji[0][2]*Grtj*Ji[2][2] + Ji[1][2]*Gsrj*Ji[0][2] + Ji[1][2]*Gssj*Ji[1][2] + Ji[1][2]*Gstj*Ji[2][2] + Ji[2][2]*Gtrj*Ji[0][2] + Ji[2][2]*Gtsj*Ji[1][2] + Ji[2][2]*Gttj*Ji[2][2];
						
				kab = ((Gx[i]*(s.xx()*Gx[j] + s.xy()*Gy[j] + s.xz()*Gz[j])*detJt + (tau.d[0]*GXX + tau.d[1]*(GXY + GYX) + tau.d[2]*(GXZ + GZX) + tau.d[3]*GYY + tau.d[4]*(GYZ + GZY) + tau.d[5]*GZZ))*detJt*detJt +
					   (Gy[i]*(s.xy()*Gx[j] + s.yy()*Gy[j] + s.yz()*Gz[j])*detJt + (tau.d[1]*GXX + tau.d[3]*(GXY + GYX) + tau.d[4]*(GXZ + GZX) + tau.d[6]*GYY + tau.d[7]*(GYZ + GZY) + tau.d[8]*GZZ))*detJt*detJt + 
					   (Gz[i]*(s.xz()*Gx[j] + s.yz()*Gy[j] + s.zz()*Gz[j])*detJt + (tau.d[2]*GXX + tau.d[4]*(GXY + GYX) + tau.d[5]*(GXZ + GZX) + tau.d[7]*GYY + tau.d[8]*(GYZ + GZY) + tau.d[9]*GZZ))*detJt*detJt);

				ke[3*i  ][3*j  ] += kab;
				ke[3*i+1][3*j+1] += kab;
				ke[3*i+2][3*j+2] += kab;
			}
	}
}

//-----------------------------------------------------------------------------
//! Calculates element material stiffness element matrix
void FEElasticMultiscaleDomain2O::ElementMaterialStiffness(FESolidElement &el, matrix &ke)
{
	int i, i3, j, j3, n;

	// Get the current element's data
	const int nint = el.GaussPoints();
	const int neln = el.Nodes();
	const int ndof = 3*neln;

	// global derivatives of shape functions
	// Gx = dH/dx
	double Gx[FEElement::MAX_NODES];
	double Gy[FEElement::MAX_NODES];
	double Gz[FEElement::MAX_NODES];

	double Gxi, Gyi, Gzi;
	double Gxj, Gyj, Gzj;

	// The 'D' matrix
	double D[6][6] = {0};	// The 'D' matrix

	// The 'D*BL' matrix
	double DBL[6][3];

	double *Grn, *Gsn, *Gtn;
	double Gr, Gs, Gt;

	// jacobian
	double Ji[3][3], detJt;
	
	// weights at gauss points
	const double *gw = el.GaussWeights();

	// calculate element stiffness matrix
	for (n=0; n<nint; ++n)
	{
		// calculate jacobian
		detJt = invjact(el, Ji, n)*gw[n];
		Grn = el.Gr(n);
		Gsn = el.Gs(n);
		Gtn = el.Gt(n);

		//// LTE 
		double *Grrn = el.Grr(n);
		double *Grsn = el.Grs(n);
		double *Grtn = el.Grt(n);

		double *Gsrn = el.Gsr(n);
		double *Gssn = el.Gss(n);
		double *Gstn = el.Gst(n);

		double *Gtrn = el.Gtr(n);
		double *Gtsn = el.Gts(n);
		double *Gttn = el.Gtt(n);

		// setup the material point
		// NOTE: deformation gradient and determinant have already been evaluated in the stress routine
		FEMaterialPoint& mp = *el.GetMaterialPoint(n);
		FEElasticMaterialPoint& pt = *(mp.ExtractData<FEElasticMaterialPoint>());
		FEMicroMaterialPoint2O& mmpt2O = *(mp.ExtractData<FEMicroMaterialPoint2O>());

		// get the 'D' matrix
		tens4ds c; c.zero();
		tens5ds d; d.zero();
		tens6ds e; e.zero();

		FEMicroMaterial2O* pmat = dynamic_cast<FEMicroMaterial2O*>(m_pMat);
		pmat->Tangent2O(mp, c, d, e);
		
		c.extract(D);

		double Grri, Grsi, Grti, Gsri, Gssi, Gsti, Gtri, Gtsi, Gtti;
		double Grrj, Grsj, Grtj, Gsrj, Gssj, Gstj, Gtrj, Gtsj, Gttj;

		for (i=0; i<neln; ++i)
		{
			Gr = Grn[i];
			Gs = Gsn[i];
			Gt = Gtn[i];

			// calculate global gradient of shape functions
			// note that we need the transposed of Ji, not Ji itself !
			Gx[i] = Ji[0][0]*Gr+Ji[1][0]*Gs+Ji[2][0]*Gt;
			Gy[i] = Ji[0][1]*Gr+Ji[1][1]*Gs+Ji[2][1]*Gt;
			Gz[i] = Ji[0][2]*Gr+Ji[1][2]*Gs+Ji[2][2]*Gt;
		}

		// we only calculate the upper triangular part
		// since ke is symmetric. The other part is
		// determined below using this symmetry.
		for (i=0, i3=0; i<neln; ++i, i3 += 3)
		{
			Gxi = Gx[i];
			Gyi = Gy[i];
			Gzi = Gz[i];

			Grri = Grrn[i];
			Grsi = Grsn[i];
			Grti = Grtn[i];

			Gsri = Gsrn[i];
			Gssi = Gssn[i];
			Gsti = Gstn[i];

			Gtri = Gtrn[i];
			Gtsi = Gtsn[i];
			Gtti = Gttn[i];

			// calculate global gradient of shape functions
			// note that we need the transposed of Ji, not Ji itself !	
			// Calculate based on Gjk = Ji[l][j]*Glm*Ji[m][k]
			double GXXi = Ji[0][0]*Grri*Ji[0][0] + Ji[0][0]*Grsi*Ji[1][0] + Ji[0][0]*Grti*Ji[2][0] + Ji[1][0]*Gsri*Ji[0][0] + Ji[1][0]*Gssi*Ji[1][0] + Ji[1][0]*Gsti*Ji[2][0] + Ji[2][0]*Gtri*Ji[0][0] + Ji[2][0]*Gtsi*Ji[1][0] + Ji[2][0]*Gtti*Ji[2][0];
			double GXYi = Ji[0][0]*Grri*Ji[0][1] + Ji[0][0]*Grsi*Ji[1][1] + Ji[0][0]*Grti*Ji[2][1] + Ji[1][0]*Gsri*Ji[0][1] + Ji[1][0]*Gssi*Ji[1][1] + Ji[1][0]*Gsti*Ji[2][1] + Ji[2][0]*Gtri*Ji[0][1] + Ji[2][0]*Gtsi*Ji[1][1] + Ji[2][0]*Gtti*Ji[2][1];
			double GXZi = Ji[0][0]*Grri*Ji[0][2] + Ji[0][0]*Grsi*Ji[1][2] + Ji[0][0]*Grti*Ji[2][2] + Ji[1][0]*Gsri*Ji[0][2] + Ji[1][0]*Gssi*Ji[1][2] + Ji[1][0]*Gsti*Ji[2][2] + Ji[2][0]*Gtri*Ji[0][2] + Ji[2][0]*Gtsi*Ji[1][2] + Ji[2][0]*Gtti*Ji[2][2];
		
			double GYXi = Ji[0][1]*Grri*Ji[0][0] + Ji[0][1]*Grsi*Ji[1][0] + Ji[0][1]*Grti*Ji[2][0] + Ji[1][1]*Gsri*Ji[0][0] + Ji[1][1]*Gssi*Ji[1][0] + Ji[1][1]*Gsti*Ji[2][0] + Ji[2][1]*Gtri*Ji[0][0] + Ji[2][1]*Gtsi*Ji[1][0] + Ji[2][1]*Gtti*Ji[2][0];
			double GYYi = Ji[0][1]*Grri*Ji[0][1] + Ji[0][1]*Grsi*Ji[1][1] + Ji[0][1]*Grti*Ji[2][1] + Ji[1][1]*Gsri*Ji[0][1] + Ji[1][1]*Gssi*Ji[1][1] + Ji[1][1]*Gsti*Ji[2][1] + Ji[2][1]*Gtri*Ji[0][1] + Ji[2][1]*Gtsi*Ji[1][1] + Ji[2][1]*Gtti*Ji[2][1];
			double GYZi = Ji[0][1]*Grri*Ji[0][2] + Ji[0][1]*Grsi*Ji[1][2] + Ji[0][1]*Grti*Ji[2][2] + Ji[1][1]*Gsri*Ji[0][2] + Ji[1][1]*Gssi*Ji[1][2] + Ji[1][1]*Gsti*Ji[2][2] + Ji[2][1]*Gtri*Ji[0][2] + Ji[2][1]*Gtsi*Ji[1][2] + Ji[2][1]*Gtti*Ji[2][2];

			double GZXi = Ji[0][2]*Grri*Ji[0][0] + Ji[0][2]*Grsi*Ji[1][0] + Ji[0][2]*Grti*Ji[2][0] + Ji[1][2]*Gsri*Ji[0][0] + Ji[1][2]*Gssi*Ji[1][0] + Ji[1][2]*Gsti*Ji[2][0] + Ji[2][2]*Gtri*Ji[0][0] + Ji[2][2]*Gtsi*Ji[1][0] + Ji[2][2]*Gtti*Ji[2][0];
			double GZYi = Ji[0][2]*Grri*Ji[0][1] + Ji[0][2]*Grsi*Ji[1][1] + Ji[0][2]*Grti*Ji[2][1] + Ji[1][2]*Gsri*Ji[0][1] + Ji[1][2]*Gssi*Ji[1][1] + Ji[1][2]*Gsti*Ji[2][1] + Ji[2][2]*Gtri*Ji[0][1] + Ji[2][2]*Gtsi*Ji[1][1] + Ji[2][2]*Gtti*Ji[2][1];
			double GZZi = Ji[0][2]*Grri*Ji[0][2] + Ji[0][2]*Grsi*Ji[1][2] + Ji[0][2]*Grti*Ji[2][2] + Ji[1][2]*Gsri*Ji[0][2] + Ji[1][2]*Gssi*Ji[1][2] + Ji[1][2]*Gsti*Ji[2][2] + Ji[2][2]*Gtri*Ji[0][2] + Ji[2][2]*Gtsi*Ji[1][2] + Ji[2][2]*Gtti*Ji[2][2];
		
			for (j=i, j3 = i3; j<neln; ++j, j3 += 3)
			{
				Gxj = Gx[j];
				Gyj = Gy[j];
				Gzj = Gz[j];

				Grrj = Grrn[j];
				Grsj = Grsn[j];
				Grtj = Grtn[j];

				Gsrj = Gsrn[j];
				Gssj = Gssn[j];
				Gstj = Gstn[j];

				Gtrj = Gtrn[j];
				Gtsj = Gtsn[j];
				Gttj = Gttn[j];

				// calculate global gradient of shape functions
				// note that we need the transposed of Ji, not Ji itself !	
				// Calculate based on Gjk = Ji[l][j]*Glm*Ji[m][k]
				double GXXj = Ji[0][0]*Grrj*Ji[0][0] + Ji[0][0]*Grsj*Ji[1][0] + Ji[0][0]*Grtj*Ji[2][0] + Ji[1][0]*Gsrj*Ji[0][0] + Ji[1][0]*Gssj*Ji[1][0] + Ji[1][0]*Gstj*Ji[2][0] + Ji[2][0]*Gtrj*Ji[0][0] + Ji[2][0]*Gtsj*Ji[1][0] + Ji[2][0]*Gttj*Ji[2][0];
				double GXYj = Ji[0][0]*Grrj*Ji[0][1] + Ji[0][0]*Grsj*Ji[1][1] + Ji[0][0]*Grtj*Ji[2][1] + Ji[1][0]*Gsrj*Ji[0][1] + Ji[1][0]*Gssj*Ji[1][1] + Ji[1][0]*Gstj*Ji[2][1] + Ji[2][0]*Gtrj*Ji[0][1] + Ji[2][0]*Gtsj*Ji[1][1] + Ji[2][0]*Gttj*Ji[2][1];
				double GXZj = Ji[0][0]*Grrj*Ji[0][2] + Ji[0][0]*Grsj*Ji[1][2] + Ji[0][0]*Grtj*Ji[2][2] + Ji[1][0]*Gsrj*Ji[0][2] + Ji[1][0]*Gssj*Ji[1][2] + Ji[1][0]*Gstj*Ji[2][2] + Ji[2][0]*Gtrj*Ji[0][2] + Ji[2][0]*Gtsj*Ji[1][2] + Ji[2][0]*Gttj*Ji[2][2];
	
				double GYXj = Ji[0][1]*Grrj*Ji[0][0] + Ji[0][1]*Grsj*Ji[1][0] + Ji[0][1]*Grtj*Ji[2][0] + Ji[1][1]*Gsrj*Ji[0][0] + Ji[1][1]*Gssj*Ji[1][0] + Ji[1][1]*Gstj*Ji[2][0] + Ji[2][1]*Gtrj*Ji[0][0] + Ji[2][1]*Gtsj*Ji[1][0] + Ji[2][1]*Gttj*Ji[2][0];
				double GYYj = Ji[0][1]*Grrj*Ji[0][1] + Ji[0][1]*Grsj*Ji[1][1] + Ji[0][1]*Grtj*Ji[2][1] + Ji[1][1]*Gsrj*Ji[0][1] + Ji[1][1]*Gssj*Ji[1][1] + Ji[1][1]*Gstj*Ji[2][1] + Ji[2][1]*Gtrj*Ji[0][1] + Ji[2][1]*Gtsj*Ji[1][1] + Ji[2][1]*Gttj*Ji[2][1];
				double GYZj = Ji[0][1]*Grrj*Ji[0][2] + Ji[0][1]*Grsj*Ji[1][2] + Ji[0][1]*Grtj*Ji[2][2] + Ji[1][1]*Gsrj*Ji[0][2] + Ji[1][1]*Gssj*Ji[1][2] + Ji[1][1]*Gstj*Ji[2][2] + Ji[2][1]*Gtrj*Ji[0][2] + Ji[2][1]*Gtsj*Ji[1][2] + Ji[2][1]*Gttj*Ji[2][2];

				double GZXj = Ji[0][2]*Grrj*Ji[0][0] + Ji[0][2]*Grsj*Ji[1][0] + Ji[0][2]*Grtj*Ji[2][0] + Ji[1][2]*Gsrj*Ji[0][0] + Ji[1][2]*Gssj*Ji[1][0] + Ji[1][2]*Gstj*Ji[2][0] + Ji[2][2]*Gtrj*Ji[0][0] + Ji[2][2]*Gtsj*Ji[1][0] + Ji[2][2]*Gttj*Ji[2][0];
				double GZYj = Ji[0][2]*Grrj*Ji[0][1] + Ji[0][2]*Grsj*Ji[1][1] + Ji[0][2]*Grtj*Ji[2][1] + Ji[1][2]*Gsrj*Ji[0][1] + Ji[1][2]*Gssj*Ji[1][1] + Ji[1][2]*Gstj*Ji[2][1] + Ji[2][2]*Gtrj*Ji[0][1] + Ji[2][2]*Gtsj*Ji[1][1] + Ji[2][2]*Gttj*Ji[2][1];
				double GZZj = Ji[0][2]*Grrj*Ji[0][2] + Ji[0][2]*Grsj*Ji[1][2] + Ji[0][2]*Grtj*Ji[2][2] + Ji[1][2]*Gsrj*Ji[0][2] + Ji[1][2]*Gssj*Ji[1][2] + Ji[1][2]*Gstj*Ji[2][2] + Ji[2][2]*Gtrj*Ji[0][2] + Ji[2][2]*Gtsj*Ji[1][2] + Ji[2][2]*Gttj*Ji[2][2];
				
				// First order component (C*d)
				// calculate D*BL matrices
				DBL[0][0] = (D[0][0]*Gxj+D[0][3]*Gyj+D[0][5]*Gzj);
				DBL[0][1] = (D[0][1]*Gyj+D[0][3]*Gxj+D[0][4]*Gzj);
				DBL[0][2] = (D[0][2]*Gzj+D[0][4]*Gyj+D[0][5]*Gxj);

				DBL[1][0] = (D[1][0]*Gxj+D[1][3]*Gyj+D[1][5]*Gzj);
				DBL[1][1] = (D[1][1]*Gyj+D[1][3]*Gxj+D[1][4]*Gzj);
				DBL[1][2] = (D[1][2]*Gzj+D[1][4]*Gyj+D[1][5]*Gxj);

				DBL[2][0] = (D[2][0]*Gxj+D[2][3]*Gyj+D[2][5]*Gzj);
				DBL[2][1] = (D[2][1]*Gyj+D[2][3]*Gxj+D[2][4]*Gzj);
				DBL[2][2] = (D[2][2]*Gzj+D[2][4]*Gyj+D[2][5]*Gxj);

				DBL[3][0] = (D[3][0]*Gxj+D[3][3]*Gyj+D[3][5]*Gzj);
				DBL[3][1] = (D[3][1]*Gyj+D[3][3]*Gxj+D[3][4]*Gzj);
				DBL[3][2] = (D[3][2]*Gzj+D[3][4]*Gyj+D[3][5]*Gxj);

				DBL[4][0] = (D[4][0]*Gxj+D[4][3]*Gyj+D[4][5]*Gzj);
				DBL[4][1] = (D[4][1]*Gyj+D[4][3]*Gxj+D[4][4]*Gzj);
				DBL[4][2] = (D[4][2]*Gzj+D[4][4]*Gyj+D[4][5]*Gxj);

				DBL[5][0] = (D[5][0]*Gxj+D[5][3]*Gyj+D[5][5]*Gzj);
				DBL[5][1] = (D[5][1]*Gyj+D[5][3]*Gxj+D[5][4]*Gzj);
				DBL[5][2] = (D[5][2]*Gzj+D[5][4]*Gyj+D[5][5]*Gxj);

				ke[i3  ][j3  ] += (Gxi*DBL[0][0] + Gyi*DBL[3][0] + Gzi*DBL[5][0] )*detJt;
				ke[i3  ][j3+1] += (Gxi*DBL[0][1] + Gyi*DBL[3][1] + Gzi*DBL[5][1] )*detJt;
				ke[i3  ][j3+2] += (Gxi*DBL[0][2] + Gyi*DBL[3][2] + Gzi*DBL[5][2] )*detJt;

				ke[i3+1][j3  ] += (Gyi*DBL[1][0] + Gxi*DBL[3][0] + Gzi*DBL[4][0] )*detJt;
				ke[i3+1][j3+1] += (Gyi*DBL[1][1] + Gxi*DBL[3][1] + Gzi*DBL[4][1] )*detJt;
				ke[i3+1][j3+2] += (Gyi*DBL[1][2] + Gxi*DBL[3][2] + Gzi*DBL[4][2] )*detJt;

				ke[i3+2][j3  ] += (Gzi*DBL[2][0] + Gyi*DBL[4][0] + Gxi*DBL[5][0] )*detJt;
				ke[i3+2][j3+1] += (Gzi*DBL[2][1] + Gyi*DBL[4][1] + Gxi*DBL[5][1] )*detJt;
				ke[i3+2][j3+2] += (Gzi*DBL[2][2] + Gyi*DBL[4][2] + Gxi*DBL[5][2] )*detJt;

				// Second-order components with D*d + D*k
				ke[i3  ][j3  ] += (Gxi*(d.d[0]*GXXj + d.d[1]*(GXYj + GYXj) + d.d[2]*(GXZj + GZXj) + d.d[3]*GYYj + d.d[4]*(GYZj + GZYj) + d.d[5]*GZZj)
					             + Gyi*(d.d[1]*GXXj + d.d[3]*(GXYj + GYXj) + d.d[4]*(GXZj + GZXj) + d.d[10]*GYYj + d.d[11]*(GYZj + GZYj) + d.d[12]*GZZj)
								 + Gzi*(d.d[2]*GXXj + d.d[4]*(GXYj + GYXj) + d.d[5]*(GXZj + GZXj) + d.d[11]*GYYj + d.d[12]*(GYZj + GZYj) + d.d[17]*GZZj))*detJt*detJt;
				
				ke[i3  ][j3+1] += (Gxi*(d.d[1]*GXXj + d.d[3]*(GXYj + GYXj) + d.d[4]*(GXZj + GZXj) + d.d[6]*GYYj + d.d[7]*(GYZj + GZYj) + d.d[8]*GZZj)
					             + Gyi*(d.d[3]*GXXj + d.d[10]*(GXYj + GYXj) + d.d[11]*(GXZj + GZXj) + d.d[13]*GYYj + d.d[14]*(GYZj + GZYj) + d.d[15]*GZZj)
								 + Gzi*(d.d[4]*GXXj + d.d[11]*(GXYj + GYXj) + d.d[12]*(GXZj + GZXj) + d.d[18]*GYYj + d.d[19]*(GYZj + GZYj) + d.d[20]*GZZj))*detJt*detJt;

				ke[i3  ][j3+2] += (Gxi*(d.d[2]*GXXj + d.d[4]*(GXYj + GYXj) + d.d[5]*(GXZj + GZXj) + d.d[7]*GYYj + d.d[8]*(GYZj + GZYj) + d.d[9]*GZZj)
					             + Gyi*(d.d[4]*GXXj + d.d[11]*(GXYj + GYXj) + d.d[12]*(GXZj + GZXj) + d.d[14]*GYYj + d.d[15]*(GYZj + GZYj) + d.d[16]*GZZj)
								 + Gzi*(d.d[5]*GXXj + d.d[12]*(GXYj + GYXj) + d.d[17]*(GXZj + GZXj) + d.d[19]*GYYj + d.d[20]*(GYZj + GZYj) + d.d[21]*GZZj))*detJt*detJt;

				ke[i3+1][j3  ] += (Gxi*(d.d[1]*GXXj + d.d[3]*(GXYj + GYXj) + d.d[4]*(GXZj + GZXj) + d.d[10]*GYYj + d.d[11]*(GYZj + GZYj) + d.d[12]*GZZj)
					             + Gyi*(d.d[3]*GXXj + d.d[10]*(GXYj + GYXj) + d.d[11]*(GXZj + GZXj) + d.d[13]*GYYj + d.d[14]*(GYZj + GZYj) + d.d[19]*GZZj)
								 + Gzi*(d.d[4]*GXXj + d.d[11]*(GXYj + GYXj) + d.d[12]*(GXZj + GZXj) + d.d[14]*GYYj + d.d[15]*(GYZj + GZYj) + d.d[20]*GZZj))*detJt*detJt;
				
				ke[i3+1][j3+1] += (Gxi*(d.d[3]*GXXj + d.d[10]*(GXYj + GYXj) + d.d[11]*(GXZj + GZXj) + d.d[13]*GYYj + d.d[14]*(GYZj + GZYj) + d.d[15]*GZZj)
					             + Gyi*(d.d[10]*GXXj + d.d[13]*(GXYj + GYXj) + d.d[14]*(GXZj + GZXj) + d.d[22]*GYYj + d.d[23]*(GYZj + GZYj) + d.d[24]*GZZj)
								 + Gzi*(d.d[11]*GXXj + d.d[14]*(GXYj + GYXj) + d.d[15]*(GXZj + GZXj) + d.d[23]*GYYj + d.d[24]*(GYZj + GZYj) + d.d[26]*GZZj))*detJt*detJt;

				ke[i3+1][j3+2] += (Gxi*(d.d[4]*GXXj + d.d[11]*(GXYj + GYXj) + d.d[12]*(GXZj + GZXj) + d.d[14]*GYYj + d.d[15]*(GYZj + GZYj) + d.d[16]*GZZj)
					             + Gyi*(d.d[11]*GXXj + d.d[14]*(GXYj + GYXj) + d.d[19]*(GXZj + GZXj) + d.d[23]*GYYj + d.d[24]*(GYZj + GZYj) + d.d[25]*GZZj)
								 + Gzi*(d.d[12]*GXXj + d.d[15]*(GXYj + GYXj) + d.d[20]*(GXZj + GZXj) + d.d[24]*GYYj + d.d[26]*(GYZj + GZYj) + d.d[27]*GZZj))*detJt*detJt;

				ke[i3+2][j3  ] += (Gxi*(d.d[2]*GXXj + d.d[4]*(GXYj + GYXj) + d.d[5]*(GXZj + GZXj) + d.d[11]*GYYj + d.d[12]*(GYZj + GZYj) + d.d[17]*GZZj)
					             + Gyi*(d.d[4]*GXXj + d.d[11]*(GXYj + GYXj) + d.d[12]*(GXZj + GZXj) + d.d[14]*GYYj + d.d[15]*(GYZj + GZYj) + d.d[20]*GZZj)
								 + Gzi*(d.d[5]*GXXj + d.d[12]*(GXYj + GYXj) + d.d[17]*(GXZj + GZXj) + d.d[19]*GYYj + d.d[20]*(GYZj + GZYj) + d.d[21]*GZZj))*detJt*detJt;

				ke[i3+2][j3+1] += (Gxi*(d.d[4]*GXXj + d.d[11]*(GXYj + GYXj) + d.d[12]*(GXZj + GZXj) + d.d[14]*GYYj + d.d[15]*(GYZj + GZYj) + d.d[20]*GZZj)
					             + Gyi*(d.d[11]*GXXj + d.d[14]*(GXYj + GYXj) + d.d[15]*(GXZj + GZXj) + d.d[23]*GYYj + d.d[24]*(GYZj + GZYj) + d.d[26]*GZZj)
								 + Gzi*(d.d[12]*GXXj + d.d[19]*(GXYj + GYXj) + d.d[20]*(GXZj + GZXj) + d.d[24]*GYYj + d.d[26]*(GYZj + GZYj) + d.d[27]*GZZj))*detJt*detJt;

				ke[i3+2][j3+2] += (Gxi*(d.d[5]*GXXj + d.d[12]*(GXYj + GYXj) + d.d[17]*(GXZj + GZXj) + d.d[15]*GYYj + d.d[20]*(GYZj + GZYj) + d.d[21]*GZZj)
					             + Gyi*(d.d[12]*GXXj + d.d[15]*(GXYj + GYXj) + d.d[20]*(GXZj + GZXj) + d.d[24]*GYYj + d.d[26]*(GYZj + GZYj) + d.d[27]*GZZj)
								 + Gzi*(d.d[17]*GXXj + d.d[20]*(GXYj + GYXj) + d.d[21]*(GXZj + GZXj) + d.d[26]*GYYj + d.d[27]*(GYZj + GZYj) + d.d[28]*GZZj))*detJt*detJt;

				
				ke[i3  ][j3  ] += (Gxj*(d.d[0]*GXXi + d.d[1]*(GXYi + GYXi) + d.d[2]*(GXZi + GZXi) + d.d[3]*GYYi + d.d[4]*(GYZi + GZYi) + d.d[5]*GZZi)
					             + Gyj*(d.d[1]*GXXi + d.d[3]*(GXYi + GYXi) + d.d[4]*(GXZi + GZXi) + d.d[10]*GYYi + d.d[11]*(GYZi + GZYi) + d.d[12]*GZZi)
								 + Gzj*(d.d[2]*GXXi + d.d[4]*(GXYi + GYXi) + d.d[5]*(GXZi + GZXi) + d.d[11]*GYYi + d.d[12]*(GYZi + GZYi) + d.d[17]*GZZi))*detJt*detJt;
		
				ke[i3  ][j3+1] += (Gxj*(d.d[1]*GXXi + d.d[3]*(GXYi + GYXi) + d.d[4]*(GXZi + GZXi) + d.d[6]*GYYi + d.d[7]*(GYZi + GZYi) + d.d[8]*GZZi)
					             + Gyj*(d.d[3]*GXXi + d.d[10]*(GXYi + GYXi) + d.d[11]*(GXZi + GZXi) + d.d[13]*GYYi + d.d[14]*(GYZi + GZYi) + d.d[15]*GZZi)
								 + Gzj*(d.d[4]*GXXi + d.d[11]*(GXYi + GYXi) + d.d[12]*(GXZi + GZXi) + d.d[18]*GYYi + d.d[19]*(GYZi + GZYi) + d.d[20]*GZZi))*detJt*detJt;

				ke[i3  ][j3+2] += (Gxj*(d.d[2]*GXXi + d.d[4]*(GXYi + GYXi) + d.d[5]*(GXZi + GZXi) + d.d[7]*GYYi + d.d[8]*(GYZi + GZYi) + d.d[9]*GZZi)
					             + Gyj*(d.d[4]*GXXi + d.d[11]*(GXYi + GYXi) + d.d[12]*(GXZi + GZXi) + d.d[14]*GYYi + d.d[15]*(GYZi + GZYi) + d.d[16]*GZZi)
								 + Gzj*(d.d[5]*GXXi + d.d[12]*(GXYi + GYXi) + d.d[17]*(GXZi + GZXi) + d.d[19]*GYYi + d.d[20]*(GYZi + GZYi) + d.d[21]*GZZi))*detJt*detJt;

				ke[i3+1][j3  ] += (Gxj*(d.d[1]*GXXi + d.d[3]*(GXYi + GYXi) + d.d[4]*(GXZi + GZXi) + d.d[10]*GYYi + d.d[11]*(GYZi + GZYi) + d.d[12]*GZZi)
					             + Gyj*(d.d[3]*GXXi + d.d[10]*(GXYi + GYXi) + d.d[11]*(GXZi + GZXi) + d.d[13]*GYYi + d.d[14]*(GYZi + GZYi) + d.d[19]*GZZi)
								 + Gzj*(d.d[4]*GXXi + d.d[11]*(GXYi + GYXi) + d.d[12]*(GXZi + GZXi) + d.d[14]*GYYi + d.d[15]*(GYZi + GZYi) + d.d[20]*GZZi))*detJt*detJt;
				
				ke[i3+1][j3+1] += (Gxj*(d.d[3]*GXXi + d.d[10]*(GXYi + GYXi) + d.d[11]*(GXZi + GZXi) + d.d[13]*GYYi + d.d[14]*(GYZi + GZYi) + d.d[15]*GZZi)
					             + Gyj*(d.d[10]*GXXi + d.d[13]*(GXYi + GYXi) + d.d[14]*(GXZi + GZXi) + d.d[22]*GYYi + d.d[23]*(GYZi + GZYi) + d.d[24]*GZZi)
								 + Gzj*(d.d[11]*GXXi + d.d[14]*(GXYi + GYXi) + d.d[15]*(GXZi + GZXi) + d.d[23]*GYYi + d.d[24]*(GYZi + GZYi) + d.d[26]*GZZi))*detJt*detJt;

				ke[i3+1][j3+2] += (Gxj*(d.d[4]*GXXi + d.d[11]*(GXYi + GYXi) + d.d[12]*(GXZi + GZXi) + d.d[14]*GYYi + d.d[15]*(GYZi + GZYi) + d.d[16]*GZZi)
					             + Gyj*(d.d[11]*GXXi + d.d[14]*(GXYi + GYXi) + d.d[19]*(GXZi + GZXi) + d.d[23]*GYYi + d.d[24]*(GYZi + GZYi) + d.d[25]*GZZi)
								 + Gzj*(d.d[12]*GXXi + d.d[15]*(GXYi + GYXi) + d.d[20]*(GXZi + GZXi) + d.d[24]*GYYi + d.d[26]*(GYZi + GZYi) + d.d[27]*GZZi))*detJt*detJt;

				ke[i3+2][j3  ] += (Gxj*(d.d[2]*GXXi + d.d[4]*(GXYi + GYXi) + d.d[5]*(GXZi + GZXi) + d.d[11]*GYYi + d.d[12]*(GYZi + GZYi) + d.d[17]*GZZi)
					             + Gyj*(d.d[4]*GXXi + d.d[11]*(GXYi + GYXi) + d.d[12]*(GXZi + GZXi) + d.d[14]*GYYi + d.d[15]*(GYZi + GZYi) + d.d[20]*GZZi)
								 + Gzj*(d.d[5]*GXXi + d.d[12]*(GXYi + GYXi) + d.d[17]*(GXZi + GZXi) + d.d[19]*GYYi + d.d[20]*(GYZi + GZYi) + d.d[21]*GZZi))*detJt*detJt;

				ke[i3+2][j3+1] += (Gxj*(d.d[4]*GXXi + d.d[11]*(GXYi + GYXi) + d.d[12]*(GXZi + GZXi) + d.d[14]*GYYi + d.d[15]*(GYZi + GZYi) + d.d[20]*GZZi)
					             + Gyj*(d.d[11]*GXXi + d.d[14]*(GXYi + GYXi) + d.d[15]*(GXZi + GZXi) + d.d[23]*GYYi + d.d[24]*(GYZi + GZYi) + d.d[26]*GZZi)
								 + Gzj*(d.d[12]*GXXi + d.d[19]*(GXYi + GYXi) + d.d[20]*(GXZi + GZXi) + d.d[24]*GYYi + d.d[26]*(GYZi + GZYi) + d.d[27]*GZZi))*detJt*detJt;

				ke[i3+2][j3+2] += (Gxj*(d.d[5]*GXXi + d.d[12]*(GXYi + GYXi) + d.d[17]*(GXZi + GZXi) + d.d[15]*GYYi + d.d[20]*(GYZi + GZYi) + d.d[21]*GZZi)
					             + Gyj*(d.d[12]*GXXi + d.d[15]*(GXYi + GYXi) + d.d[20]*(GXZi + GZXi) + d.d[24]*GYYi + d.d[26]*(GYZi + GZYi) + d.d[27]*GZZi)
								 + Gzj*(d.d[17]*GXXi + d.d[20]*(GXYi + GYXi) + d.d[21]*(GXZi + GZXi) + d.d[26]*GYYi + d.d[27]*(GYZi + GZYi) + d.d[28]*GZZi))*detJt*detJt;

				// Second-order component E*k
				ke[i3  ][j3  ] += (GXXi*(e.d[0]*GXXj + e.d[1]*(GXYj + GYXj) + e.d[2]*(GXZj + GZXj) + e.d[3]*GYYj + e.d[4]*(GYZj + GZYj) + e.d[5]*GZZj)
					            + (GXYi + GYXi)*(e.d[1]*GXXj + e.d[3]*(GXYj + GYXj) + e.d[4]*(GXZj + GZXj) + e.d[10]*GYYj + e.d[11]*(GYZj + GZYj) + e.d[12]*GZZj)
								+ (GXZi + GZXi)*(e.d[2]*GXXj + e.d[4]*(GXYj + GYXj) + e.d[5]*(GXZj + GZXj) + e.d[17]*GYYj + e.d[18]*(GYZj + GZYj) + e.d[19]*GZZj)
								 + GYYi*(e.d[3]*GXXj + e.d[10]*(GXYj + GYXj) + e.d[11]*(GXZj + GZXj) + e.d[13]*GYYj + e.d[14]*(GYZj + GZYj) + e.d[24]*GZZj)
								+ (GYZi + GZYi)*(e.d[4]*GXXj + e.d[11]*(GXYj + GYXj) + e.d[18]*(GXZj + GZXj) + e.d[14]*GYYj + e.d[24]*(GYZj + GZYj) + e.d[29]*GZZj)
								 + GZZi*(e.d[5]*GXXj + e.d[18]*(GXYj + GYXj) + e.d[19]*(GXZj + GZXj) + e.d[24]*GYYj + e.d[22]*(GYZj + GZYj) + e.d[23]*GZZj))*detJt*detJt;

				ke[i3  ][j3+1] += (GXXi*(e.d[1]*GXXj + e.d[3]*(GXYj + GYXj) + e.d[4]*(GXZj + GZXj) + e.d[6]*GYYj + e.d[7]*(GYZj + GZYj) + e.d[8]*GZZj)
					            + (GXYi + GYXi)*(e.d[3]*GXXj + e.d[10]*(GXYj + GYXj) + e.d[11]*(GXZj + GZXj) + e.d[13]*GYYj + e.d[14]*(GYZj + GZYj) + e.d[15]*GZZj)
								+ (GXZi + GZXi)*(e.d[4]*GXXj + e.d[17]*(GXYj + GYXj) + e.d[18]*(GXZj + GZXj) + e.d[20]*GYYj + e.d[21]*(GYZj + GZYj) + e.d[22]*GZZj)
								 + GYYi*(e.d[10]*GXXj + e.d[13]*(GXYj + GYXj) + e.d[14]*(GXZj + GZXj) + e.d[25]*GYYj + e.d[26]*(GYZj + GZYj) + e.d[27]*GZZj)
								+ (GYZi + GZYi)*(e.d[11]*GXXj + e.d[14]*(GXYj + GYXj) + e.d[24]*(GXZj + GZXj) + e.d[30]*GYYj + e.d[31]*(GYZj + GZYj) + e.d[32]*GZZj)
								 + GZZi*(e.d[18]*GXXj + e.d[24]*(GXYj + GYXj) + e.d[22]*(GXZj + GZXj) + e.d[34]*GYYj + e.d[35]*(GYZj + GZYj) + e.d[36]*GZZj))*detJt*detJt;

				ke[i3  ][j3+2] += (GXXi*(e.d[2]*GXXj + e.d[4]*(GXYj + GYXj) + e.d[5]*(GXZj + GZXj) + e.d[7]*GYYj + e.d[8]*(GYZj + GZYj) + e.d[9]*GZZj)
					            + (GXYi + GYXi)*(e.d[4]*GXXj + e.d[11]*(GXYj + GYXj) + e.d[12]*(GXZj + GZXj) + e.d[14]*GYYj + e.d[15]*(GYZj + GZYj) + e.d[16]*GZZj)
								+ (GXZi + GZXi)*(e.d[5]*GXXj + e.d[18]*(GXYj + GYXj) + e.d[19]*(GXZj + GZXj) + e.d[21]*GYYj + e.d[22]*(GYZj + GZYj) + e.d[23]*GZZj)
								 + GYYi*(e.d[11]*GXXj + e.d[14]*(GXYj + GYXj) + e.d[24]*(GXZj + GZXj) + e.d[26]*GYYj + e.d[27]*(GYZj + GZYj) + e.d[28]*GZZj)
								+ (GYZi + GZYi)*(e.d[18]*GXXj + e.d[24]*(GXYj + GYXj) + e.d[29]*(GXZj + GZXj) + e.d[31]*GYYj + e.d[32]*(GYZj + GZYj) + e.d[33]*GZZj)
								 + GZZi*(e.d[19]*GXXj + e.d[22]*(GXYj + GYXj) + e.d[23]*(GXZj + GZXj) + e.d[35]*GYYj + e.d[36]*(GYZj + GZYj) + e.d[37]*GZZj))*detJt*detJt;

				ke[i3+1][j3  ] += (GXXi*(e.d[1]*GXXj + e.d[3]*(GXYj + GYXj) + e.d[4]*(GXZj + GZXj) + e.d[10]*GYYj + e.d[11]*(GYZj + GZYj) + e.d[18]*GZZj)
					            + (GXYi + GYXi)*(e.d[3]*GXXj + e.d[10]*(GXYj + GYXj) + e.d[11]*(GXZj + GZXj) + e.d[13]*GYYj + e.d[14]*(GYZj + GZYj) + e.d[21]*GZZj)
								+ (GXZi + GZXi)*(e.d[4]*GXXj + e.d[11]*(GXYj + GYXj) + e.d[18]*(GXZj + GZXj) + e.d[14]*GYYj + e.d[21]*(GYZj + GZYj) + e.d[22]*GZZj)
								 + GYYi*(e.d[10]*GXXj + e.d[13]*(GXYj + GYXj) + e.d[14]*(GXZj + GZXj) + e.d[25]*GYYj + e.d[26]*(GYZj + GZYj) + e.d[31]*GZZj)
								+ (GYZi + GZYi)*(e.d[17]*GXXj + e.d[14]*(GXYj + GYXj) + e.d[24]*(GXZj + GZXj) + e.d[26]*GYYj + e.d[31]*(GYZj + GZYj) + e.d[35]*GZZj)
								 + GZZi*(e.d[18]*GXXj + e.d[21]*(GXYj + GYXj) + e.d[22]*(GXZj + GZXj) + e.d[31]*GYYj + e.d[35]*(GYZj + GZYj) + e.d[36]*GZZj))*detJt*detJt;

				ke[i3+1][j3+1] += (GXXi*(e.d[3]*GXXj + e.d[10]*(GXYj + GYXj) + e.d[11]*(GXZj + GZXj) + e.d[13]*GYYj + e.d[14]*(GYZj + GZYj) + e.d[15]*GZZj)
					            + (GXYi + GYXi)*(e.d[10]*GXXj + e.d[13]*(GXYj + GYXj) + e.d[14]*(GXZj + GZXj) + e.d[25]*GYYj + e.d[26]*(GYZj + GZYj) + e.d[27]*GZZj)
								+ (GXZi + GZXi)*(e.d[11]*GXXj + e.d[14]*(GXYj + GYXj) + e.d[21]*(GXZj + GZXj) + e.d[30]*GYYj + e.d[31]*(GYZj + GZYj) + e.d[32]*GZZj)
								 + GYYi*(e.d[13]*GXXj + e.d[25]*(GXYj + GYXj) + e.d[26]*(GXZj + GZXj) + e.d[38]*GYYj + e.d[39]*(GYZj + GZYj) + e.d[40]*GZZj)
								+ (GYZi + GZYi)*(e.d[14]*GXXj + e.d[26]*(GXYj + GYXj) + e.d[31]*(GXZj + GZXj) + e.d[39]*GYYj + e.d[40]*(GYZj + GZYj) + e.d[42]*GZZj)
								 + GZZi*(e.d[21]*GXXj + e.d[31]*(GXYj + GYXj) + e.d[35]*(GXZj + GZXj) + e.d[40]*GYYj + e.d[42]*(GYZj + GZYj) + e.d[43]*GZZj))*detJt*detJt;

				ke[i3+1][j3+2] += (GXXi*(e.d[4]*GXXj + e.d[11]*(GXYj + GYXj) + e.d[18]*(GXZj + GZXj) + e.d[14]*GYYj + e.d[15]*(GYZj + GZYj) + e.d[16]*GZZj)
					            + (GXYi + GYXi)*(e.d[11]*GXXj + e.d[14]*(GXYj + GYXj) + e.d[21]*(GXZj + GZXj) + e.d[26]*GYYj + e.d[27]*(GYZj + GZYj) + e.d[28]*GZZj)
								+ (GXZi + GZXi)*(e.d[18]*GXXj + e.d[21]*(GXYj + GYXj) + e.d[22]*(GXZj + GZXj) + e.d[31]*GYYj + e.d[32]*(GYZj + GZYj) + e.d[33]*GZZj)
								 + GYYi*(e.d[14]*GXXj + e.d[26]*(GXYj + GYXj) + e.d[31]*(GXZj + GZXj) + e.d[39]*GYYj + e.d[40]*(GYZj + GZYj) + e.d[41]*GZZj)
								+ (GYZi + GZYi)*(e.d[24]*GXXj + e.d[31]*(GXYj + GYXj) + e.d[35]*(GXZj + GZXj) + e.d[40]*GYYj + e.d[42]*(GYZj + GZYj) + e.d[43]*GZZj)
								 + GZZi*(e.d[22]*GXXj + e.d[35]*(GXYj + GYXj) + e.d[36]*(GXZj + GZXj) + e.d[42]*GYYj + e.d[43]*(GYZj + GZYj) + e.d[44]*GZZj))*detJt*detJt;

				ke[i3+2][j3  ] += (GXXi*(e.d[2]*GXXj + e.d[4]*(GXYj + GYXj) + e.d[5]*(GXZj + GZXj) + e.d[17]*GYYj + e.d[18]*(GYZj + GZYj) + e.d[19]*GZZj)
					            + (GXYi + GYXi)*(e.d[4]*GXXj + e.d[11]*(GXYj + GYXj) + e.d[18]*(GXZj + GZXj) + e.d[14]*GYYj + e.d[21]*(GYZj + GZYj) + e.d[22]*GZZj)
								+ (GXZi + GZXi)*(e.d[5]*GXXj + e.d[18]*(GXYj + GYXj) + e.d[19]*(GXZj + GZXj) + e.d[24]*GYYj + e.d[22]*(GYZj + GZYj) + e.d[23]*GZZj)
								 + GYYi*(e.d[17]*GXXj + e.d[14]*(GXYj + GYXj) + e.d[24]*(GXZj + GZXj) + e.d[26]*GYYj + e.d[31]*(GYZj + GZYj) + e.d[35]*GZZj)
								+ (GYZi + GZYi)*(e.d[18]*GXXj + e.d[21]*(GXYj + GYXj) + e.d[22]*(GXZj + GZXj) + e.d[31]*GYYj + e.d[35]*(GYZj + GZYj) + e.d[36]*GZZj)
								 + GZZi*(e.d[19]*GXXj + e.d[22]*(GXYj + GYXj) + e.d[23]*(GXZj + GZXj) + e.d[35]*GYYj + e.d[36]*(GYZj + GZYj) + e.d[37]*GZZj))*detJt*detJt;

				ke[i3+2][j3+1] += (GXXi*(e.d[4]*GXXj + e.d[17]*(GXYj + GYXj) + e.d[18]*(GXZj + GZXj) + e.d[14]*GYYj + e.d[21]*(GYZj + GZYj) + e.d[22]*GZZj)
					            + (GXYi + GYXi)*(e.d[11]*GXXj + e.d[14]*(GXYj + GYXj) + e.d[21]*(GXZj + GZXj) + e.d[26]*GYYj + e.d[31]*(GYZj + GZYj) + e.d[35]*GZZj)
								+ (GXZi + GZXi)*(e.d[18]*GXXj + e.d[24]*(GXYj + GYXj) + e.d[22]*(GXZj + GZXj) + e.d[31]*GYYj + e.d[35]*(GYZj + GZYj) + e.d[36]*GZZj)
								 + GYYi*(e.d[14]*GXXj + e.d[26]*(GXYj + GYXj) + e.d[31]*(GXZj + GZXj) + e.d[39]*GYYj + e.d[40]*(GYZj + GZYj) + e.d[42]*GZZj)
								+ (GYZi + GZYi)*(e.d[21]*GXXj + e.d[31]*(GXYj + GYXj) + e.d[35]*(GXZj + GZXj) + e.d[40]*GYYj + e.d[42]*(GYZj + GZYj) + e.d[43]*GZZj)
								 + GZZi*(e.d[22]*GXXj + e.d[35]*(GXYj + GYXj) + e.d[36]*(GXZj + GZXj) + e.d[42]*GYYj + e.d[43]*(GYZj + GZYj) + e.d[44]*GZZj))*detJt*detJt;

				ke[i3+2][j3+2] += (GXXi*(e.d[5]*GXXj + e.d[18]*(GXYj + GYXj) + e.d[19]*(GXZj + GZXj) + e.d[21]*GYYj + e.d[22]*(GYZj + GZYj) + e.d[23]*GZZj)
					            + (GXYi + GYXi)*(e.d[18]*GXXj + e.d[21]*(GXYj + GYXj) + e.d[22]*(GXZj + GZXj) + e.d[31]*GYYj + e.d[35]*(GYZj + GZYj) + e.d[33]*GZZj)
								+ (GXZi + GZXi)*(e.d[19]*GXXj + e.d[22]*(GXYj + GYXj) + e.d[23]*(GXZj + GZXj) + e.d[35]*GYYj + e.d[36]*(GYZj + GZYj) + e.d[37]*GZZj)
								 + GYYi*(e.d[24]*GXXj + e.d[31]*(GXYj + GYXj) + e.d[35]*(GXZj + GZXj) + e.d[40]*GYYj + e.d[42]*(GYZj + GZYj) + e.d[43]*GZZj)
								+ (GYZi + GZYi)*(e.d[22]*GXXj + e.d[35]*(GXYj + GYXj) + e.d[36]*(GXZj + GZXj) + e.d[42]*GYYj + e.d[43]*(GYZj + GZYj) + e.d[44]*GZZj)
								 + GZZi*(e.d[23]*GXXj + e.d[36]*(GXYj + GYXj) + e.d[37]*(GXZj + GZXj) + e.d[43]*GYYj + e.d[44]*(GYZj + GZYj) + e.d[45]*GZZj))*detJt*detJt;

			}
		}
	}
}

//-----------------------------------------------------------------------------
//! Calculate the gradient of deformation gradient of element el at integration point n.
//! The gradient of the deformation gradient is returned in G.
void FEElasticMultiscaleDomain2O::defhess(FESolidElement &el, int n, tens3drs &G)
{
	int neln = el.Nodes();

	// get the nodal positions
	vec3d X[FEElement::MAX_NODES];
	vec3d x[FEElement::MAX_NODES];
	FEMesh& mesh = *GetMesh();
	for (int i=0; i<neln; ++i)
	{
		X[i] = mesh.Node(el.m_node[i]).m_r0;
		x[i] = mesh.Node(el.m_node[i]).m_rt;
	}

	// we need the Jacobian with respect to the reference configuration
	double Ji[3][3];
	invjac0(el, Ji, n);

	// shape function derivatives
	double* Gr = el.Gr(n);
	double* Gs = el.Gs(n);
	double* Gt = el.Gt(n);

	double *Grrn = el.Grr(n); double *Grsn = el.Grs(n); double *Grtn = el.Grt(n);
	double *Gsrn = el.Gsr(n); double *Gssn = el.Gss(n); double *Gstn = el.Gst(n);
	double *Gtrn = el.Gtr(n); double *Gtsn = el.Gts(n); double *Gttn = el.Gtt(n);

	// calculate K = dJ/dr
	double K[3][3][3] = {0};
	for (int a=0; a<neln; ++a)
	{
		// second derivatives of shape functions
		double G2[3][3];
		G2[0][0] = Grrn[a]; G2[0][1] = Grsn[a]; G2[0][2] = Grtn[a];
		G2[1][0] = Gsrn[a]; G2[1][1] = Gssn[a]; G2[1][2] = Gstn[a];
		G2[2][0] = Gtrn[a]; G2[2][1] = Gtsn[a]; G2[2][2] = Gttn[a];

		for (int j=0; j<3; ++j)
			for (int k=0; k<3; ++k)
			{
				K[0][j][k] += G2[j][k]*X[a].x;
				K[1][j][k] += G2[j][k]*X[a].y;
				K[2][j][k] += G2[j][k]*X[a].z;
			}
	}

	// calculate A = -J^-1*dJ/drJ^-1
	double A[3][3][3] = {0};
	for (int i=0; i<3; ++i)
		for (int j=0; j<3; ++j)
		{
			for (int p=0; p<3; ++p)
				for (int q=0; q<3; ++q)
				{
					A[i][j][0] -= Ji[j][p]*K[p][q][0]*Ji[q][i];
					A[i][j][1] -= Ji[j][p]*K[p][q][1]*Ji[q][i];
					A[i][j][2] -= Ji[j][p]*K[p][q][2]*Ji[q][i];
				}
		}

	// loop over nodes
	G.zero();
	for (int a=0; a<neln; ++a)
	{
		// first derivative of shape functions
		double G1[3];
		G1[0] = Gr[a];
		G1[1] = Gs[a];
		G1[2] = Gt[a];

		// second derivatives of shape functions
		double G2[3][3];
		G2[0][0] = Grrn[a]; G2[0][1] = Grsn[a]; G2[0][2] = Grtn[a];
		G2[1][0] = Gsrn[a]; G2[1][1] = Gssn[a]; G2[1][2] = Gstn[a];
		G2[2][0] = Gtrn[a]; G2[2][1] = Gtsn[a]; G2[2][2] = Gttn[a];

		// calculate dB/dr
		double D[3][3] = {0};
		for (int i=0; i<3; ++i)
			for (int k=0; k<3; ++k)
			{
				for (int j=0; j<3; ++j) D[i][k] += A[i][j][k]*G1[j] + Ji[j][i]*G2[j][k];
			}

		// calculate global gradient of shape functions
		double H[3][3] = {0};
		for (int i=0; i<3; ++i)
			for (int j=0; j<3; ++j)
			{
				H[i][j] += D[i][0]*Ji[0][j] + D[i][1]*Ji[1][j] + D[i][2]*Ji[2][j];
			}

		// calculate gradient of deformation gradient
		// Note that k >= j. Since tensdrs has symmetries this
		// prevents overwriting of symmetric components
		for (int j=0; j<3; ++j)
			for (int k=j; k<3; ++k)
			{
				G(0,j,k) += H[j][k]*x[a].x;
				G(1,j,k) += H[j][k]*x[a].y;
				G(2,j,k) += H[j][k]*x[a].z;
			}
	}
}

//-----------------------------------------------------------------------------
//! Calculate the gradient of deformation gradient of element el at integration point n.
//! The gradient of the deformation gradient is returned in G.
void FEElasticMultiscaleDomain2O::defhess(FESolidElement &el, double r, double s, double t, tens3drs &G)
{
	int neln = el.Nodes();

	// get the nodal positions
	vec3d X[FEElement::MAX_NODES];
	vec3d x[FEElement::MAX_NODES];
	FEMesh& mesh = *GetMesh();
	for (int i=0; i<neln; ++i)
	{
		X[i] = mesh.Node(el.m_node[i]).m_r0;
		x[i] = mesh.Node(el.m_node[i]).m_rt;
	}

	// we need the Jacobian with respect to the reference configuration
	double Ji[3][3];
	invjac0(el, Ji, r, s, t);

	// shape function derivatives
	const int M = FEElement::MAX_NODES;
	double Gr[M], Gs[M], Gt[M];
	el.shape_deriv(Gr, Gs, Gt, r, s, t);

	double Grr[M], Gss[M], Gtt[M], Grs[M], Gst[M], Grt[M];
	el.shape_deriv2(Grr, Gss, Gtt, Grs, Gst, Grt, r, s, t);

	// calculate K = dJ/dr
	double K[3][3][3] = {0};
	for (int a=0; a<neln; ++a)
	{
		// second derivatives of shape functions
		double G2[3][3];
		G2[0][0] = Grr[a]; G2[0][1] = Grs[a]; G2[0][2] = Grt[a];
		G2[1][0] = Grs[a]; G2[1][1] = Gss[a]; G2[1][2] = Gst[a];
		G2[2][0] = Grt[a]; G2[2][1] = Gst[a]; G2[2][2] = Gtt[a];

		for (int j=0; j<3; ++j)
			for (int k=0; k<3; ++k)
			{
				K[0][j][k] += G2[j][k]*X[a].x;
				K[1][j][k] += G2[j][k]*X[a].y;
				K[2][j][k] += G2[j][k]*X[a].z;
			}
	}

	// calculate A = -J^-1*dJ/drJ^-1
	double A[3][3][3] = {0};
	for (int i=0; i<3; ++i)
		for (int j=0; j<3; ++j)
		{
			for (int p=0; p<3; ++p)
				for (int q=0; q<3; ++q)
				{
					A[i][j][0] -= Ji[j][p]*K[p][q][0]*Ji[q][i];
					A[i][j][1] -= Ji[j][p]*K[p][q][1]*Ji[q][i];
					A[i][j][2] -= Ji[j][p]*K[p][q][2]*Ji[q][i];
				}
		}

	// loop over nodes
	G.zero();
	for (int a=0; a<neln; ++a)
	{
		// first derivative of shape functions
		double G1[3];
		G1[0] = Gr[a];
		G1[1] = Gs[a];
		G1[2] = Gt[a];

		// second derivatives of shape functions
		double G2[3][3];
		G2[0][0] = Grr[a]; G2[0][1] = Grs[a]; G2[0][2] = Grt[a];
		G2[1][0] = Grs[a]; G2[1][1] = Gss[a]; G2[1][2] = Gst[a];
		G2[2][0] = Grt[a]; G2[2][1] = Gst[a]; G2[2][2] = Gtt[a];

		// calculate dB/dr
		double D[3][3] = {0};
		for (int i=0; i<3; ++i)
			for (int k=0; k<3; ++k)
			{
				for (int j=0; j<3; ++j) D[i][k] += A[i][j][k]*G1[j] + Ji[j][i]*G2[j][k];
			}

		// calculate global gradient of shape functions
		double H[3][3] = {0};
		for (int i=0; i<3; ++i)
			for (int j=0; j<3; ++j)
			{
				H[i][j] += D[i][0]*Ji[0][j] + D[i][1]*Ji[1][j] + D[i][2]*Ji[2][j];
			}

		// calculate gradient of deformation gradient
		// Note that k >= j. Since tensdrs has symmetries this
		// prevents overwriting of symmetric components
		for (int j=0; j<3; ++j)
			for (int k=j; k<3; ++k)
			{
				G(0,j,k) += H[j][k]*x[a].x;
				G(1,j,k) += H[j][k]*x[a].y;
				G(2,j,k) += H[j][k]*x[a].z;
			}
	}
}
