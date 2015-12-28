//
//  FEReactiveViscoelastic.cpp
//  FEBioMech
//
//  Created by Gerard Ateshian on 8/25/14.
//  Copyright (c) 2014 febio.org. All rights reserved.
//

#include "FEReactiveViscoelastic.h"
#include "FECore/FECoreKernel.h"
#include <limits>

///////////////////////////////////////////////////////////////////////////////
//
// FEReactiveViscoelasticMaterial
//
///////////////////////////////////////////////////////////////////////////////

// Material parameters for the FEMultiphasic material
BEGIN_PARAMETER_LIST(FEReactiveViscoelasticMaterial, FEElasticMaterial)
    ADD_PARAMETER2(m_wmin , FE_PARAM_DOUBLE, FE_RANGE_CLOSED(0.0, 1.0), "wmin");
    ADD_PARAMETER2(m_btype, FE_PARAM_INT   , FE_RANGE_CLOSED(1,2), "kinetics");
    ADD_PARAMETER2(m_ttype, FE_PARAM_INT   , FE_RANGE_CLOSED(0,2), "trigger");
END_PARAMETER_LIST();

//-----------------------------------------------------------------------------
//! constructor
FEReactiveViscoelasticMaterial::FEReactiveViscoelasticMaterial(FEModel* pfem) : FEElasticMaterial(pfem)
{
    m_wmin = 0;
    m_btype = 0;
    m_ttype = 0;

	// set material properties
	AddProperty(&m_pBase, "elastic"   );
	AddProperty(&m_pBond, "bond"      );
	AddProperty(&m_pRelx, "relaxation");
}

//-----------------------------------------------------------------------------
void FEReactiveViscoelasticMaterial::SetLocalCoordinateSystem(FEElement& el, int n, FEMaterialPoint& mp)
{
	FEElasticMaterial::SetLocalCoordinateSystem(el, n, mp);
	FEElasticMaterial* pme = GetBaseMaterial();
	pme->SetLocalCoordinateSystem(el, n, mp);
	FEElasticMaterial* pmb = GetBondMaterial();
	pmb->SetLocalCoordinateSystem(el, n, mp);
}

//-----------------------------------------------------------------------------
//! data initialization
bool FEReactiveViscoelasticMaterial::Init()
{
    FEUncoupledMaterial* m_pMat = dynamic_cast<FEUncoupledMaterial*>((FEElasticMaterial*)m_pBase);
    if (m_pMat != nullptr)
        return MaterialError("Elastic material should not be of type uncoupled");
    
    m_pMat = dynamic_cast<FEUncoupledMaterial*>((FEElasticMaterial*)m_pBond);
    if (m_pMat != nullptr)
        return MaterialError("Bond material should not be of type uncoupled");
    
    if (m_pBase->Init() == false) return false;
    if (m_pBond->Init() == false) return false;
    if (m_pRelx->Init() == false) return false;
    
    return FEElasticMaterial::Init();
}

//-----------------------------------------------------------------------------
//! Create material point data for this material
FEMaterialPoint* FEReactiveViscoelasticMaterial::CreateMaterialPointData()
{
	return new FEReactiveVEMaterialPoint(m_pBase->CreateMaterialPointData(), this);
}

//-----------------------------------------------------------------------------
//! detect new generation
bool FEReactiveViscoelasticMaterial::NewGeneration(FEMaterialPoint& mp)
{
    double d;
    double eps = std::numeric_limits<double>::epsilon();

    // get the elastic material poit data
    FEElasticMaterialPoint& pe = *mp.ExtractData<FEElasticMaterialPoint>();
    
    // get the reactive viscoelastic point data
    FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();
    
    // check if the current deformation gradient is different from that of
    // the last generation, in which case store the current state
    // evaluate the relative deformation gradient
    mat3d F = pe.m_F;
    int lg = (int)pt.m_Fi.size() - 1;
    mat3d Fi = (lg > -1) ? pt.m_Fi[lg] : mat3d(mat3dd(1));
    mat3d Fu = F*Fi;

    switch (m_ttype) {
        case 0:
        {
            // trigger in response to any strain
            // evaluate the Lagrangian strain
            mat3ds E = ((Fu.transpose()*Fu).sym() - mat3dd(1))/2;
            
            d = E.norm();
        }
            break;
        case 1:
        {
            // trigger in response to distortional strain
            // evaluate spatial Hencky (logarithmic) strain
            mat3ds Bu = (Fu*Fu.transpose()).sym();
            double l[3];
            vec3d v[3];
            Bu.eigen2(l,v);
            mat3ds h = (dyad(v[0])*log(l[0]) + dyad(v[1])*log(l[1]) + dyad(v[2])*log(l[2]))/2;
            
            // evaluate distortion magnitude (always positive)
            d = (h.dev()).norm();
        }
            break;
        case 2:
        {
            // trigger in response to dilatational strain
            d = fabs(log(Fu.det()));
        }
            break;
            
        default:
            d = 0;
            break;
    }
    
    if (d > eps) return true;
    
    return false;
}

//-----------------------------------------------------------------------------
//! evaluate bond mass fraction
double FEReactiveViscoelasticMaterial::BreakingBondMassFraction(FEMaterialPoint& mp, const int ig)
{
    // get the reactive viscoelastic point data
    FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();
    
    // bond mass fraction
    double w = 0;
    
    // current time
    double time = FEMaterialPoint::time;
    
    switch (m_btype) {
        case 1:
        {
            // time when this generation started breaking
            double v = pt.m_v[ig];
            
            if (time >= v)
                w = pt.m_w[ig]*m_pRelx->Relaxation(mp, time - v);
        }
            break;
        case 2:
        {
            double tu, tv;
            if (ig == 0) {
                tv = time - pt.m_v[ig];
                w = m_pRelx->Relaxation(mp, tv);
            }
            else
            {
                tu = time - pt.m_v[ig-1];
                tv = time - pt.m_v[ig];
                w = m_pRelx->Relaxation(mp, tv) - m_pRelx->Relaxation(mp, tu);
            }
        }
            break;
            
        default:
            break;
    }
    
    assert((w >= 0) && (w <= 1));
    
    return w;
}

//-----------------------------------------------------------------------------
//! evaluate bond mass fraction of reforming generation
double FEReactiveViscoelasticMaterial::ReformingBondMassFraction(FEMaterialPoint& mp)
{
    // get the elastic part
    FEElasticMaterialPoint& ep = *mp.ExtractData<FEElasticMaterialPoint>();
    
    // get the reactive viscoelastic point data
    FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();
    
    // keep safe copy of deformation gradient
    mat3d F = ep.m_F;
    double J = ep.m_J;
    
    // get current number of generations
    int ng = (int)pt.m_Fi.size();
    
    double w = 1;
    
    for (int ig=0; ig<ng-1; ++ig)
    {
        // evaluate relative deformation gradient for this generation Fu(v)
        ep.m_F = pt.m_Fi[ig+1].inverse()*pt.m_Fi[ig];
        ep.m_J = pt.m_Ji[ig]/pt.m_Ji[ig+1];
        // evaluate the breaking bond mass fraction for this generation
        w -= BreakingBondMassFraction(mp, ig);
    }
    
    // restore safe copy of deformation gradient
    ep.m_F = F;
    ep.m_J = J;
    
    assert((w >= 0) && (w <= 1));
    
    // return the bond mass fraction of the reforming generation
    return w;
}

//-----------------------------------------------------------------------------
//! Stress function
mat3ds FEReactiveViscoelasticMaterial::Stress(FEMaterialPoint& mp)
{
   if (mp.dt == 0) return mat3ds(0,0,0,0,0,0);
    
	// get the elastic part
	FEElasticMaterialPoint& ep = *mp.ExtractData<FEElasticMaterialPoint>();
    
	// get the reactive viscoelastic point data
	FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();
    
	// calculate the base material Cauchy stress
	mat3ds s = m_pBase->Stress(mp);
    
    // current number of breaking generations
    int ng = (int)pt.m_Fi.size();
    
    // no bonds have broken
    if (ng == 0) {
        s += m_pBond->Stress(mp);
    }
    // bonds have broken
    else {
        // keep safe copy of deformation gradient
        mat3d F = ep.m_F;
        double J = ep.m_J;
        
        double w;
        mat3ds sb;
        
        // calculate the bond stresses for breaking generations
        for (int ig=0; ig<ng; ++ig) {
            // evaluate relative deformation gradient for this generation
            ep.m_F = F*pt.m_Fi[ig];
            ep.m_J = J*pt.m_Ji[ig];
            // evaluate bond mass fraction for this generation
            w = BreakingBondMassFraction(mp, ig);
            // evaluate bond stress
            sb = m_pBond->Stress(mp);
            // add bond stress to total stress
            s += sb*(w*pt.m_Ji[ig]);
        }
        
        // restore safe copy of deformation gradient
        ep.m_F = F;
        ep.m_J = J;
    }
    
	// return the total Cauchy stress
	return s;
}

//-----------------------------------------------------------------------------
//! Material tangent
tens4ds FEReactiveViscoelasticMaterial::Tangent(FEMaterialPoint& mp)
{
    CullGenerations(mp);
    
	// get the elastic part
	FEElasticMaterialPoint& ep = *mp.ExtractData<FEElasticMaterialPoint>();
    
	// get the reactive viscoelastic point data
	FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();
    
	// calculate the base material tangent
	tens4ds c = m_pBase->Tangent(mp);
    
    // current number of breaking generations
    int ng = (int)pt.m_Fi.size();
    
    // no bonds have broken
    if (ng == 0) {
        c += m_pBond->Tangent(mp);
    }
    // bonds have broken
    else {
        // keep safe copy of deformation gradient
        mat3d F = ep.m_F;
        double J = ep.m_J;
        
        double w;
        tens4ds cb;
        
        // calculate the bond tangents for breaking generations
        for (int ig=0; ig<ng; ++ig) {
            // evaluate relative deformation gradient for this generation
            ep.m_F = F*pt.m_Fi[ig];
            ep.m_J = J*pt.m_Ji[ig];
            // evaluate bond mass fraction for this generation
            w = BreakingBondMassFraction(mp, ig);
            // evaluate bond tangent
            cb = m_pBond->Tangent(mp);
            // add bond tangent to total tangent
            c += cb*(w*pt.m_Ji[ig]);
        }
        
        // restore safe copy of deformation gradient
        ep.m_F = F;
        ep.m_J = J;
    }
    
	// return the total tangent
	return c;
}

//-----------------------------------------------------------------------------
//! strain energy density function
double FEReactiveViscoelasticMaterial::StrainEnergyDensity(FEMaterialPoint& mp)
{
    if (mp.dt == 0) return 0;
    
    // get the elastic part
    FEElasticMaterialPoint& ep = *mp.ExtractData<FEElasticMaterialPoint>();
    
    // get the reactive viscoelastic point data
    FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();
    
    // calculate the base material Cauchy stress
    double sed = m_pBase->StrainEnergyDensity(mp);
    
    // current number of breaking generations
    int ng = (int)pt.m_Fi.size();
    
    // no bonds have broken
    if (ng == 0) {
        sed += m_pBond->StrainEnergyDensity(mp);
    }
    // bonds have broken
    else {
        // keep safe copy of deformation gradient
        mat3d F = ep.m_F;
        double J = ep.m_J;
        
        double w;
        double sedb;
        
        // calculate the strain energy density for breaking generations
        for (int ig=0; ig<ng; ++ig) {
            // evaluate relative deformation gradient for this generation
            ep.m_F = F*pt.m_Fi[ig];
            ep.m_J = J*pt.m_Ji[ig];
            // evaluate bond mass fraction for this generation
            w = BreakingBondMassFraction(mp, ig);
            // evaluate bond stress
            sedb = m_pBond->StrainEnergyDensity(mp);
            // add bond stress to total stress
            sed += sedb*w;
        }
        
        // restore safe copy of deformation gradient
        ep.m_F = F;
        ep.m_J = J;
    }
    
    // return the total Cauchy stress
    return sed;
}

//-----------------------------------------------------------------------------
//! Cull generations that have relaxed below a threshold
void FEReactiveViscoelasticMaterial::CullGenerations(FEMaterialPoint& mp)
{
    // get the reactive viscoelastic point data
    FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();
    
    if (pt.m_Fi.empty()) return;

    // culling termination flag
    bool done = false;
    
    // always check oldest generation
    while (!done) {
        double w = BreakingBondMassFraction(mp, 0);
        if ((w > m_wmin) || (pt.m_Fi.size() == 1))
            done = true;
        else {
            pt.m_Fi.pop_front();
            pt.m_Ji.pop_front();
            pt.m_v.pop_front();
            pt.m_w.pop_front();
        }
    }
    
    return;
}
