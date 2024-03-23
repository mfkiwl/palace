// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "errorestimator.hpp"

#include <limits>
#include "fem/bilinearform.hpp"
#include "fem/integrator.hpp"
#include "fem/libceed/ceed.hpp"
#include "fem/libceed/coefficient.hpp"
#include "fem/libceed/integrator.hpp"
#include "linalg/amg.hpp"
#include "linalg/gmg.hpp"
#include "linalg/iterative.hpp"
#include "linalg/jacobi.hpp"
#include "linalg/rap.hpp"
#include "models/materialoperator.hpp"
#include "utils/communication.hpp"
#include "utils/diagnostic.hpp"
#include "utils/omp.hpp"
#include "utils/timer.hpp"

PalacePragmaDiagnosticPush
PalacePragmaDiagnosticDisableUnused

#include "fem/qfunctions/hcurlhdiv_error_qf.h"

PalacePragmaDiagnosticPop

namespace palace
{

namespace
{

template <typename OperType>
auto BuildLevelParOperator(std::unique_ptr<Operator> &&a,
                           const FiniteElementSpace &trial_fespace,
                           const FiniteElementSpace &test_fespace);

template <>
auto BuildLevelParOperator<Operator>(std::unique_ptr<Operator> &&a,
                                     const FiniteElementSpace &trial_fespace,
                                     const FiniteElementSpace &test_fespace)
{
  return std::make_unique<ParOperator>(std::move(a), trial_fespace, test_fespace, false);
}

template <>
auto BuildLevelParOperator<ComplexOperator>(std::unique_ptr<Operator> &&a,
                                            const FiniteElementSpace &trial_fespace,
                                            const FiniteElementSpace &test_fespace)
{
  return std::make_unique<ComplexParOperator>(std::move(a), nullptr, trial_fespace,
                                              test_fespace, false);
}

template <typename OperType>
auto BuildLevelParOperator(std::unique_ptr<Operator> &&a, const FiniteElementSpace &fespace)
{
  return BuildLevelParOperator<OperType>(std::move(a), fespace, fespace);
}

template <typename OperType>
std::unique_ptr<OperType> GetMassMatrix(const FiniteElementSpaceHierarchy &fespaces,
                                        bool use_mg)
{
  constexpr bool skip_zeros = false;
  BilinearForm m(fespaces.GetFinestFESpace());
  m.AddDomainIntegrator<VectorFEMassIntegrator>();
  if (!use_mg)
  {
    return BuildLevelParOperator<OperType>(m.Assemble(skip_zeros),
                                           fespaces.GetFinestFESpace());
  }
  else
  {
    auto m_vec = m.Assemble(fespaces, skip_zeros);
    auto M_mg = std::make_unique<BaseMultigridOperator<OperType>>(fespaces.GetNumLevels());
    for (std::size_t l = 0; l < fespaces.GetNumLevels(); l++)
    {
      const auto &fespace_l = fespaces.GetFESpaceAtLevel(l);
      M_mg->AddOperator(BuildLevelParOperator<OperType>(std::move(m_vec[l]), fespace_l));
    }
    return M_mg;
  }
}

template <typename OperType>
auto ConfigureLinearSolver(const FiniteElementSpaceHierarchy &fespaces, double tol,
                           int max_it, int print, bool use_mg)
{
  // The system matrix for the projection is real, SPD and diagonally dominant.
  std::unique_ptr<Solver<OperType>> pc;
  if (!use_mg)
  {
    // Use eigenvalue estimate to compute optimal Jacobi damping parameter.
    pc = std::make_unique<JacobiSmoother<OperType>>(fespaces.GetFinestFESpace().GetComm(),
                                                    0.0);
  }
  else
  {
    auto amg = std::make_unique<BoomerAmgSolver>(1, 1, 0);
    amg->SetStrengthThresh(0.8);  // More coarsening to save memory
    if (fespaces.GetNumLevels() > 1)
    {
      const int mg_smooth_order = 2;  // Smooth order independent of FE space order
      pc = std::make_unique<GeometricMultigridSolver<OperType>>(
          fespaces.GetFinestFESpace().GetComm(),
          std::make_unique<MfemWrapperSolver<OperType>>(std::move(amg)),
          fespaces.GetProlongationOperators(), nullptr, 1, 1, mg_smooth_order, 1.0, 0.0,
          true);
    }
    else
    {
      pc = std::make_unique<MfemWrapperSolver<OperType>>(std::move(amg));
    }
  }

  auto pcg =
      std::make_unique<CgSolver<OperType>>(fespaces.GetFinestFESpace().GetComm(), print);
  pcg->SetInitialGuess(false);
  pcg->SetRelTol(tol);
  pcg->SetAbsTol(std::numeric_limits<double>::epsilon());
  pcg->SetMaxIter(max_it);
  return std::make_unique<BaseKspSolver<OperType>>(std::move(pcg), std::move(pc));
}

}  // namespace

template <typename VecType>
FluxProjector<VecType>::FluxProjector(const MaterialOperator &mat_op,
                                      const FiniteElementSpaceHierarchy &nd_fespaces,
                                      double tol, int max_it, int print, bool use_mg)
{
  BlockTimer bt(Timer::CONSTRUCT_ESTIMATOR);
  const auto &nd_fespace = nd_fespaces.GetFinestFESpace();
  {
    // Flux operator is always partially assembled.
    MaterialPropertyCoefficient muinv_func(mat_op.GetAttributeToMaterial(),
                                           mat_op.GetInvPermeability());
    BilinearForm flux(nd_fespace);
    flux.AddDomainIntegrator<MixedVectorCurlIntegrator>(muinv_func);
    Flux = BuildLevelParOperator<OperType>(flux.PartialAssemble(), nd_fespace);
  }
  M = GetMassMatrix<OperType>(nd_fespaces, use_mg);
  ksp = ConfigureLinearSolver<OperType>(nd_fespaces, tol, max_it, print, use_mg);
  ksp->SetOperators(*M, *M);

  rhs.SetSize(nd_fespace.GetTrueVSize());
  rhs.UseDevice(true);
}

template <typename VecType>
FluxProjector<VecType>::FluxProjector(const MaterialOperator &mat_op,
                                      const FiniteElementSpace &h1_fespace,
                                      const FiniteElementSpaceHierarchy &rt_fespaces,
                                      double tol, int max_it, int print, bool use_mg)
{
  BlockTimer bt(Timer::CONSTRUCT_ESTIMATOR);
  const auto &rt_fespace = rt_fespaces.GetFinestFESpace();
  {
    // Flux operator is always partially assembled.
    MaterialPropertyCoefficient epsilon_func(mat_op.GetAttributeToMaterial(),
                                             mat_op.GetPermittivityReal());
    BilinearForm flux(h1_fespace, rt_fespace);
    flux.AddDomainIntegrator<MixedVectorGradientIntegrator>(epsilon_func);
    Flux = BuildLevelParOperator<OperType>(flux.PartialAssemble(), h1_fespace, rt_fespace);
  }
  M = GetMassMatrix<OperType>(rt_fespaces, use_mg);
  ksp = ConfigureLinearSolver<OperType>(rt_fespaces, tol, max_it, print, use_mg);
  ksp->SetOperators(*M, *M);

  rhs.SetSize(rt_fespace.GetTrueVSize());
  rhs.UseDevice(true);
}

template <typename VecType>
void FluxProjector<VecType>::Mult(const VecType &x, VecType &y) const
{
  BlockTimer bt(Timer::SOLVE_ESTIMATOR);
  MFEM_ASSERT(y.Size() == rhs.Size(), "Invalid vector dimensions for FluxProjector::Mult!");
  Flux->Mult(x, rhs);
  // Mpi::Print(" Computing smooth flux projection for error estimation\n");
  ksp->Mult(rhs, y);
}

template <typename VecType>
CurlFluxErrorEstimator<VecType>::CurlFluxErrorEstimator(
    const MaterialOperator &mat_op, FiniteElementSpaceHierarchy &nd_fespaces, double tol,
    int max_it, int print, bool use_mg)
  : mat_op(mat_op), nd_fespace(nd_fespaces.GetFinestFESpace()),
    projector(mat_op, nd_fespaces, tol, max_it, print, use_mg), U_gf(nd_fespace.GetVSize()),
    F(nd_fespace.GetTrueVSize()), F_gf(nd_fespace.GetVSize())
{
  U_gf.UseDevice(true);
  F.UseDevice(true);
  F_gf.UseDevice(true);
}

template <typename VecType>
void CurlFluxErrorEstimator<VecType>::AddErrorIndicator(const VecType &U,
                                                        ErrorIndicator &indicator) const
{
  // Compute the projection of the discontinuous flux onto the smooth finite element space
  // and populate the corresponding grid functions.
  BlockTimer bt(Timer::ESTIMATION);
  projector.Mult(U, F);
  if constexpr (std::is_same<VecType, ComplexVector>::value)
  {
    nd_fespace.GetProlongationMatrix()->Mult(U.Real(), U_gf.Real());
    nd_fespace.GetProlongationMatrix()->Mult(U.Imag(), U_gf.Imag());
    nd_fespace.GetProlongationMatrix()->Mult(F.Real(), F_gf.Real());
    nd_fespace.GetProlongationMatrix()->Mult(F.Imag(), F_gf.Imag());
  }
  else
  {
    nd_fespace.GetProlongationMatrix()->Mult(U, U_gf);
    nd_fespace.GetProlongationMatrix()->Mult(F, F_gf);
  }

  // Use libCEED operators to perform the error estimate integration over each element. The
  // discontinuous flux is μ⁻¹ ∇ × U. Output is stored as error integral for all elements
  // first, then scaling integral.
  const auto &mesh = nd_fespace.GetMesh();
  constexpr CeedInt elem_num_comp = 2;
  Vector estimates(elem_num_comp * mesh.GetNE());
  estimates.UseDevice(true);
  estimates = 0.0;
  const std::size_t nt = ceed::internal::GetCeedObjects().size();
  PalacePragmaOmp(parallel if (nt > 1))
  {
    Ceed ceed = ceed::internal::GetCeedObjects()[utils::GetThreadNum()];
    for (const auto &[geom, data] : mesh.GetCeedGeomFactorData(ceed))
    {
      // Only integrate over domain elements (not on the boundary).
      if (mfem::Geometry::Dimension[geom] < mesh.Dimension())
      {
        continue;
      }

      // Create libCEED vector wrappers for use with libCEED operators. Each thread writes
      // to non-overlapping entries of the estimates vector.
      CeedVector estimates_vec, U_gf_vec, F_gf_vec;
      ceed::InitCeedVector(estimates, ceed, &estimates_vec);
      if constexpr (std::is_same<VecType, ComplexVector>::value)
      {
        ceed::InitCeedVector(U_gf.Real(), ceed, &U_gf_vec);
        ceed::InitCeedVector(F_gf.Real(), ceed, &F_gf_vec);
      }
      else
      {
        ceed::InitCeedVector(U_gf, ceed, &U_gf_vec);
        ceed::InitCeedVector(F_gf, ceed, &F_gf_vec);
      }

      // Construct mesh element restriction for elements of this element geometry type.
      CeedElemRestriction mesh_elem_restr;
      PalaceCeedCall(ceed,
                     CeedElemRestrictionCreate(
                         ceed, static_cast<CeedInt>(data.indices.size()), 1, elem_num_comp,
                         mesh.GetNE(), elem_num_comp * mesh.GetNE(), CEED_MEM_HOST,
                         CEED_COPY_VALUES, data.indices.data(), &mesh_elem_restr));

      // Element restriction and basis objects for inputs.
      CeedElemRestriction nd_restr =
          nd_fespace.GetCeedElemRestriction(ceed, geom, data.indices);
      CeedBasis nd_basis = nd_fespace.GetCeedBasis(ceed, geom);

      // Construct coefficient for discontinuous flux.
      MaterialPropertyCoefficient muinv_func(mat_op.GetAttributeToMaterial(),
                                             mat_op.GetInvPermeability());
      auto ctx = ceed::PopulateCoefficientContext(mesh.SpaceDimension(), &muinv_func,
                                                  mesh.SpaceDimension(), nullptr);

      // Assemble the libCEED operator. Inputs: Discontinuous flux, then smooth flux.
      // Currently only supports 3D, since curl in 2D requires special treatment.
      ceed::CeedQFunctionInfo info;
      info.assemble_q_data = false;
      switch (10 * mesh.SpaceDimension() + mesh.Dimension())
      {
        case 33:
          info.apply_qf = f_apply_hdivhcurl_error_33;
          info.apply_qf_path = PalaceQFunctionRelativePath(f_apply_hdivhcurl_error_33_loc);
          break;
        default:
          MFEM_ABORT("Invalid value of (dim, space_dim) = ("
                     << mesh.Dimension() << ", " << mesh.SpaceDimension()
                     << ") for CurlFluxErrorEstimator!");
      }
      info.trial_ops = ceed::EvalMode::Curl;
      info.test_ops = ceed::EvalMode::Interp;

      CeedOperator op;
      ceed::AssembleCeedElementErrorIntegrator(
          info, (void *)ctx.data(), ctx.size() * sizeof(CeedIntScalar), ceed, U_gf_vec,
          F_gf_vec, nd_restr, nd_restr, nd_basis, nd_basis, mesh_elem_restr, data.geom_data,
          data.geom_data_restr, &op);

      // Do the integration (both input vectors are passive). For complex case, add sum of
      // squares of real and imaginary parts to the estimates before square root.
      PalaceCeedCall(ceed, CeedOperatorApplyAdd(op, CEED_VECTOR_NONE, estimates_vec,
                                                CEED_REQUEST_IMMEDIATE));
      if constexpr (std::is_same<VecType, ComplexVector>::value)
      {
        ceed::InitCeedVector(U_gf.Imag(), ceed, &U_gf_vec, false);
        ceed::InitCeedVector(F_gf.Imag(), ceed, &F_gf_vec, false);
        PalaceCeedCall(ceed, CeedOperatorApplyAdd(op, CEED_VECTOR_NONE, estimates_vec,
                                                  CEED_REQUEST_IMMEDIATE));
      }

      // Cleanup.
      PalaceCeedCall(ceed, CeedOperatorDestroy(&op));
      PalaceCeedCall(ceed, CeedElemRestrictionDestroy(&mesh_elem_restr));
      PalaceCeedCall(ceed, CeedVectorDestroy(&estimates_vec));
      PalaceCeedCall(ceed, CeedVectorDestroy(&U_gf_vec));
      PalaceCeedCall(ceed, CeedVectorDestroy(&F_gf_vec));
    }
  }

  // Finalize the element-wise error estimates.
  Vector estimates0(estimates, 0, mesh.GetNE()),
      estimates1(estimates, mesh.GetNE(), mesh.GetNE());
  auto norm2 = linalg::Sum<Vector>(mesh.GetComm(), estimates1);
  linalg::Sqrt(estimates0, (norm2 > 0.0) ? 1.0 / norm2 : 1.0);
  indicator.AddIndicator(estimates0);
}

GradFluxErrorEstimator::GradFluxErrorEstimator(const MaterialOperator &mat_op,
                                               FiniteElementSpace &h1_fespace,
                                               FiniteElementSpaceHierarchy &rt_fespaces,
                                               double tol, int max_it, int print,
                                               bool use_mg)
  : mat_op(mat_op), h1_fespace(h1_fespace), rt_fespace(rt_fespaces.GetFinestFESpace()),
    projector(mat_op, h1_fespace, rt_fespaces, tol, max_it, print, use_mg),
    U_gf(h1_fespace.GetVSize()), F(rt_fespace.GetTrueVSize()), F_gf(rt_fespace.GetVSize())
{
  U_gf.UseDevice(true);
  F.UseDevice(true);
  F_gf.UseDevice(true);
}

void GradFluxErrorEstimator::AddErrorIndicator(const Vector &U,
                                               ErrorIndicator &indicator) const
{
  // Compute the projection of the discontinuous flux onto the smooth finite element space
  // and populate the corresponding grid functions.
  BlockTimer bt(Timer::ESTIMATION);
  projector.Mult(U, F);
  h1_fespace.GetProlongationMatrix()->Mult(U, U_gf);
  rt_fespace.GetProlongationMatrix()->Mult(F, F_gf);

  // Use libCEED operators to perform the error estimate integration over each element. The
  // discontinuous flux is ε ∇U. Output is stored as error integral for all elements first,
  // then scaling integral.
  const auto &mesh = h1_fespace.GetMesh();
  constexpr CeedInt elem_num_comp = 2;
  Vector estimates(elem_num_comp * mesh.GetNE());
  estimates.UseDevice(true);
  estimates = 0.0;
  const std::size_t nt = ceed::internal::GetCeedObjects().size();
  PalacePragmaOmp(parallel if (nt > 1))
  {
    Ceed ceed = ceed::internal::GetCeedObjects()[utils::GetThreadNum()];
    for (const auto &[geom, data] : mesh.GetCeedGeomFactorData(ceed))
    {
      // Only integrate over domain elements (not on the boundary).
      if (mfem::Geometry::Dimension[geom] < mesh.Dimension())
      {
        continue;
      }

      // Create libCEED vector wrappers for use with libCEED operators. Each thread writes
      // to non-overlapping entries of the estimates vector.
      CeedVector estimates_vec, U_gf_vec, F_gf_vec;
      ceed::InitCeedVector(estimates, ceed, &estimates_vec);
      ceed::InitCeedVector(U_gf, ceed, &U_gf_vec);
      ceed::InitCeedVector(F_gf, ceed, &F_gf_vec);

      // Construct mesh element restriction for elements of this element geometry type.
      CeedElemRestriction mesh_elem_restr;
      PalaceCeedCall(ceed,
                     CeedElemRestrictionCreate(
                         ceed, static_cast<CeedInt>(data.indices.size()), 1, elem_num_comp,
                         mesh.GetNE(), elem_num_comp * mesh.GetNE(), CEED_MEM_HOST,
                         CEED_COPY_VALUES, data.indices.data(), &mesh_elem_restr));

      // Element restriction and basis objects for inputs.
      CeedElemRestriction h1_restr =
          h1_fespace.GetCeedElemRestriction(ceed, geom, data.indices);
      CeedElemRestriction rt_restr =
          rt_fespace.GetCeedElemRestriction(ceed, geom, data.indices);
      CeedBasis h1_basis = h1_fespace.GetCeedBasis(ceed, geom);
      CeedBasis rt_basis = rt_fespace.GetCeedBasis(ceed, geom);

      // Construct coefficient for discontinuous flux.
      MaterialPropertyCoefficient epsilon_func(mat_op.GetAttributeToMaterial(),
                                               mat_op.GetPermittivityReal());
      auto ctx = ceed::PopulateCoefficientContext(mesh.SpaceDimension(), &epsilon_func,
                                                  mesh.SpaceDimension(), nullptr);

      // Assemble the libCEED operator. Inputs: Discontinuous flux, then smooth flux.
      ceed::CeedQFunctionInfo info;
      info.assemble_q_data = false;
      switch (10 * mesh.SpaceDimension() + mesh.Dimension())
      {
        case 22:
          info.apply_qf = f_apply_hcurlhdiv_error_22;
          info.apply_qf_path = PalaceQFunctionRelativePath(f_apply_hcurlhdiv_error_22_loc);
          break;
        case 33:
          info.apply_qf = f_apply_hcurlhdiv_error_33;
          info.apply_qf_path = PalaceQFunctionRelativePath(f_apply_hcurlhdiv_error_33_loc);
          break;
        default:
          MFEM_ABORT("Invalid value of (dim, space_dim) = ("
                     << mesh.Dimension() << ", " << mesh.SpaceDimension()
                     << ") for GradFluxErrorEstimator!");
      }
      info.trial_ops = ceed::EvalMode::Grad;
      info.test_ops = ceed::EvalMode::Interp;

      CeedOperator op;
      ceed::AssembleCeedElementErrorIntegrator(
          info, (void *)ctx.data(), ctx.size() * sizeof(CeedIntScalar), ceed, U_gf_vec,
          F_gf_vec, h1_restr, rt_restr, h1_basis, rt_basis, mesh_elem_restr, data.geom_data,
          data.geom_data_restr, &op);

      // Do the integration (both input vectors are passive).
      PalaceCeedCall(ceed, CeedOperatorApplyAdd(op, CEED_VECTOR_NONE, estimates_vec,
                                                CEED_REQUEST_IMMEDIATE));

      // Cleanup.
      PalaceCeedCall(ceed, CeedOperatorDestroy(&op));
      PalaceCeedCall(ceed, CeedElemRestrictionDestroy(&mesh_elem_restr));
      PalaceCeedCall(ceed, CeedVectorDestroy(&estimates_vec));
      PalaceCeedCall(ceed, CeedVectorDestroy(&U_gf_vec));
      PalaceCeedCall(ceed, CeedVectorDestroy(&F_gf_vec));
    }
  }

  // Finalize the element-wise error estimates.
  Vector estimates0(estimates, 0, mesh.GetNE()),
      estimates1(estimates, mesh.GetNE(), mesh.GetNE());
  auto norm2 = linalg::Sum<Vector>(mesh.GetComm(), estimates1);
  linalg::Sqrt(estimates0, (norm2 > 0.0) ? 1.0 / norm2 : 1.0);
  indicator.AddIndicator(estimates0);
}

template class FluxProjector<Vector>;
template class FluxProjector<ComplexVector>;
template class CurlFluxErrorEstimator<Vector>;
template class CurlFluxErrorEstimator<ComplexVector>;

}  // namespace palace
