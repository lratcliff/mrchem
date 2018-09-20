/*
 * MRChem, a numerical real-space code for molecular electronic structure
 * calculations within the self-consistent field (SCF) approximations of quantum
 * chemistry (Hartree-Fock and Density Functional Theory).
 * Copyright (C) 2018 Stig Rune Jensen, Jonas Juselius, Luca Frediani, and contributors.
 *
 * This file is part of MRChem.
 *
 * MRChem is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MRChem is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with MRChem.  If not, see <https://www.gnu.org/licenses/>.
 *
 * For information on the complete list of contributors to MRChem, see:
 * <https://mrchem.readthedocs.io/>
 */

#include "catch.hpp"

#include "MRCPP/MWOperators"

#include "mrchem.h"

#include "analyticfunctions/HydrogenFunction.h"
#include "qmoperators/one_electron/NuclearOperator.h"
#include "qmfunctions/Orbital.h"
#include "qmfunctions/orbital_utils.h"

using namespace mrchem;
using namespace orbital;

namespace nuclear_potential {

TEST_CASE("NuclearOperator", "[nuclear_operator]") {
    const double prec = 1.0e-3;
    const double thrs = prec*prec;

    int nShells = 2;
    OrbitalVector Phi;
    for (int n = 1; n <= nShells; n++) {
        int L = n;
        for (int l = 0; l < L; l++) {
            int M = 2*l+1;
            for (int m = 0; m < M; m++) {
                HydrogenFunction f(n, l, m);

                Orbital phi(SPIN::Paired);
                phi.alloc(NUMBER::Real);
                mrcpp::project(prec, phi.real(), f);
                Phi.push_back(phi);
            }
        }
    }

    // reference values for hydrogen eigenfunctions
    int i = 0;
    DoubleVector E_P(Phi.size());
    for (int n = 1; n <= nShells; n++) {
        int L = n;
        double E_n = 1.0/(2.0*n*n);     //E_n = Z^2/(2*n^2)
        for (int l = 0; l < L; l++) {
            int M = 2*l+1;
            for (int m = 0; m < M; m++) {
                E_P(i++) = -2.0*E_n;    //virial theorem: 2<E_K> = -<E_P>
            }
        }
    }

    Nuclei nucs;
    nucs.push_back("H", 0);
    NuclearOperator V(nucs, prec);

    V.setup(prec);
    SECTION("apply") {
        Orbital Vphi_0 = V(Phi[0]);
        ComplexDouble V_00 = orbital::dot(Phi[0], Vphi_0);
        REQUIRE( V_00.real() == Approx(E_P(0)).epsilon(prec) );
        REQUIRE( V_00.imag() < thrs );
        Vphi_0.free();
    }
    SECTION("vector apply") {
        OrbitalVector VPhi = V(Phi);
        for (int i = 0; i < Phi.size(); i++) {
            ComplexDouble V_ii = orbital::dot(Phi[i], VPhi[i]);
            REQUIRE( V_ii.real() == Approx(E_P(i)).epsilon(prec) );
            REQUIRE( V_ii.imag() < thrs );
        }
        free(VPhi);
    }
    SECTION("expectation value") {
        ComplexDouble V_00 = V(Phi[0], Phi[0]);
        REQUIRE( V_00.real() == Approx(E_P(0)).epsilon(prec) );
        REQUIRE( V_00.imag() < thrs );
    }
    SECTION("expectation matrix ") {
        ComplexMatrix v = V(Phi, Phi);
        for (int i = 0; i < Phi.size(); i++) {
            REQUIRE( v(i,i).real() == Approx(E_P(i)).epsilon(prec) );
            REQUIRE( v(i,i).imag() < thrs );
        }
    }
    V.clear();
    free(Phi);
}

} //namespace kinetic_operator
