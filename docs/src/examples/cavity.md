```@raw html
<!--- Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved. --->
<!--- SPDX-License-Identifier: Apache-2.0 --->
```

# Eigenmodes of a Cylindrical Cavity

!!! note
    
    The files for this example can be found in the [`examples/cavity/`]
    (https://github.com/awslabs/palace/blob/main/examples/cavity) directory of the *Palace*
    source code.

This example demonstrates *Palace*'s eigenmode simulation type to solve for the lowest
frequency modes of a cylindrical cavity resonator. In particular, we consider a cylindrical
cavity filled with Teflon (``\varepsilon_r = 2.08``,
``\tan\delta = 4\times 10^{-4}``), with radius ``a = 2.74\text{ cm}`` and height
``d = 2a``. From [[1]](#References), the frequencies of the ``\text{TE}_{nml}`` and
``\text{TM}_{nml}`` modes are given by

```math
\begin{aligned}
f_{\text{TE},nml} &= \frac{1}{2\pi\sqrt{\mu\varepsilon}}
    \sqrt{\left(\frac{p'_{nm}}{a}\right)^2 +
    \left(\frac{l\pi}{d}\right)^2} \\
f_{\text{TM},nml} &= \frac{1}{2\pi\sqrt{\mu\varepsilon}}
    \sqrt{\left(\frac{p_{nm}}{a}\right)^2 +
    \left(\frac{l\pi}{d}\right)^2} \\
\end{aligned}
```

where  ``p_{nm}`` and ``p'_{nm}`` denote the ``m``-th root (``m\geq 1``) of the ``n``-th
order Bessel function (``n\geq 0``) of the first kind, ``J_n``, and its derivative,
``J'_n``, respectively.

In addition, we have analytic expressions for the unloaded quality factors due to dielectric
loss, ``Q_d``, and imperfectly conducting walls, ``Q_c``. In particular,

```math
Q_d = \frac{1}{\tan\delta}
```

and, for a surface resistance ``R_s``,

```math
Q_c = \frac{(ka)^3\eta ad}{4(p'_{nm})^2 R_s}
    \left[1-\left(\frac{n}{p'_{nm}}\right)^2\right]
    \left\{\frac{ad}{2}
        \left[1+\left(\frac{\beta an}{(p'_{nm})^2}\right)^2\right] +
        \left(\frac{\beta a^2}{p'_{nm}}\right)^2
        \left(1-\frac{n^2}{(p'_{nm})^2}\right)\right\}^{-1}
```

where ``k=\omega\sqrt{\mu\varepsilon}``, ``\eta=\sqrt{\mu/\varepsilon}``, and
``\beta=l\pi/d``.

The initial Gmsh mesh for this problem, from [`mesh/cavity.msh`]
(https://github.com/awslabs/palace/blob/main/examples/cavity/mesh/cavity.msh), is shown
below. We use quadratic triangular prism elements.

```@raw html
<br/><p align="center">
  <img src="../../assets/examples/cavity-1.png" width="60%" />
</p><br/>
```

There are two configuration files for this problem, [`cavity_pec.json`]
(https://github.com/awslabs/palace/blob/main/examples/cavity/cavity_pec.json) and
[`cavity_impedance.json`]
(https://github.com/awslabs/palace/blob/main/examples/cavity/cavity_impedance.json).

In both, the [`config["Problem"]["Type"]`](../config/problem.md#config%5B%22Problem%22%5D)

field is set to `"Eigenmode"`, and we use the mesh shown above with a single level of
uniform mesh refinement (`"UniformLevels": 1`). The material properties for Teflon are
entered under [`config["Domains"]["Materials"]`]
(../config/domains.md#domains%5B%22Materials%22%5D). The
[`config["Domains"]["Postprocessing"]["Dielectric]"`]
(../config/domains.md#domains["Postprocessing"]["Dielectric"]) object is used to extract the
quality factor due to bulk dielectric loss; in this problem since there is only one domain
this is trivial, but in problems with multiple material domains this feature can be used to
isolate the energy-participation ratio (EPR) and associated quality factor due to different
domains in the model.

The only difference between the two configuration files is in the `"Boundaries"` object:
`cavity_pec.json` prescribes a perfect electric conductor (`"PEC"`) boundary condition to
the cavity boundary surfaces, while `cavity_impedance.json` prescribes a surface impedance
condition with the surface resistance ``R_s = 0.0184\text{ }\Omega\text{/sq}``, for copper
at ``5\text{ GHz}``.

In both cases, we configure the eigenvalue solver to solve for the ``15`` lowest frequency
modes above ``2.0\text{ GHz}`` (the dominant mode frequencies for both the
``\text{TE}`` and ``\text{TM}`` cases fall around ``2.9\text{ GHz}`` frequency for this
problem). A sparse direct solver is used for the solutions of the linear system resulting
from the spatial discretization of the governing equations, using in this case a second-
order finite element space.

The frequencies for the lowest order ``\text{TE}`` and ``\text{TM}`` modes computed using
the above formula for this problem are listed in the table below.

| ``(n,m,l)`` | ``f_{\text{TE}}``       | ``f_{\text{TM}}``       |
|:----------- | -----------------------:| -----------------------:|
| ``(0,1,0)`` | ----                    | ``2.903605\text{ GHz}`` |
| ``(1,1,0)`` | ----                    | ``4.626474\text{ GHz}`` |
| ``(2,1,0)`` | ----                    | ``6.200829\text{ GHz}`` |
| ``(0,1,1)`` | ``5.000140\text{ GHz}`` | ``3.468149\text{ GHz}`` |
| ``(1,1,1)`` | ``2.922212\text{ GHz}`` | ``5.000140\text{ GHz}`` |
| ``(2,1,1)`` | ``4.146842\text{ GHz}`` | ``6.484398\text{ GHz}`` |
| ``(0,1,2)`` | ``5.982709\text{ GHz}`` | ``4.776973\text{ GHz}`` |
| ``(1,1,2)`` | ``4.396673\text{ GHz}`` | ``5.982709\text{ GHz}`` |
| ``(2,1,2)`` | ``5.290341\text{ GHz}`` | ``7.269033\text{ GHz}`` |

First, we examine the output of the `cavity_pec.json` simulation. The file
`postpro/pec/eig.csv` contains information about the computed eigenfrequencies and
associated quality factors:

```
            m,             Re{f} (GHz),             Im{f} (GHz),                       Q
 1.000000e+00,        +2.907427019e+00,        +5.814853773e-04,        +2.500000164e+03
 2.000000e+00,        +2.924280615e+00,        +5.848561115e-04,        +2.500000099e+03
 3.000000e+00,        +2.924280615e+00,        +5.848561049e-04,        +2.500000128e+03
 4.000000e+00,        +3.471366614e+00,        +6.942732642e-04,        +2.500000261e+03
 5.000000e+00,        +4.150995312e+00,        +8.301991170e-04,        +2.499999886e+03
 6.000000e+00,        +4.150995313e+00,        +8.301990448e-04,        +2.500000103e+03
 7.000000e+00,        +4.398872301e+00,        +8.797743653e-04,        +2.500000320e+03
 8.000000e+00,        +4.398872302e+00,        +8.797744266e-04,        +2.500000146e+03
 9.000000e+00,        +4.633923986e+00,        +9.267848072e-04,        +2.500000023e+03
 1.000000e+01,        +4.633923988e+00,        +9.267847614e-04,        +2.500000148e+03
 1.100000e+01,        +4.780067633e+00,        +9.560135244e-04,        +2.500000056e+03
 1.200000e+01,        +5.006352487e+00,        +1.001270561e-03,        +2.499999892e+03
 1.300000e+01,        +5.007045617e+00,        +1.001409149e-03,        +2.499999986e+03
 1.400000e+01,        +5.007045618e+00,        +1.001409063e-03,        +2.500000202e+03
 1.500000e+01,        +5.294282054e+00,        +1.058856488e-03,        +2.499999867e+03
```

Indeed we can find a correspondence between the analytic modes predicted and the solutions
obtained by *Palace*. Since the only source of loss in the simulation is the nonzero
dielectric loss tangent, we have ``Q = Q_d = 1/0.0004 = 2.50\times 10^3`` in all cases.

Next, we run `cavity_impedance.json`, which  adds the surface impedance boundary condition.
Examining `postpro/impedance/eig.csv` we see that the mode frequencies are roughly
unchanged but the quality factors have fallen due to the addition of imperfectly conducting
walls to the model:

```
            m,             Re{f} (GHz),             Im{f} (GHz),                       Q
 1.000000e+00,        +2.907427020e+00,        +7.092642123e-04,        +2.049607929e+03
 2.000000e+00,        +2.924280612e+00,        +7.056235938e-04,        +2.072125084e+03
 3.000000e+00,        +2.924280613e+00,        +7.056079073e-04,        +2.072171150e+03
 4.000000e+00,        +3.471366618e+00,        +8.645831050e-04,        +2.007537914e+03
 5.000000e+00,        +4.150995316e+00,        +9.793016718e-04,        +2.119365029e+03
 6.000000e+00,        +4.150995324e+00,        +9.792938252e-04,        +2.119382014e+03
 7.000000e+00,        +4.398872282e+00,        +1.000618955e-03,        +2.198075688e+03
 8.000000e+00,        +4.398872287e+00,        +1.000607552e-03,        +2.198100739e+03
 9.000000e+00,        +4.633923998e+00,        +1.054820966e-03,        +2.196545322e+03
 1.000000e+01,        +4.633923999e+00,        +1.054827262e-03,        +2.196532213e+03
 1.100000e+01,        +4.780067631e+00,        +1.126452446e-03,        +2.121735268e+03
 1.200000e+01,        +5.006352481e+00,        +1.086541116e-03,        +2.303802647e+03
 1.300000e+01,        +5.007045627e+00,        +1.171987929e-03,        +2.136133675e+03
 1.400000e+01,        +5.007045634e+00,        +1.171975803e-03,        +2.136155781e+03
 1.500000e+01,        +5.294282063e+00,        +1.207981180e-03,        +2.191376111e+03
```

However, the bulk dielectric loss postprocessing results, written to
`postpro/impedance/domain-Q.csv`, still give ``Q_d = 2.50\times 10^3`` for every mode as
expected.

Focusing on the ``\text{TE}_{011}`` mode with ``f_{\text{TE},010} = 5.00\text{ GHz}``, we
can read the mode quality factor ``Q = 2.30\times 10^3``. Subtracting out the contribution
of dielectric losses, we have

```math
Q_c = \left(\frac{1}{Q}-\frac{1}{Q_d}\right)^{-1} = 2.94\times 10^4
```

which is the same as the analytical result given in Example 6.4 from [[1]](#References) for
this geometry.

Finally, a clipped view of the electric field (left) and magnetic flux density magnitudes
for the ``\text{TE}_{011}`` mode is shown below.

```@raw html
<br/><p align="center">
  <img src="../../assets/examples/cavity-2a.png" width="45%" />
  <img src="../../assets/examples/cavity-2b.png" width="45%" />
</p>
```

## Mesh convergence

The effect of mesh size can be investigated for the cylindrical cavity resonator using
[`convergence_study.jl`]
(https://github.com/awslabs/palace/blob/main/examples/cavity/convergence_study.jl). For
a polynomial order of solution and refinement level, a mesh is generated using Gmsh using
polynomials of the same order to resolve the boundary geometry. The eigenvalue problem is
then solved for ``f_{\text{TM},010}`` and ``f_{\text{TE},111}``, and the relative error,
``\frac{f-f_{\text{true}}}{f_{\text{true}}}``, of each mode plotted against
``\text{DOF}^{-\frac{1}{3}}``, a notional mesh size. Three different element types are
considered: tetrahedra, prisms and hexahedra, and the results are plotted below. The
``x``-axis is a notional measure of the overall cost of the solve, accounting for
polynomial order.

```@raw html
<br/><p align="center">
  <img src="../../assets/examples/cavity_error_tetrahedra.png" width="70%" />
</p><br/>
```

```@raw html
<br/><p align="center">
  <img src="../../assets/examples/cavity_error_prism.png" width="70%" />
</p><br/>
```

```@raw html
<br/><p align="center">
  <img src="../../assets/examples/cavity_error_hexahedra.png" width="70%" />
</p><br/>
```

The observed rate of convergence for the eigenvalues are ``p+1`` for odd polynomials and
``p+2`` for even polynomials. Given the eigenmodes are analytic functions, the theoretical
maximum convergence rate is ``2p`` [[2]](#References). The figures demonstrate that
increasing the polynomial order of the solution will give reduced error, however the effect
may only become significant on sufficiently refined meshes.

## References

[1] D. M. Pozar, _Microwave Engineering_, Wiley, Hoboken, NJ, 2012.\
[2] A. Buffa, P. Houston, I. Perugia, _Discontinuous Galerkin computation of the Maxwell
eigenvalues on simplicial meshes_, Journal of Computational and Applied Mathematics 204
(2007) 317-333.
