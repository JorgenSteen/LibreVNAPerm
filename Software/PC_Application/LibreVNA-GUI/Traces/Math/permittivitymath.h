#ifndef PERMITTIVITYMATH_H
#define PERMITTIVITYMATH_H

#include <complex>
#include <array>

/*
 * Bilinear (3-standard) calibration method for permittivity extraction.
 *
 * At each frequency the open-ended coaxial probe's reflection-coefficient ->
 * permittivity map is a bilinear (Moebius) transformation:
 *
 *     eps* = (A*Gamma + B) / (C*Gamma + 1)
 *
 * Three complex coefficients A, B, C fully fix the map. They are recovered per
 * frequency from three reference standards, each a known (Gamma, eps*) pair
 * (open/air, water, saltwater). With A, B, C known, the same formula returns
 * eps* for any sample measurement.
 *
 * Reference: Marsland & Evans (1987); Sarolic (2022).
 *
 * CONVENTIONS (must be consistent between what you feed in and what you read out):
 * - Gamma / S11 is the LINEAR complex reflection coefficient (Re + j*Im),
 *   not dB, not magnitude/phase.
 * - Permittivity uses the physics/lossy convention with a NEGATIVE imaginary
 *   part: eps* = eps' - j*eps'' with eps'' >= 0 for a lossy material.
 *   The solve and apply steps are convention-agnostic: the recovered eps* comes
 *   back in the same convention as the standards fed in. Do not mix conventions.
 * - Everything is per frequency point.
 *
 * This file is intentionally free of Qt dependencies (only Eigen + std) so the
 * math can be unit-tested standalone against the Python reference
 * implementation (bilinear-share/bilinear.py).
 */

namespace PermittivityMath {

struct BilinearCoefficients {
    std::complex<double> A, B, C;
    // 2-norm condition number of the 3x3 system (same definition as
    // numpy.linalg.cond). Large values (>> 1e3) flag frequencies where the
    // three standards are poorly separated in the Gamma-plane and the
    // extracted eps* is untrustworthy.
    double conditionNumber;
};

// Solve for A, B, C at one frequency point from the three calibration
// standards. gammaStandards[k] is the measured S11 of standard k,
// epsStandards[k] its known eps*, in the same order.
// Row k of the system: [gamma_k, 1, -eps_k*gamma_k] * [A, B, C]^T = eps_k
BilinearCoefficients solveBilinearCoefficients(const std::array<std::complex<double>, 3> &gammaStandards,
                                               const std::array<std::complex<double>, 3> &epsStandards);

// Apply the calibrated map to a sample's measured S11: eps* = (A*g + B) / (C*g + 1)
std::complex<double> applyBilinear(std::complex<double> gamma, const BilinearCoefficients &coeff);

/*
 * Reference permittivity models of the three calibration standards.
 * All return eps* = eps' - j*eps'' (eps'' >= 0). Water and saltwater are
 * temperature-dependent -- recompute them at the actual liquid temperature of
 * each measurement session. freq in Hz, tempC in degrees Celsius.
 */

// open / air standard: 1 - 0j across the band (temperature-independent)
std::complex<double> airPermittivity();

// water: single-Debye model (Kaatze 1989), parameters linear in T around 20-25 C
std::complex<double> waterDebye(double freq, double tempC);

// saltwater (0.9% NaCl): Cole-Cole + ionic conductivity (Peyman 2007),
// parameters linearly interpolated over a 10-40 C table (clamped outside)
std::complex<double> saltwaterColeCole(double freq, double tempC);

// temperature range covered by the saltwater parameter table
constexpr double saltwaterTableMinTemp = 10.0;
constexpr double saltwaterTableMaxTemp = 40.0;

}

#endif // PERMITTIVITYMATH_H
