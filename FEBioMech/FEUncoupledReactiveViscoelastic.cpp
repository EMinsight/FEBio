/*This file is part of the FEBio source code and is licensed under the MIT license
listed below.

See Copyright-FEBio.txt for details.

Copyright (c) 2020 University of Utah, The Trustees of Columbia University in 
the City of New York, and others.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.*/



#include "stdafx.h"
#include "FEUncoupledReactiveViscoelastic.h"
#include "FECore/FECoreKernel.h"
#include <FECore/FEModel.h>
#include <limits>

///////////////////////////////////////////////////////////////////////////////
//
// FEUncoupledReactiveViscoelasticMaterial
//
///////////////////////////////////////////////////////////////////////////////

// Material parameters for the FEUncoupledReactiveViscoelastic material
BEGIN_FECORE_CLASS(FEUncoupledReactiveViscoelasticMaterial, FEUncoupledMaterial)
	ADD_PARAMETER(m_wmin , FE_RANGE_CLOSED(0.0, 1.0), "wmin"    );
	ADD_PARAMETER(m_btype, FE_RANGE_CLOSED(1, 2), "kinetics");
	ADD_PARAMETER(m_ttype, FE_RANGE_CLOSED(0, 2), "trigger" );
    ADD_PARAMETER(m_emin , FE_RANGE_GREATER_OR_EQUAL(0.0), "emin");

	// set material properties
	ADD_PROPERTY(m_pBase, "elastic");
	ADD_PROPERTY(m_pBond, "bond");
	ADD_PROPERTY(m_pRelx, "relaxation");

END_FECORE_CLASS();

//-----------------------------------------------------------------------------
//! constructor
FEUncoupledReactiveViscoelasticMaterial::FEUncoupledReactiveViscoelasticMaterial(FEModel* pfem) : FEUncoupledMaterial(pfem)
{
    m_wmin = 0;
    m_btype = 0;
    m_ttype = 0;
    m_emin = 0;

    m_nmax = 0;

	m_pBase = 0;
	m_pBond = 0;
	m_pRelx = 0;
}

//-----------------------------------------------------------------------------
//! Create material point data for this material
FEMaterialPoint* FEUncoupledReactiveViscoelasticMaterial::CreateMaterialPointData()
{
    return new FEReactiveVEMaterialPoint(m_pBase->CreateMaterialPointData(), this);
}

//-----------------------------------------------------------------------------
//! detect new generation
bool FEUncoupledReactiveViscoelasticMaterial::NewGeneration(FEMaterialPoint& mp)
{
    double d;
    double eps = max(m_emin, 10*std::numeric_limits<double>::epsilon());

    // get the elastic material point data
    FEElasticMaterialPoint& pe = *mp.ExtractData<FEElasticMaterialPoint>();
    
    // get the reactive viscoelastic point data
    FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();
    
    // check if the current deformation gradient is different from that of
    // the last generation, in which case store the current state
    // evaluate the relative deformation gradient
    mat3d F = pe.m_F;
    int lg = (int)pt.m_Uv.size() - 1;
    mat3ds Ui = (lg > -1) ? pt.m_Uv[lg].inverse() : mat3dd(1);
    mat3d Fu = F*Ui;
    
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
double FEUncoupledReactiveViscoelasticMaterial::BreakingBondMassFraction(FEMaterialPoint& mp, const int ig, const mat3ds D)
{
    // get the reactive viscoelastic point data
    FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();
    
    // bond mass fraction
    double w = 0;
    
    // current time
    double time = GetFEModel()->GetTime().currentTime;
    double tv = time - pt.m_v[ig];

    switch (m_btype) {
        case 1:
        {
            if (tv >= 0)
                w = pt.m_f[ig]*m_pRelx->Relaxation(mp, tv, D);
        }
            break;
        case 2:
        {
            if (ig == 0) {
                w = m_pRelx->Relaxation(mp, tv, D);
            }
            else
            {
                double tu = time - pt.m_v[ig-1];
                w = m_pRelx->Relaxation(mp, tv, D) - m_pRelx->Relaxation(mp, tu, D);
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
double FEUncoupledReactiveViscoelasticMaterial::ReformingBondMassFraction(FEMaterialPoint& mp)
{
    // get the elastic part
    FEElasticMaterialPoint& ep = *mp.ExtractData<FEElasticMaterialPoint>();
    
    // get the reactive viscoelastic point data
    FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();
    
    mat3ds D = ep.RateOfDeformation();
    
    // keep safe copy of deformation gradient
    mat3d F = ep.m_F;
    double J = ep.m_J;
    
    // get current number of generations
    int ng = (int)pt.m_Uv.size();
    
    double w = 1;
    
    for (int ig=0; ig<ng-1; ++ig)
    {
        // evaluate deformation gradient when this generation starts breaking
        ep.m_F = pt.m_Uv[ig];
        ep.m_J = pt.m_Jv[ig];
        // evaluate the breaking bond mass fraction for this generation
        w -= BreakingBondMassFraction(mp, ig, D);
    }
    
    // restore safe copy of deformation gradient
    ep.m_F = F;
    ep.m_J = J;
    
    assert((w >= 0) && (w <= 1));
    
    // return the bond mass fraction of the reforming generation
    return w;
}

//-----------------------------------------------------------------------------
//! Stress function in strong bonds
mat3ds FEUncoupledReactiveViscoelasticMaterial::DevStressStrongBonds(FEMaterialPoint& mp)
{
    // calculate the base material Cauchy stress
    return m_pBase->DevStress(mp);
}

//-----------------------------------------------------------------------------
//! Stress function in weak bonds
mat3ds FEUncoupledReactiveViscoelasticMaterial::DevStressWeakBonds(FEMaterialPoint& mp)
{
    double dt = GetFEModel()->GetTime().timeIncrement;
    if (dt == 0) return mat3ds(0, 0, 0, 0, 0, 0);
    
    // get the elastic part
    FEElasticMaterialPoint& ep = *mp.ExtractData<FEElasticMaterialPoint>();
    
    // get the reactive viscoelastic point data
    FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();
    
    mat3ds D = ep.RateOfDeformation();
    
    // calculate the base material Cauchy stress
    mat3ds s; s.zero();
    
    // current number of breaking generations
    int ng = (int)pt.m_Uv.size();
    
    // no bonds have broken
    if (ng == 0) {
        s += m_pBond->DevStress(mp);
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
            // evaluate bond mass fraction for this generation
            ep.m_F = pt.m_Uv[ig];
            ep.m_J = pt.m_Jv[ig];
            w = BreakingBondMassFraction(mp, ig, D);
            // evaluate relative deformation gradient for this generation
            ep.m_F = (ig > 0) ? F*pt.m_Uv[ig-1].inverse() : F;
            ep.m_J = (ig > 0) ? J/pt.m_Jv[ig-1] : J;
            // evaluate bond stress
            sb = m_pBond->DevStress(mp);
            // add bond stress to total stress
            s += (ig > 0) ? sb*w/pt.m_Jv[ig-1] : sb*w;
        }
        
        // restore safe copy of deformation gradient
        ep.m_F = F;
        ep.m_J = J;
    }
    
    return s;
}

//-----------------------------------------------------------------------------
//! Stress function
mat3ds FEUncoupledReactiveViscoelasticMaterial::DevStress(FEMaterialPoint& mp)
{
    // calculate the base material Cauchy stress
    mat3ds s = DevStressStrongBonds(mp);
    s+= DevStressWeakBonds(mp);
    
    // return the total Cauchy stress
    return s;
}

//-----------------------------------------------------------------------------
//! Material tangent in strong bonds
tens4ds FEUncoupledReactiveViscoelasticMaterial::DevTangentStrongBonds(FEMaterialPoint& mp)
{
    // calculate the base material tangent
    return m_pBase->DevTangent(mp);
}

//-----------------------------------------------------------------------------
//! Material tangent in weak bonds
tens4ds FEUncoupledReactiveViscoelasticMaterial::DevTangentWeakBonds(FEMaterialPoint& mp)
{
    // get the elastic part
    FEElasticMaterialPoint& ep = *mp.ExtractData<FEElasticMaterialPoint>();
    
    // get the reactive viscoelastic point data
    FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();
    
    mat3ds D = ep.RateOfDeformation();
    
    // calculate the base material tangent
    tens4ds c; c.zero();
    
    // current number of breaking generations
    int ng = (int)pt.m_Uv.size();
    
    // no bonds have broken
    if (ng == 0) {
        c += m_pBond->DevTangent(mp);
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
            // evaluate bond mass fraction for this generation
            ep.m_F = pt.m_Uv[ig];
            ep.m_J = pt.m_Jv[ig];
            w = BreakingBondMassFraction(mp, ig, D);
            // evaluate relative deformation gradient for this generation
            ep.m_F = (ig > 0) ? F*pt.m_Uv[ig-1].inverse() : F;
            ep.m_J = (ig > 0) ? J/pt.m_Jv[ig-1] : J;
            // evaluate bond tangent
            cb = m_pBond->DevTangent(mp);
            // add bond tangent to total tangent
            c += (ig > 0) ? cb*w/pt.m_Jv[ig-1] : cb*w;
        }
        
        // restore safe copy of deformation gradient
        ep.m_F = F;
        ep.m_J = J;
    }
    
    return c;
}

//-----------------------------------------------------------------------------
//! Material tangent
tens4ds FEUncoupledReactiveViscoelasticMaterial::DevTangent(FEMaterialPoint& mp)
{
    tens4ds c = DevTangentStrongBonds(mp);
    c+= DevTangentWeakBonds(mp);
    
    // return the total tangent
    return c;
}

//-----------------------------------------------------------------------------
//! strain energy density function for weak bonds
double FEUncoupledReactiveViscoelasticMaterial::DevStrainEnergyDensityStrongBonds(FEMaterialPoint& mp)
{
    // calculate the base material strain energy density
    return m_pBase->DevStrainEnergyDensity(mp);
}

//-----------------------------------------------------------------------------
//! strain energy density function
double FEUncoupledReactiveViscoelasticMaterial::DevStrainEnergyDensityWeakBonds(FEMaterialPoint& mp)
{
    double dt = GetFEModel()->GetTime().timeIncrement;
    if (dt == 0) return 0;
    
    // get the elastic part
    FEElasticMaterialPoint& ep = *mp.ExtractData<FEElasticMaterialPoint>();
    
    // get the reactive viscoelastic point data
    FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();
    
    mat3ds D = ep.RateOfDeformation();
    
    double sed = 0;
    
    // current number of breaking generations
    int ng = (int)pt.m_Uv.size();
    
    // no bonds have broken
    if (ng == 0) {
        sed += m_pBond->DevStrainEnergyDensity(mp);
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
            // evaluate bond mass fraction for this generation
            ep.m_F = pt.m_Uv[ig];
            ep.m_J = pt.m_Jv[ig];
            w = BreakingBondMassFraction(mp, ig, D);
            // evaluate relative deformation gradient for this generation
            ep.m_F = (ig > 0) ? F*pt.m_Uv[ig-1].inverse() : F;
            ep.m_J = (ig > 0) ? J/pt.m_Jv[ig-1] : J;
            // evaluate bond stress
            sedb = m_pBond->DevStrainEnergyDensity(mp);
            // add bond stress to total stress
            sed += sedb*w;
        }
        
        // restore safe copy of deformation gradient
        ep.m_F = F;
        ep.m_J = J;
    }
    
    return sed;
}

//-----------------------------------------------------------------------------
//! strain energy density function
double FEUncoupledReactiveViscoelasticMaterial::DevStrainEnergyDensity(FEMaterialPoint& mp)
{
    double sed = DevStrainEnergyDensityStrongBonds(mp);
    sed += DevStrainEnergyDensityWeakBonds(mp);
    
    // return the total strain energy density
    return sed;
}

//-----------------------------------------------------------------------------
//! Cull generations that have relaxed below a threshold
void FEUncoupledReactiveViscoelasticMaterial::CullGenerations(FEMaterialPoint& mp)
{
    // get the elastic part
    FEElasticMaterialPoint& ep = *mp.ExtractData<FEElasticMaterialPoint>();
    
    // get the reactive viscoelastic point data
    FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();
    
    mat3ds D = ep.RateOfDeformation();
    
    int ng = (int)pt.m_v.size();
    m_nmax = max(m_nmax, ng);
    
    // don't cull if we have too few generations
    if (ng < 3) return;
    
    // don't reduce number of generations to less than max value achieved so far
    if (ng < m_nmax) return;

    // always check oldest generation
    double w0 = BreakingBondMassFraction(mp, 0, D);
    if (w0 < m_wmin) {
        double w1 = BreakingBondMassFraction(mp, 1, D);
        pt.m_v[1] = (w0*pt.m_v[0] + w1*pt.m_v[1])/(w0+w1);
        pt.m_Uv[1] = (pt.m_Uv[0]*w0 + pt.m_Uv[1]*w1)/(w0+w1);
        pt.m_Jv[1] = pt.m_Uv[1].det();
        pt.m_f[1] = (w0*pt.m_f[0] + w1*pt.m_f[1])/(w0+w1);
        pt.m_Uv.pop_front();
        pt.m_Jv.pop_front();
        pt.m_v.pop_front();
        pt.m_f.pop_front();
    }
    
    return;
}

//-----------------------------------------------------------------------------
//! Update specialized material points
void FEUncoupledReactiveViscoelasticMaterial::UpdateSpecializedMaterialPoints(FEMaterialPoint& mp, const FETimeInfo& tp)
{
    // get the reactive viscoelastic point data
    FEReactiveVEMaterialPoint& pt = *mp.ExtractData<FEReactiveVEMaterialPoint>();

    pt.UpdateGenerations(tp);
}
