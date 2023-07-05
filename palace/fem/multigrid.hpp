// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#ifndef PALACE_FEM_MULTIGRID_HPP
#define PALACE_FEM_MULTIGRID_HPP

#include <memory>
#include <vector>
#include <mfem.hpp>
#include "linalg/operator.hpp"
#include "linalg/rap.hpp"
#include "utils/iodata.hpp"

namespace palace::utils
{

//
// Methods for constructing hierarchies of finite element spaces for geometric multigrid.
//

// Helper function for getting the order of the finite element space underlying a bilinear
// form.
inline auto GetMaxElementOrder(mfem::BilinearForm &a)
{
  return a.FESpace()->GetMaxElementOrder();
}

// Helper function for getting the order of the finite element space underlying a mixed
// bilinear form.
inline auto GetMaxElementOrder(mfem::MixedBilinearForm &a)
{
  return std::max(a.TestFESpace()->GetMaxElementOrder(),
                  a.TrialFESpace()->GetMaxElementOrder());
}

// Assembly a bilinear or mixed bilinear form. If the order is lower than the specified
// threshold, the operator is assembled as a sparse matrix.
template <typename BilinearForm>
inline std::unique_ptr<Operator>
AssembleOperator(std::unique_ptr<BilinearForm> &&a, bool mfem_pa_support,
                 int pa_order_threshold, int skip_zeros = 1)
{
  mfem::AssemblyLevel assembly_level =
      (mfem::DeviceCanUseCeed() ||
       (mfem_pa_support && GetMaxElementOrder(*a) >= pa_order_threshold))
          ? mfem::AssemblyLevel::PARTIAL
          : mfem::AssemblyLevel::LEGACY;
  a->SetAssemblyLevel(assembly_level);
  a->Assemble(skip_zeros);
  a->Finalize(skip_zeros);
  if (assembly_level == mfem::AssemblyLevel::LEGACY ||
      (assembly_level == mfem::AssemblyLevel::PARTIAL &&
       GetMaxElementOrder(*a) < pa_order_threshold &&
       std::is_base_of<mfem::BilinearForm, BilinearForm>::value))
  {
    // libCEED full assembly does not support mixed forms.
#ifdef MFEM_USE_CEED
    mfem::SparseMatrix *spm =
        a->HasExt() ? mfem::ceed::CeedOperatorFullAssemble(*a) : a->LoseMat();
#else
    mfem::SparseMatrix *spm = a->LoseMat();
#endif
    MFEM_VERIFY(spm, "Missing assembled sparse matrix!");
    return std::unique_ptr<Operator>(spm);
  }
  else
  {
    return std::move(a);
  }
}

// Construct sequence of FECollection objects.
template <typename FECollection>
std::vector<std::unique_ptr<FECollection>> inline ConstructFECollections(
    int p, int dim, int mg_max_levels,
    config::LinearSolverData::MultigridCoarsenType mg_coarsen_type, bool mat_lor)
{
  // If the solver will use a LOR preconditioner, we need to construct with a specific basis
  // type.
  MFEM_VERIFY(p >= 1, "FE space order must not be less than 1!");
  int b1 = mfem::BasisType::GaussLobatto, b2 = mfem::BasisType::GaussLegendre;
  if (mat_lor)
  {
    b2 = mfem::BasisType::IntegratedGLL;
  }
  constexpr int pm1 = (std::is_base_of<mfem::H1_FECollection, FECollection>::value ||
                       std::is_base_of<mfem::ND_FECollection, FECollection>::value)
                          ? 0
                          : 1;

  // Construct the p-multigrid hierarchy, first finest to coarsest and then reverse the
  // order.
  std::vector<std::unique_ptr<FECollection>> fecs;
  for (int l = 0; l < std::max(1, mg_max_levels); l++)
  {
    if constexpr (std::is_base_of<mfem::ND_FECollection, FECollection>::value ||
                  std::is_base_of<mfem::RT_FECollection, FECollection>::value)
    {
      fecs.push_back(std::make_unique<FECollection>(p - pm1, dim, b1, b2));
    }
    else
    {
      fecs.push_back(std::make_unique<FECollection>(p - pm1, dim, b1));
      MFEM_CONTRACT_VAR(b2);
    }
    if (p == 1)
    {
      break;
    }
    switch (mg_coarsen_type)
    {
      case config::LinearSolverData::MultigridCoarsenType::LINEAR:
        p--;
        break;
      case config::LinearSolverData::MultigridCoarsenType::LOGARITHMIC:
        p = (p + 1) / 2;
        break;
      case config::LinearSolverData::MultigridCoarsenType::INVALID:
        MFEM_ABORT("Invalid coarsening type for p-multigrid levels!");
        break;
    }
  }
  std::reverse(fecs.begin(), fecs.end());
  return fecs;
}

// Construct a hierarchy of finite element spaces given a sequence of meshes and
// finite element collections. Dirichlet boundary conditions are additionally
// marked.
template <typename FECollection>
inline mfem::ParFiniteElementSpaceHierarchy ConstructFiniteElementSpaceHierarchy(
    int mg_max_levels, bool mg_legacy_transfer, int pa_order_threshold,
    const std::vector<std::unique_ptr<mfem::ParMesh>> &mesh,
    const std::vector<std::unique_ptr<FECollection>> &fecs,
    const mfem::Array<int> *dbc_marker = nullptr,
    std::vector<mfem::Array<int>> *dbc_tdof_lists = nullptr)
{
  MFEM_VERIFY(!mesh.empty() && !fecs.empty() &&
                  (!dbc_tdof_lists || dbc_tdof_lists->empty()),
              "Empty mesh or FE collection for FE space construction!");
  auto mesh_levels = std::min(mesh.size() - 1, mg_max_levels - fecs.size());
  auto *fespace = new mfem::ParFiniteElementSpace(mesh[mesh.size() - mesh_levels - 1].get(),
                                                  fecs[0].get());
  if (dbc_marker && dbc_tdof_lists)
  {
    fespace->GetEssentialTrueDofs(*dbc_marker, dbc_tdof_lists->emplace_back());
  }
  mfem::ParFiniteElementSpaceHierarchy fespaces(mesh[mesh.size() - mesh_levels - 1].get(),
                                                fespace, false, true);

  // h-refinement
  for (std::size_t l = mesh.size() - mesh_levels; l < mesh.size(); l++)
  {
    fespace = new mfem::ParFiniteElementSpace(mesh[l].get(), fecs[0].get());
    if (dbc_marker && dbc_tdof_lists)
    {
      fespace->GetEssentialTrueDofs(*dbc_marker, dbc_tdof_lists->emplace_back());
    }
    auto *P = new ParOperator(
        std::make_unique<mfem::TransferOperator>(fespaces.GetFinestFESpace(), *fespace),
        fespaces.GetFinestFESpace(), *fespace, true);
    fespaces.AddLevel(mesh[l].get(), fespace, P, false, true, true);
  }

  // p-refinement
  for (std::size_t l = 1; l < fecs.size(); l++)
  {
    fespace = new mfem::ParFiniteElementSpace(mesh.back().get(), fecs[l].get());
    if (dbc_marker && dbc_tdof_lists)
    {
      fespace->GetEssentialTrueDofs(*dbc_marker, dbc_tdof_lists->emplace_back());
    }
    ParOperator *P;
    if (!mg_legacy_transfer && mfem::DeviceCanUseCeed())
    {
      // Partial and full assembly for this operator is only available with libCEED backend.
      auto p = std::make_unique<mfem::DiscreteLinearOperator>(&fespaces.GetFinestFESpace(),
                                                              fespace);
      p->AddDomainInterpolator(new mfem::IdentityInterpolator);
      P = new ParOperator(AssembleOperator(std::move(p), false, pa_order_threshold),
                          fespaces.GetFinestFESpace(), *fespace, true);
    }
    else
    {
      P = new ParOperator(
          std::make_unique<mfem::TransferOperator>(fespaces.GetFinestFESpace(), *fespace),
          fespaces.GetFinestFESpace(), *fespace, true);
    }
    fespaces.AddLevel(mesh.back().get(), fespace, P, false, true, true);
  }
  return fespaces;
}

}  // namespace palace::utils

#endif  // PALACE_FEM_MULTIGRID_HPP
