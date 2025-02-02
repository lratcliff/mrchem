/*
 * MRChem, a numerical real-space code for molecular electronic structure
 * calculations within the self-consistent field (SCF) approximations of quantum
 * chemistry (Hartree-Fock and Density Functional Theory).
 * Copyright (C) 2023 Stig Rune Jensen, Luca Frediani, Peter Wind and contributors.
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

#pragma once

#include <memory>

#include "QMOperator.h"

namespace mrchem {

class QMDerivative final : public QMOperator {
public:
    QMDerivative(int d, std::shared_ptr<mrcpp::DerivativeOperator<3>> D, bool im = false);
    QMDerivative(const QMDerivative &inp);

private:
    const bool imag;     // add imaginary unit prefactor, for faster application
    const int apply_dir; // Cartesian direction of derivative
    std::shared_ptr<mrcpp::DerivativeOperator<3>> derivative;

    bool isReal() const { return not(imag); }
    bool isImag() const { return imag; }

    ComplexDouble evalf(const mrcpp::Coord<3> &r) const override { return 0.0; }

    Orbital apply(Orbital inp) override;
    Orbital dagger(Orbital inp) override;
    QMOperatorVector apply(std::shared_ptr<QMOperator> &O) override;
};

} // namespace mrchem
