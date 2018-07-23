#include "MRCPP/Printer"
#include "MRCPP/Timer"

#include "parallel.h"
#include "utils/math_utils.h"
#include "utils/RRMaximizer.h"

#include "qmfunction_utils.h"
#include "orbital_utils.h"
#include "Orbital.h"
#include "OrbitalIterator.h"

using mrcpp::Timer;
using mrcpp::Printer;
using mrcpp::FunctionTree;
using mrcpp::FunctionTreeVector;

namespace mrchem {

/****************************************
 * Orbital related standalone functions *
 ****************************************/

/** @brief Compute <bra|ket> = int bra^\dag(r) * ket(r) dr.
 *
 *  Complicated by the fact that both bra and ket can be interpreted
 *  as complex conjugate versions of themselves. Notice that the <bra|
 *  position is already complex conjugated.
 *
 */
ComplexDouble orbital::dot(Orbital bra, Orbital ket) {
    if ((bra.spin() == SPIN::Alpha) and (ket.spin() == SPIN::Beta)) return 0.0;
    if ((bra.spin() == SPIN::Beta) and (ket.spin() == SPIN::Alpha)) return 0.0;

    double bra_conj(1.0), ket_conj(1.0);
    if (bra.conjugate()) bra_conj = -1.0;
    if (ket.conjugate()) ket_conj = -1.0;

    return qmfunction::dot(bra, bra_conj, ket, ket_conj);
}

/** @brief Compute the diagonal dot products <bra_i|ket_i>
 *
 * MPI: dot product is computed by the ket owner and the corresponding
 *      bra is communicated. The resulting vector is allreduced, and
 *      the foreign bra's are cleared.
 *
 */
ComplexVector orbital::dot(OrbitalVector &bra, OrbitalVector &ket) {
    if (bra.size() != ket.size()) MSG_FATAL("Size mismatch");

    int N = bra.size();
    ComplexVector result = ComplexVector::Zero(N);
    for (int i = 0; i < N; i++) {
        // The bra is sent to the owner of the ket
        if (bra[i].rankID() != ket[i].rankID()) {
            int tag = 8765*i;
            int src = bra[i].rankID();
            int dst = ket[i].rankID();
            if (mpi::my_orb(bra[i])) mpi::send_orbital(bra[i], dst, tag);
            if (mpi::my_orb(ket[i])) mpi::recv_orbital(bra[i], src, tag);
        }
        result[i] = orbital::dot(bra[i], ket[i]);
    }
    mpi::free_foreign(bra);
    mpi::allreduce_vector(result, mpi::comm_orb);
    return result;
}

/** @brief Compare spin and occupancy of two orbitals
 *
 *  Returns true if orbital parameters are the same.
 *
 */
bool orbital::compare(const Orbital &orb_a, const Orbital &orb_b) {
    bool comp = true;
    if (compare_occ(orb_a, orb_b) < 0) {
        MSG_WARN("Different occupancy");
        comp = false;
    }
    if (compare_spin(orb_a, orb_b) < 0) {
        MSG_WARN("Different spin");
        comp = false;
    }
    return comp;
}

/** @brief Compare occupancy of two orbitals
 *
 *  Returns the common occupancy if they match, -1 if they differ.
 *
 */
int orbital::compare_occ(const Orbital &orb_a, const Orbital &orb_b) {
    int comp = -1;
    if (orb_a.occ() == orb_b.occ()) comp = orb_a.occ();
    return comp;
}

/** @brief Compare spin of two orbitals
 *
 *  Returns the common spin if they match, -1 if they differ.
 *
 */
int orbital::compare_spin(const Orbital &orb_a, const Orbital &orb_b) {
    int comp = -1;
    if (orb_a.spin() == orb_b.spin()) comp = orb_a.spin();
    return comp;
}

/** @brief out = a*inp_a + b*inp_b
  * Complicated by the fact that both inputs can be interpreted as complex
  * conjugate versions of themselves. */
Orbital orbital::add(ComplexDouble a, Orbital inp_a, ComplexDouble b, Orbital inp_b, double prec) {
    ComplexVector coefs(2);
    coefs(0) = a;
    coefs(1) = b;

    OrbitalVector orbs;
    orbs.push_back(inp_a);
    orbs.push_back(inp_b);

    return linear_combination(coefs, orbs, prec);
}

/** @brief out_i = a*(inp_a)_i + b*(inp_b)_i
 *
 *  Component-wise addition of orbitals.
 *
 */
OrbitalVector orbital::add(ComplexDouble a, OrbitalVector &inp_a,
                           ComplexDouble b, OrbitalVector &inp_b,
                           double prec) {
    if (inp_a.size() != inp_b.size()) MSG_ERROR("Size mismatch");

    OrbitalVector out;
    for (int i = 0; i < inp_a.size(); i++) {
        if (inp_a[i].rankID() != inp_b[i].rankID()) MSG_FATAL("MPI rank mismatch");
        Orbital out_i = add(a, inp_a[i], b, inp_b[i]);
        out.push_back(out_i);
    }
    return out;
}

/** @brief out = inp_a * inp_b
  *
  * Complicated by the fact that both inputs can be interpreted
  * as complex conjugate versions of themselves.
  *
  */
Orbital orbital::multiply(Orbital inp_a, Orbital inp_b, double prec) {

    int occ = compare_occ(inp_a, inp_b);
    int spin = compare_spin(inp_a, inp_b);
    Orbital out(spin, occ);

    double conj_a(1.0), conj_b(1.0);
    if (inp_a.conjugate()) conj_a = -1.0;
    if (inp_b.conjugate()) conj_b = -1.0;
    
    qmfunction::multiply(inp_a, conj_a, inp_b, conj_b, out, prec);
    return out;
}

/** @brief out = c_0*inp_0 + c_1*inp_1 + ...
  *
  * Complicated by the fact that both inputs can be interpreted as complex
  * conjugate versions of themselves.
  *
  */
Orbital orbital::linear_combination(const ComplexVector &c, OrbitalVector &inp, double prec) {
    if (c.size() != inp.size()) MSG_ERROR("Size mismatch");
    double thrs = mrcpp::MachineZero;

    Orbital out;
    QMFunctionVector tmp_orb;
    ComplexVector tmp_coef(c.size());
    // set output spin from first contributing input
    for (int i = 0; i < inp.size(); i++) {
        if (std::abs(c[i]) < thrs) continue;
        out = inp[i].paramCopy();
        break;
    }
    // all contributing input spins must be equal
    for (int i = 0; i < inp.size(); i++) {
        if (std::abs(c[i]) < thrs) continue;
        if (out.spin() != inp[i].spin()) {
            // empty orbitals with wrong spin can occur
            if (inp[i].hasReal()) MSG_FATAL("Mixing spins");
            if (inp[i].hasImag()) MSG_FATAL("Mixing spins");
        }
        double sign = (inp[i].conjugate()) ? -1.0 : 1.0;
        tmp_orb.push_back(std::make_tuple(sign, inp[i]));
        tmp_coef[tmp_orb.size()-1] = c[i];
    }
    qmfunction::linear_combination(tmp_coef, tmp_orb, out, prec);
    return out;
}

/** @brief Orbital transformation out_vec = U*inp_vec
 *
 * The transformation matrix is not necessarily square.
 *
 */
OrbitalVector orbital::linear_combination(const ComplexMatrix &U, OrbitalVector &inp, double prec) {
    // Get all out orbitals belonging to this MPI
    //LUCA: if one does the following, then the matrix must be square...
    OrbitalVector out = orbital::param_copy(inp);
    OrbitalIterator iter(inp);
    while (iter.next()) {
        for (int i = 0; i < out.size(); i++) {
            if (not mpi::my_orb(out[i])) continue;
            OrbitalVector orb_vec;
            ComplexVector coef_vec(iter.get_size());
            for (int j = 0; j < iter.get_size(); j++) {
                int idx_j = iter.idx(j);
                Orbital &recv_j = iter.orbital(j);
                coef_vec[j] = U(i, idx_j);
                orb_vec.push_back(recv_j);
            }
            Orbital tmp_i = linear_combination(coef_vec, orb_vec, prec);
            out[i].add(1.0, tmp_i, prec); // In place addition
            tmp_i.free();
        }
    }
    return out;
}

/** @brief Deep copy
 *
 * New orbitals are constructed as deep copies of the input set.
 *
 */
OrbitalVector orbital::deep_copy(OrbitalVector &inp) {
    OrbitalVector out;
    for (int i = 0; i < inp.size(); i++) {
        Orbital out_i = inp[i].deepCopy();
        out.push_back(out_i);
    }
    return out;
}

/** @brief Parameter copy
 *
 * New orbitals are constructed as parameter copies of the input set.
 *
 */
OrbitalVector orbital::param_copy(const OrbitalVector &inp) {
    OrbitalVector out;
    for (int i = 0; i < inp.size(); i++) {
        Orbital out_i = inp[i].paramCopy();
        out.push_back(out_i);
    }
    return out;
}

/** @brief Adjoin two vectors
 *
 * The orbitals of the input vector are appended to
 * (*this) vector, the ownership is transferred. Leaves
 * the input vector empty.
 *
 */
OrbitalVector orbital::adjoin(OrbitalVector &inp_a, OrbitalVector &inp_b) {
    OrbitalVector out;
    for (int i = 0; i < inp_a.size(); i++) out.push_back(inp_a[i]);
    for (int i = 0; i < inp_b.size(); i++) out.push_back(inp_b[i]);
    inp_a.clear();
    inp_b.clear();
    return out;
}

/** @brief Disjoin vector in two parts
 *
 * All orbitals of a particular spin is collected in a new vector
 * and returned. These orbitals are removed from (*this) vector,
 * and the ownership is transferred.
 *
 */
OrbitalVector orbital::disjoin(OrbitalVector &inp, int spin) {
    OrbitalVector out;
    OrbitalVector tmp;
    for (int i = 0; i < inp.size(); i++) {
        Orbital &orb_i = inp[i];
        if (orb_i.spin() == spin) {
            out.push_back(orb_i);
        } else {
            tmp.push_back(orb_i);
        }
    }
    inp.clear();
    inp = tmp;
    return out;
}

/** @brief Write orbitals to disk
 *
 * @param Phi: orbitals to save
 * @param file: file name prefix
 * @param n_orbs: number of orbitals to save
 *
 * The given file name (e.g. "phi") will be appended with orbital number ("phi_0").
 * Produces separate files for meta data ("phi_0.meta"), real ("phi_0_re.tree") and
 * imaginary ("phi_0_im.tree") parts. Negative n_orbs means that all orbitals in the
 * vector are saved.
 */
void orbital::save_orbitals(OrbitalVector &Phi, const std::string &file, int n_orbs) {
    if (n_orbs < 0) n_orbs = Phi.size();
    if (n_orbs > Phi.size()) MSG_ERROR("Index out of bounds");
    for (int i = 0; i < n_orbs; i++) {
        if (not mpi::my_orb(Phi[i])) continue; //only save own orbitals
        std::stringstream orbname;
        orbname << file << "_" << i;
        Phi[i].saveOrbital(orbname.str());
    }
}

/** @brief Read orbitals from disk
 *
 * @param file: file name prefix
 * @param n_orbs: number of orbitals to read
 *
 * The given file name (e.g. "phi") will be appended with orbital number ("phi_0").
 * Reads separate files for meta data ("phi_0.meta"), real ("phi_0_re.tree") and
 * imaginary ("phi_0_im.tree") parts. Negative n_orbs means that all orbitals matching
 * the prefix name will be read.
 */
OrbitalVector orbital::load_orbitals(const std::string &file, int n_orbs) {
    OrbitalVector Phi;
    for (int i = 0; true; i++) {
        if (n_orbs > 0 and i >= n_orbs) break;
        Orbital phi_i;
        std::stringstream orbname;
        orbname << file << "_" << i;
        phi_i.loadOrbital(orbname.str());
        phi_i.setRankId(mpi::orb_rank);
        if (phi_i.hasReal() or phi_i.hasImag()) {
            phi_i.setRankId(i%mpi::orb_size);
            Phi.push_back(phi_i);
            if (i%mpi::orb_size != mpi::orb_rank) phi_i.clear(true);
        } else {
            break;
        }
    }
    //distribute errors
    DoubleVector errors = DoubleVector::Zero(Phi.size());
    for (int i = 0; i < Phi.size(); i++) {
        if (mpi::my_orb(Phi[i])) errors(i) = Phi[i].error();
    }
    mpi::allreduce_vector(errors, mpi::comm_orb);
    orbital::set_errors(Phi, errors);
    return Phi;
}

/** @brief Frees each orbital in the vector
 *
 * Leaves an empty vector. Orbitals are freed.
 *
 */
void orbital::free(OrbitalVector &vec) {
    for (int i = 0; i < vec.size(); i++) vec[i].free();
    vec.clear();
}

/** @brief Normalize all orbitals in the set */
void orbital::normalize(OrbitalVector &vec) {
    for (int i = 0; i < vec.size(); i++) {
        vec[i].normalize();
    }
}

/** @brief Gram-Schmidt orthogonalize orbitals within the set */
void orbital::orthogonalize(OrbitalVector &vec) {
    for (int i = 0; i < vec.size(); i++) {
        Orbital &orb_i = vec[i];
        for (int j = 0; j < i; j++) {
            Orbital &orb_j = vec[j];
            orb_i.orthogonalize(orb_j);
        }
    }
}

/** @brief Orthogonalize the out orbital against all orbitals in inp */
void orbital::orthogonalize(OrbitalVector &vec, OrbitalVector &inp) {
    for (int i = 0; i < vec.size(); i++) {
        vec[i].orthogonalize(inp);
    }
}

ComplexMatrix orbital::calc_overlap_matrix(OrbitalVector &braket) {
    ComplexMatrix S = ComplexMatrix::Zero(braket.size(), braket.size());

    // Get all ket orbitals belonging to this MPI
    OrbitalChunk my_ket = mpi::get_my_chunk(braket);

    // Receive ALL orbitals on the bra side, use only MY orbitals on the ket side
    // Computes the FULL columns associated with MY orbitals on the ket side
    OrbitalIterator iter(braket, true); // use symmetry
    while (iter.next()) {
        for (int i = 0; i < iter.get_size(); i++) {
            int idx_i = iter.idx(i);
            Orbital &bra_i = iter.orbital(i);
            for (int j = 0; j < my_ket.size(); j++) {
                int idx_j = std::get<0>(my_ket[j]);
                Orbital &ket_j = std::get<1>(my_ket[j]);
                if (mpi::my_orb(bra_i) and idx_j > idx_i) continue;
                if (mpi::my_unique_orb(ket_j) or mpi::orb_rank == 0) {
                    S(idx_i, idx_j) = orbital::dot(bra_i, ket_j);
                    S(idx_j, idx_i) = std::conj(S(idx_i, idx_j));
                }
            }
        }
    }
    // Assumes all MPIs have (only) computed their own columns of the matrix
    mpi::allreduce_matrix(S, mpi::comm_orb);
    return S;
}

/** @brief Compute the overlap matrix S_ij = <bra_i|ket_j>
 *
 * MPI: Each rank will compute the full columns related to their
 *      orbitals in the ket vector. The bra orbitals are communicated
 *      one rank at the time (all orbitals belonging to a given rank
 *      is communicated at the same time). This algorithm sets NO
 *      restrictions on the distributions of the bra or ket orbitals
 *      among the available ranks. After the columns have been computed,
 *      the full matrix is allreduced, e.i. all MPIs will have the full
 *      matrix at exit.
 *
 */
ComplexMatrix orbital::calc_overlap_matrix(OrbitalVector &bra, OrbitalVector &ket) {
    ComplexMatrix S = ComplexMatrix::Zero(bra.size(), ket.size());

    // Get all ket orbitals belonging to this MPI
    OrbitalChunk my_ket = mpi::get_my_chunk(ket);

    // Receive ALL orbitals on the bra side, use only MY orbitals on the ket side
    // Computes the FULL columns associated with MY orbitals on the ket side
    OrbitalIterator iter(bra);
    while (iter.next()) {
        for (int i = 0; i < iter.get_size(); i++) {
            int idx_i = iter.idx(i);
            Orbital &bra_i = iter.orbital(i);
            for (int j = 0; j < my_ket.size(); j++) {
                int idx_j = std::get<0>(my_ket[j]);
                Orbital &ket_j = std::get<1>(my_ket[j]);
                if (mpi::my_unique_orb(ket_j) or mpi::orb_rank == 0) {
                    S(idx_i, idx_j) = orbital::dot(bra_i, ket_j);
                }
            }
        }
    }
    // Assumes all MPIs have (only) computed their own columns of the matrix
    mpi::allreduce_matrix(S, mpi::comm_orb);
    return S;
}

/** @brief Compute Löwdin orthonormalization matrix
 *
 * @param Phi: orbitals to orthonomalize
 *
 * Computes the inverse square root of the orbital overlap matrix S^(-1/2)
 */
ComplexMatrix orbital::calc_lowdin_matrix(OrbitalVector &Phi) {
    Timer timer;
    printout(1, "Calculating Löwdin orthonormalization matrix      ");

    ComplexMatrix S_tilde = orbital::calc_overlap_matrix(Phi);
    ComplexMatrix S_m12 = math_utils::hermitian_matrix_pow(S_tilde, -1.0/2.0);

    timer.stop();
    println(1, timer.getWallTime());
    return S_m12;
}

/** @brief Minimize the spatial extension of orbitals, by orbital rotation
 *
 * @param Phi: orbitals to localize
 *
 * Minimizes \f$  \sum_{i=1,N}\langle i| {\bf R^2}  | i \rangle - \langle i| {\bf R}| i \rangle^2 \f$
 * which is equivalent to maximizing \f$  \sum_{i=1,N}\langle i| {\bf R}| i \rangle^2\f$
 *
 * The resulting transformation includes the orthonormalization of the orbitals.
 * Orbitals are rotated in place, and the transformation matrix is returned.
 */
ComplexMatrix orbital::localize(double prec, OrbitalVector &Phi) {
    Printer::printHeader(0, "Localizing orbitals");
    Timer timer;

    ComplexMatrix U;
    int n_it = 0;
    if (Phi.size() > 1) {
        Timer rmat;
        RRMaximizer rr(prec, Phi);
        rmat.stop();
        Printer::printDouble(0, "Computing position matrices", rmat.getWallTime(), 5);
        Timer rr_t;
        n_it = rr.maximize();
        rr_t.stop();
        Printer::printDouble(0, "Computing Foster-Boys matrix", rr_t.getWallTime(), 5);

        if (n_it > 0) {
            println(0, " Converged after iteration   " << std::setw(30) << n_it);
            U = rr.getTotalU().transpose().cast<ComplexDouble>();
        } else {
            println(0, " Foster-Boys localization did not converge!");
        }
    } else {
        println(0, " Cannot localize less than two orbitals");
    }

    if (n_it <= 0) {
        Timer orth_t;
        U = orbital::calc_lowdin_matrix(Phi);
        orth_t.stop();
        Printer::printDouble(0, "Computing Lowdin matrix", orth_t.getWallTime(), 5);
    }

    Timer rot_t;
    OrbitalVector Psi = orbital::linear_combination(U, Phi, prec);
    orbital::free(Phi);
    Phi = Psi;
    rot_t.stop();
    Printer::printDouble(0, "Rotating orbitals", rot_t.getWallTime(), 5);

    timer.stop();
    Printer::printFooter(0, timer, 2);
    return U;
}

/** @brief Perform the orbital rotation that diagonalizes the Fock matrix
 *
 * @param Phi: orbitals to rotate
 * @param F: Fock matrix to diagonalize
 *
 * The resulting transformation includes the orthonormalization of the orbitals.
 * Orbitals are rotated in place and Fock matrix is diagonalized in place.
 * The transformation matrix is returned.
 */
ComplexMatrix orbital::diagonalize(double prec, OrbitalVector &Phi, ComplexMatrix &F) {
    Printer::printHeader(0, "Digonalizing Fock matrix");
    Timer timer;

    Timer orth_t;
    ComplexMatrix S_m12 = orbital::calc_lowdin_matrix(Phi);
    F = S_m12.transpose()*F*S_m12;
    orth_t.stop();
    Printer::printDouble(0, "Computing Lowdin matrix", orth_t.getWallTime(), 5);

    Timer diag_t;
    ComplexMatrix U = ComplexMatrix::Zero(F.rows(), F.cols());
    int np = orbital::size_paired(Phi);
    int na = orbital::size_alpha(Phi);
    int nb = orbital::size_beta(Phi);
    if (np > 0) math_utils::diagonalize_block(F, U, 0,       np);
    if (na > 0) math_utils::diagonalize_block(F, U, np,      na);
    if (nb > 0) math_utils::diagonalize_block(F, U, np + na, nb);
    U = U * S_m12;
    diag_t.stop();
    Printer::printDouble(0, "Diagonalizing matrix", diag_t.getWallTime(), 5);

    Timer rot_t;
    OrbitalVector Psi = orbital::linear_combination(U, Phi, prec);
    orbital::free(Phi);
    Phi = Psi;
    rot_t.stop();
    Printer::printDouble(0, "Rotating orbitals", rot_t.getWallTime(), 5);

    timer.stop();
    Printer::printFooter(0, timer, 2);
    return U;
}

/** @brief Perform the Löwdin orthonormalization
 *
 * @param Phi: orbitals to orthonormalize
 *
 * Orthonormalizes the orbitals by multiplication of the Löwdin matrix S^(-1/2).
 * Orbitals are rotated in place, and the transformation matrix is returned.
 */
ComplexMatrix orbital::orthonormalize(double prec, OrbitalVector &Phi) {
    ComplexMatrix U = orbital::calc_lowdin_matrix(Phi);
    OrbitalVector Psi = orbital::linear_combination(U, Phi, prec);
    orbital::free(Phi);
    Phi = Psi;
    return U;
}

/** @brief Returns the number of occupied orbitals */
int orbital::size_occupied(const OrbitalVector &vec) {
    int nOcc = 0;
    for (int i = 0; i < vec.size(); i++) {
        if (vec[i].occ() > 0) nOcc++;
    }
    return nOcc;
}

/** @brief Returns the number of empty orbitals */
int orbital::size_empty(const OrbitalVector &vec) {
    int nEmpty = 0;
    for (int i = 0; i < vec.size(); i++) {
        if (vec[i].occ() == 0) nEmpty++;
    }
    return nEmpty;
}

/** @brief Returns the number of singly occupied orbitals */
int orbital::size_singly(const OrbitalVector &vec) {
    int nSingly = 0;
    for (int i = 0; i < vec.size(); i++) {
        if (vec[i].occ() == 1) nSingly++;
    }
    return nSingly;
}

/** @brief Returns the number of doubly occupied orbitals */
int orbital::size_doubly(const OrbitalVector &vec) {
    int nDoubly = 0;
    for (int i = 0; i < vec.size(); i++) {
        if (vec[i].occ() == 1) nDoubly++;
    }
    return nDoubly;
}

/** @brief Returns the number of paired orbitals */
int orbital::size_paired(const OrbitalVector &vec) {
    int nPaired = 0;
    for (int i = 0; i < vec.size(); i++) {
        if (vec[i].spin() == SPIN::Paired) nPaired++;
    }
    return nPaired;
}

/** @brief Returns the number of alpha orbitals */
int orbital::size_alpha(const OrbitalVector &vec) {
    int nAlpha = 0;
    for (int i = 0; i < vec.size(); i++) {
        if (vec[i].spin() == SPIN::Alpha) nAlpha++;
    }
    return nAlpha;
}

/** @brief Returns the number of beta orbitals */
int orbital::size_beta(const OrbitalVector &vec) {
    int nBeta = 0;
    for (int i = 0; i < vec.size(); i++) {
        if (vec[i].spin() == SPIN::Beta) nBeta++;
    }
    return nBeta;
}

/** @brief Returns the spin multiplicity of the vector */
int orbital::get_multiplicity(const OrbitalVector &vec) {
    int nAlpha = get_electron_number(vec, SPIN::Alpha);
    int nBeta = get_electron_number(vec, SPIN::Beta);
    int S = std::abs(nAlpha - nBeta);
    return S + 1;
}

/** @brief Returns the number of electrons with the given spin
 *
 * Paired spin (default input) returns the total number of electrons.
 *
 */
int orbital::get_electron_number(const OrbitalVector &vec, int spin) {
    int nElectrons = 0;
    for (int i = 0; i < vec.size(); i++) {
        const Orbital &orb = vec[i];
        if (spin == SPIN::Paired) {
            nElectrons += orb.occ();
        } else if (spin == SPIN::Alpha) {
            if (orb.spin() == SPIN::Paired or orb.spin() == SPIN::Alpha) {
                nElectrons += 1;
            }
        } else if (spin == SPIN::Beta) {
            if (orb.spin() == SPIN::Paired or orb.spin() == SPIN::Beta) {
                nElectrons += 1;
            }
        } else {
            MSG_ERROR("Invalid spin argument");
        }
    }
    return nElectrons;
}

/** @brief Returns a vector containing the orbital errors */
DoubleVector orbital::get_errors(const OrbitalVector &vec) {
    int nOrbs = vec.size();
    DoubleVector errors = DoubleVector::Zero(nOrbs);
    for (int i = 0; i < nOrbs; i++) {
        errors(i) = vec[i].error();
    }
    return errors;
}

/** @brief Assign errors to each orbital.
 *
 * Length of input vector must match the number of orbitals in the set.
 *
 */
void orbital::set_errors(OrbitalVector &vec, const DoubleVector &errors) {
    if (vec.size() != errors.size()) MSG_ERROR("Size mismatch");
    for (int i = 0; i < vec.size(); i++) {
        vec[i].setError(errors(i));
    }
}

/** @brief Returns a vector containing the orbital spins */
IntVector orbital::get_spins(const OrbitalVector &vec) {
    int nOrbs = vec.size();
    IntVector spins = IntVector::Zero(nOrbs);
    for (int i = 0; i < nOrbs; i++) {
        spins(i) = vec[i].spin();
    }
    return spins;
}

/** @brief Assigns spin to each orbital
 *
 * Length of input vector must match the number of orbitals in the set.
 *
 */
void orbital::set_spins(OrbitalVector &vec, const IntVector &spins) {
    if (vec.size() != spins.size()) MSG_ERROR("Size mismatch");
    for (int i = 0; i < vec.size(); i++) {
        vec[i].setSpin(spins(i));
    }
}

/** @brief Returns a vector containing the orbital occupancies */
IntVector orbital::get_occupancies(const OrbitalVector &vec) {
    int nOrbs = vec.size();
    IntVector occ = IntVector::Zero(nOrbs);
    for (int i = 0; i < nOrbs; i++) {
        occ(i) = vec[i].occ();
    }
    return occ;
}

/** @brief Assigns spin to each orbital
 *
 * Length of input vector must match the number of orbitals in the set.
 *
 */
void orbital::set_occupancies(OrbitalVector &vec, const IntVector &occ) {
    if (vec.size() != occ.size()) MSG_ERROR("Size mismatch");
    for (int i = 0; i < vec.size(); i++) {
        vec[i].setOcc(occ(i));
    }
}

/** @brief Returns a vector containing the orbital square norms */
DoubleVector orbital::get_squared_norms(const OrbitalVector &vec) {
    int nOrbs = vec.size();
    DoubleVector norms = DoubleVector::Zero(nOrbs);
    for (int i = 0; i < nOrbs; i++) {
        if (mpi::my_orb(vec[i])) norms(i) = vec[i].squaredNorm();
    }
    return norms;
}

/** @brief Returns a vector containing the orbital norms */
DoubleVector orbital::get_norms(const OrbitalVector &vec) {
    int nOrbs = vec.size();
    DoubleVector norms = DoubleVector::Zero(nOrbs);
    for (int i = 0; i < nOrbs; i++) {
        if (mpi::my_orb(vec[i])) norms(i) = vec[i].norm();
    }
    return norms;
}

void orbital::print(const OrbitalVector &vec) {
    Printer::setScientific();
    printout(0, "============================================================\n");
    printout(0, " OrbitalVector:");
    printout(0, std::setw(4) << vec.size()          << " orbitals  ");
    printout(0, std::setw(4) << size_occupied(vec)  << " occupied  ");
    printout(0, std::setw(4) << get_electron_number(vec) << " electrons\n");
    printout(0, "------------------------------------------------------------\n");
    printout(0, "   n  RankID           Norm          Spin Occ      Error    \n");
    printout(0, "------------------------------------------------------------\n");
    for (int i = 0; i < vec.size(); i++) {
        println(0, std::setw(4) << i << vec[i]);
    }
    printout(0, "============================================================\n\n\n");
}

} //namespace mrchem
