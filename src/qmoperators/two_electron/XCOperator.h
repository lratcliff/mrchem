#pragma once

#include "XCPotential.h"
#include "XCPotentialD1.h"
#include "XCPotentialD2.h"
#include "qmoperators/RankZeroTensorOperator.h"

/** @class XCOperator
 *
 * @brief DFT Exchange-Correlation operator containing a single XCPotential
 *
 * This class is a simple TensorOperator realization of @class XCPotential.
 *
 */

namespace mrchem {

class XCOperator final : public RankZeroTensorOperator {
public:
    explicit XCOperator(std::shared_ptr<mrdft::XCFunctional> F,
                        std::shared_ptr<OrbitalVector> Phi = nullptr,
                        bool mpi_shared = false) {
        potential = std::make_shared<XCPotentialD1>(F, Phi, mpi_shared);

        // Invoke operator= to assign *this operator
        RankZeroTensorOperator &XC = (*this);
        XC = potential;
    }
    XCOperator(std::shared_ptr<mrdft::XCFunctional> F,
               std::shared_ptr<OrbitalVector> Phi,
               std::shared_ptr<OrbitalVector> X,
               std::shared_ptr<OrbitalVector> Y,
               bool mpi_shared = false) {
        potential = std::make_shared<XCPotentialD2>(F, Phi, X, Y, mpi_shared);

        // Invoke operator= to assign *this operator
        RankZeroTensorOperator &XC = (*this);
        XC = potential;
    }
    ~XCOperator() override = default;

    void setupDensity(double prec = -1.0) { potential->setupDensity(prec); }
    void setupPotential(double prec = -1.0) { potential->setupPotential(prec); }

    double getEnergy() { return potential->getEnergy(); }
    mrcpp::FunctionTree<3> &getDensity(int spin) { return potential->getDensity(spin); }
    std::shared_ptr<mrdft::XCFunctional> &getFunctional() { return potential->getFunctional(); }

private:
    std::shared_ptr<XCPotential> potential{nullptr};
};

} // namespace mrchem
