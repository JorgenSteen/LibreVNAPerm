#include "permittivitymath.h"

#include "Tools/Eigen/Dense"

#include <cmath>

namespace PermittivityMath {

BilinearCoefficients solveBilinearCoefficients(const std::array<std::complex<double>, 3> &gammaStandards,
                                               const std::array<std::complex<double>, 3> &epsStandards)
{
    // eps_k = (A*gamma_k + B) / (C*gamma_k + 1)
    // => A*gamma_k + B - C*eps_k*gamma_k = eps_k
    Eigen::Matrix3cd M;
    Eigen::Vector3cd rhs;
    for (int k = 0; k < 3; k++) {
        const auto gk = gammaStandards[k];
        const auto ek = epsStandards[k];
        M(k, 0) = gk;
        M(k, 1) = 1.0;
        M(k, 2) = -ek * gk;
        rhs(k) = ek;
    }

    Eigen::JacobiSVD<Eigen::Matrix3cd> svd(M, Eigen::ComputeFullU | Eigen::ComputeFullV);
    const auto &sv = svd.singularValues();

    BilinearCoefficients ret;
    ret.conditionNumber = sv(2) > 0.0 ? sv(0) / sv(2) : INFINITY;
    Eigen::Vector3cd coeffs = svd.solve(rhs);
    ret.A = coeffs(0);
    ret.B = coeffs(1);
    ret.C = coeffs(2);
    return ret;
}

std::complex<double> applyBilinear(std::complex<double> gamma, const BilinearCoefficients &coeff)
{
    return (coeff.A * gamma + coeff.B) / (coeff.C * gamma + 1.0);
}

std::complex<double> airPermittivity()
{
    return std::complex<double>(1.0, 0.0);
}

std::complex<double> waterDebye(double freq, double tempC)
{
    // single-Debye model, Kaatze 1989 (J. Chem. Eng. Data 34, 371-374),
    // parameters linear in T over the ~20-25 C range
    const double eps_s = 80.08 - 0.394 * (tempC - 20.0);         // static permittivity
    const double eps_inf = 5.2;                                  // high-frequency limit
    const double tau = (9.36 - 0.088 * (tempC - 20.0)) * 1e-12;  // relaxation time, s

    const double w = 2.0 * M_PI * freq;
    // 1/(1 + j*w*tau) has a negative imaginary part -> eps' - j*eps''
    return eps_inf + (eps_s - eps_inf) / std::complex<double>(1.0, w * tau);
}

// Cole-Cole parameters for 0.9% NaCl saltwater at discrete temperatures.
// Columns: T[C], eps_inf, eps_s, tau[s], alpha, sigma[S/m]  (Peyman 2007-derived)
static constexpr double saltParams[][6] = {
    {10.0, 5.2, 81.64, 1.249e-11, 0.0117,  1.143},
    {15.0, 5.2, 79.79, 1.056e-11, 0.0112,  1.277},
    {20.0, 5.2, 77.96, 9.049e-12, 0.0107,  1.411},
    {22.5, 5.2, 77.06, 8.458e-12, 0.01045, 1.478},
    {25.0, 5.2, 76.16, 7.867e-12, 0.0102,  1.545},
    {30.0, 5.2, 74.38, 6.948e-12, 0.0097,  1.679},
    {35.0, 5.2, 72.63, 6.226e-12, 0.0092,  1.812},
    {40.0, 5.2, 70.88, 5.504e-12, 0.0087,  1.945},
};
static constexpr int saltParamRows = sizeof(saltParams) / sizeof(saltParams[0]);

// linear interpolation into a column of the table, clamped at the ends
// (same behaviour as numpy.interp)
static double interpSaltParam(double tempC, int column)
{
    if (tempC <= saltParams[0][0]) {
        return saltParams[0][column];
    }
    for (int i = 1; i < saltParamRows; i++) {
        if (tempC <= saltParams[i][0]) {
            const double t0 = saltParams[i - 1][0], t1 = saltParams[i][0];
            const double v0 = saltParams[i - 1][column], v1 = saltParams[i][column];
            return v0 + (v1 - v0) * (tempC - t0) / (t1 - t0);
        }
    }
    return saltParams[saltParamRows - 1][column];
}

std::complex<double> saltwaterColeCole(double freq, double tempC)
{
    const double eps_inf = interpSaltParam(tempC, 1);
    const double eps_s = interpSaltParam(tempC, 2);
    const double tau = interpSaltParam(tempC, 3);
    const double alpha = interpSaltParam(tempC, 4);
    const double sigma = interpSaltParam(tempC, 5);
    constexpr double eps0 = 8.854187817e-12;  // vacuum permittivity, F/m

    const double w = 2.0 * M_PI * freq;
    // eps* = eps_inf + (eps_s - eps_inf) / (1 + (j*w*tau)^(1-alpha)) - j*sigma/(w*eps0)
    const auto jwt = std::pow(std::complex<double>(0.0, w * tau), 1.0 - alpha);
    auto eps = eps_inf + (eps_s - eps_inf) / (1.0 + jwt);
    eps -= std::complex<double>(0.0, sigma / (w * eps0));
    return eps;
}

}
