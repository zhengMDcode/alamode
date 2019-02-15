/*
 integration.cpp

 Copyright (c) 2014 Terumasa Tadano

 This file is distributed under the terms of the MIT license.
 Please see the file 'LICENCE.txt' in the root directory 
 or http://opensource.org/licenses/mit-license.php for information.
*/

#include "mpi_common.h"
#include "integration.h"
#include "error.h"
#include "kpoint.h"
#include "mathfunctions.h"
#include "memory.h"
#include "system.h"
#include <iomanip>
#include <vector>
#include <algorithm>
#include <cmath>

using namespace PHON_NS;

Integration::Integration(PHON *phon): Pointers(phon)
{
    set_default_variables();
}

Integration::~Integration()
{
    deallocate_variables();
};

void Integration::set_default_variables()
{
    use_tetrahedron = true;
    ismear = -1;
    epsilon = 0.0;
    ntetra = 0;
    tetras = nullptr;
}

void Integration::deallocate_variables()
{
    if (tetras) {
        memory->deallocate(tetras);
    }
}


void Integration::setup_integration()
{
    MPI_Bcast(&ismear, 1, MPI_INT, 0, MPI_COMM_WORLD);

    const auto nk = kpoint->nk;
    const auto nkx = kpoint->nkx;
    const auto nky = kpoint->nky;
    const auto nkz = kpoint->nkz;

    if (mympi->my_rank == 0) {
        std::cout << std::endl;
        if (ismear == -1) {
            std::cout << " ISMEAR = -1: Tetrahedron method will be used." << std::endl;
        } else if (ismear == 0) {
            std::cout << " ISMEAR = 0: Lorentzian broadening with epsilon = "
                << std::fixed << std::setprecision(2) << epsilon << " (cm^-1)" << std::endl;
        } else if (ismear == 1) {
            std::cout << " ISMEAR = 1: Gaussian broadening with epsilon = "
                << std::fixed << std::setprecision(2) << epsilon << " (cm^-1)" << std::endl;
        } else {
            error->exit("setup_relaxation", "Invalid ksum_mode");
        }
        std::cout << std::endl;
    }

    if (ismear == -1) {
        ntetra = 6 * nk;
        memory->allocate(tetras, ntetra, 4);
        prepare_tetrahedron(nkx, nky, nkz);
    }

    epsilon *= time_ry / Hz_to_kayser;
    MPI_Bcast(&epsilon, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);
}


void Integration::prepare_tetrahedron(const int nk1,
                                      const int nk2,
                                      const int nk3) const
{
    const auto nk23 = nk2 * nk3;

    for (int i = 0; i < nk1; ++i) {
        for (int j = 0; j < nk2; ++j) {
            for (int k = 0; k < nk3; ++k) {

                const auto ii = (i + 1) % nk1;
                const auto jj = (j + 1) % nk2;
                const auto kk = (k + 1) % nk3;

                const auto n1 = k + j * nk3 + i * nk23;
                const auto n2 = k + j * nk3 + ii * nk23;
                const auto n3 = k + jj * nk3 + i * nk23;
                const auto n4 = k + jj * nk3 + ii * nk23;
                const auto n5 = kk + j * nk3 + i * nk23;
                const auto n6 = kk + j * nk3 + ii * nk23;
                const auto n7 = kk + jj * nk3 + i * nk23;
                const auto n8 = kk + jj * nk3 + ii * nk23;

                auto m = 6 * (k + j * nk3 + i * nk23);

                tetras[m][0] = n1;
                tetras[m][1] = n2;
                tetras[m][2] = n3;
                tetras[m][3] = n6;

                ++m;

                tetras[m][0] = n2;
                tetras[m][1] = n3;
                tetras[m][2] = n4;
                tetras[m][3] = n6;

                ++m;

                tetras[m][0] = n1;
                tetras[m][1] = n3;
                tetras[m][2] = n5;
                tetras[m][3] = n6;

                ++m;

                tetras[m][0] = n3;
                tetras[m][1] = n4;
                tetras[m][2] = n6;
                tetras[m][3] = n8;

                ++m;

                tetras[m][0] = n3;
                tetras[m][1] = n6;
                tetras[m][2] = n7;
                tetras[m][3] = n8;

                ++m;

                tetras[m][0] = n3;
                tetras[m][1] = n5;
                tetras[m][2] = n6;
                tetras[m][3] = n7;
            }
        }
    }
}

double Integration::do_tetrahedron(const double *energy,
                                   const double *f,
                                   const double e_ref)
{
    /*
    This function returns the summation of the given function f_{k}
    over the k-points which have the energy "e_ref" using the tetrahedron method.

    Ret(e_ref) = \int f(k) \delta(e_ref - energy(k))

    */

    auto ret = 0.0;
    double I1, I2, I3, I4;

    const auto frac3 = 1.0 / 3.0;
    double g;

    tetra_pair pair;

    auto vol_tot = 0.0;

    for (unsigned int i = 0; i < ntetra; ++i) {

        tetra_data.clear();

        for (unsigned int j = 0; j < 4; ++j) {
            const auto knum = tetras[i][j];
            pair.e = energy[knum];
            pair.f = f[knum];
            tetra_data.push_back(pair);
        }

        std::sort(tetra_data.begin(), tetra_data.end());

        const auto e1 = tetra_data[0].e;
        const auto e2 = tetra_data[1].e;
        const auto e3 = tetra_data[2].e;
        const auto e4 = tetra_data[3].e;

        const auto f1 = tetra_data[0].f;
        const auto f2 = tetra_data[1].f;
        const auto f3 = tetra_data[2].f;
        const auto f4 = tetra_data[3].f;

        const auto vol = volume(tetras[i]);
        vol_tot += vol;

        if (e3 <= e_ref && e_ref < e4) {
            g = 3.0 * std::pow(e4 - e_ref, 2) / ((e4 - e1) * (e4 - e2) * (e4 - e3));

            I1 = frac3 * fij(e1, e4, e_ref);
            I2 = frac3 * fij(e2, e4, e_ref);
            I3 = frac3 * fij(e3, e4, e_ref);
            I4 = frac3 * (fij(e4, e1, e_ref) + fij(e4, e2, e_ref) + fij(e4, e3, e_ref));

            ret += vol * g * (I1 * f1 + I2 * f2 + I3 * f3 + I4 * f4);

        } else if (e2 <= e_ref && e_ref < e3) {
            g = 3.0 * (e2 - e1 + 2.0 * (e_ref - e2) - (e4 + e3 - e2 - e1)
                * std::pow(e_ref - e2, 2) / ((e3 - e2) * (e4 - e2))) / ((e3 - e1) * (e4 - e1));

            I1 = frac3 * fij(e1, e4, e_ref) * g + fij(e1, e3, e_ref) * fij(e3, e1, e_ref) * fij(e2, e3, e_ref) / (e4 -
                e1);
            I2 = frac3 * fij(e2, e3, e_ref) * g + std::pow(fij(e2, e4, e_ref), 2) * fij(e3, e2, e_ref) / (e4 - e1);
            I3 = frac3 * fij(e3, e2, e_ref) * g + std::pow(fij(e3, e1, e_ref), 2) * fij(e2, e3, e_ref) / (e4 - e1);
            I4 = frac3 * fij(e4, e1, e_ref) * g + fij(e4, e2, e_ref) * fij(e2, e4, e_ref) * fij(e3, e2, e_ref) / (e4 -
                e1);

            ret += vol * (I1 * f1 + I2 * f2 + I3 * f3 + I4 * f4);

        } else if (e1 <= e_ref && e_ref < e2) {
            g = 3.0 * std::pow(e_ref - e1, 2) / ((e2 - e1) * (e3 - e1) * (e4 - e1));

            I1 = frac3 * (fij(e1, e2, e_ref) + fij(e1, e3, e_ref) + fij(e1, e4, e_ref));
            I2 = frac3 * fij(e2, e1, e_ref);
            I3 = frac3 * fij(e3, e1, e_ref);
            I4 = frac3 * fij(e4, e1, e_ref);

            ret += vol * g * (I1 * f1 + I2 * f2 + I3 * f3 + I4 * f4);

        }

    }

    return ret / vol_tot;
}

double Integration::dos_integration(double *energy,
                                    const double e_ref)
{
    auto dos_ret = 0.0;
    std::vector<double> e_tetra;

    auto vol_tot = 0.0;

    for (auto i = 0; i < ntetra; ++i) {
        e_tetra.clear();
        for (auto j = 0; j < 4; ++j) {
            e_tetra.push_back(energy[tetras[i][j]]);
        }
        std::sort(e_tetra.begin(), e_tetra.end());

        const auto e1 = e_tetra[0];
        const auto e2 = e_tetra[1];
        const auto e3 = e_tetra[2];
        const auto e4 = e_tetra[3];

        const auto vol = volume(tetras[i]);
        vol_tot += vol;

        if (e3 <= e_ref && e_ref < e4) {
            dos_ret += vol * (3.0 * std::pow(e4 - e_ref, 2) / ((e4 - e1) * (e4 - e2) * (e4 - e3)));
        } else if (e2 <= e_ref && e_ref < e3) {
            dos_ret += vol * 3.0 * (e2 - e1 + 2.0 * (e_ref - e2) - (e4 + e3 - e2 - e1)
                * std::pow(e_ref - e2, 2) / ((e3 - e2) * (e4 - e2))) / ((e3 - e1) * (e4 - e1));
        } else if (e1 <= e_ref && e_ref < e2) {
            dos_ret += vol * 3.0 * std::pow(e_ref - e1, 2) / ((e2 - e1) * (e3 - e1) * (e4 - e1));
        }
    }

    return dos_ret / vol_tot;
}


void Integration::calc_weight_tetrahedron(const int nk_irreducible,
                                          const int *map_to_irreducible_k,
                                          double *weight,
                                          const double *energy,
                                          const double e_ref)
{
    int i;

    double g;

    double vol_tot = 0.0;
    // std::vector<TetraWithKnum> tetra_data;
    // TetraWithKnum pair;
    double e_tmp[4];
    int sort_arg[4], kindex[4];

    for (i = 0; i < nk_irreducible; ++i) weight[i] = 0.0;

    for (i = 0; i < ntetra; ++i) {

        for (int j = 0; j < 4; ++j) {
            e_tmp[j] = energy[tetras[i][j]];
            kindex[j] = map_to_irreducible_k[tetras[i][j]];
        }

        insertion_sort(e_tmp, sort_arg, 4);
        const auto e1 = e_tmp[0];
        const auto e2 = e_tmp[1];
        const auto e3 = e_tmp[2];
        const auto e4 = e_tmp[3];

        const auto k1 = kindex[sort_arg[0]];
        const auto k2 = kindex[sort_arg[1]];
        const auto k3 = kindex[sort_arg[2]];
        const auto k4 = kindex[sort_arg[3]];

        const auto vol = volume(tetras[i]);
        vol_tot += vol;

        auto I1 = 0.0;
        auto I2 = 0.0;
        auto I3 = 0.0;
        auto I4 = 0.0;

        if (e3 <= e_ref && e_ref < e4) {
            // g = 3.0 * std::pow(e4 - e_ref, 2) / ((e4 - e1) * (e4 - e2) * (e4 - e3));
            g = std::pow(e4 - e_ref, 2) / ((e4 - e1) * (e4 - e2) * (e4 - e3));

            I1 = g * fij(e1, e4, e_ref);
            I2 = g * fij(e2, e4, e_ref);
            I3 = g * fij(e3, e4, e_ref);
            I4 = g * (fij(e4, e1, e_ref) + fij(e4, e2, e_ref) + fij(e4, e3, e_ref));

        } else if (e2 <= e_ref && e_ref < e3) {
            //  g = 3.0 * ((e2 - e1) + 2.0 * (e_ref - e2) - (e4 + e3 - e2 - e1)
            //      * std::pow((e_ref - e2), 2) / ((e3 - e2) * (e4 - e2))) / ((e3 - e1) * (e4 - e1));
            g = (e2 - e1 + 2.0 * (e_ref - e2) - (e4 + e3 - e2 - e1)
                * std::pow(e_ref - e2, 2) / ((e3 - e2) * (e4 - e2))) / ((e3 - e1) * (e4 - e1));

            I1 = g * fij(e1, e4, e_ref) + fij(e1, e3, e_ref) * fij(e3, e1, e_ref) * fij(e2, e3, e_ref) / (e4 - e1);
            I2 = g * fij(e2, e3, e_ref) + std::pow(fij(e2, e4, e_ref), 2) * fij(e3, e2, e_ref) / (e4 - e1);
            I3 = g * fij(e3, e2, e_ref) + std::pow(fij(e3, e1, e_ref), 2) * fij(e2, e3, e_ref) / (e4 - e1);
            I4 = g * fij(e4, e1, e_ref) + fij(e4, e2, e_ref) * fij(e2, e4, e_ref) * fij(e3, e2, e_ref) / (e4 - e1);

        } else if (e1 <= e_ref && e_ref < e2) {
            //  g = 3.0 * std::pow(e_ref - e1, 2) / ((e2 - e1) * (e3 - e1) * (e4 - e1));
            g = std::pow(e_ref - e1, 2) / ((e2 - e1) * (e3 - e1) * (e4 - e1));

            I1 = g * (fij(e1, e2, e_ref) + fij(e1, e3, e_ref) + fij(e1, e4, e_ref));
            I2 = g * fij(e2, e1, e_ref);
            I3 = g * fij(e3, e1, e_ref);
            I4 = g * fij(e4, e1, e_ref);

        }
        weight[k1] += vol * I1;
        weight[k2] += vol * I2;
        weight[k3] += vol * I3;
        weight[k4] += vol * I4;
    }

    for (i = 0; i < nk_irreducible; ++i) weight[i] /= vol_tot;
}

void Integration::calc_weight_smearing(const std::vector<std::vector<KpointList>> &kpinfo,
                                       double *weight,
                                       double *energy,
                                       const double e_ref,
                                       const int smearing_method) const
{
    unsigned int i;
    unsigned int knum;

    const auto epsilon = this->epsilon * Hz_to_kayser / time_ry;

    if (smearing_method == 0) {
        for (i = 0; i < kpinfo.size(); ++i) {
            knum = kpinfo[i][0].knum;
            weight[i] = kpoint->weight_k[i] * delta_lorentz(e_ref - energy[knum], epsilon);
        }
    } else if (smearing_method == 1) {
        for (i = 0; i < kpinfo.size(); ++i) {
            knum = kpinfo[i][0].knum;
            weight[i] = kpoint->weight_k[i] * delta_gauss(e_ref - energy[knum], epsilon);
        }
    }
}

void Integration::calc_weight_smearing(const int nk,
                                       const int nk_irreducible,
                                       const int *map_to_irreducible_k,
                                       double *weight,
                                       double *energy,
                                       const double e_ref,
                                       const int smearing_method) const
{
    int i;

    const auto epsilon = this->epsilon * Hz_to_kayser / time_ry;
    const auto invnk = 1.0 / static_cast<double>(nk);

    for (i = 0; i < nk_irreducible; ++i) weight[i] = 0.0;

    if (smearing_method == 0) {
        for (i = 0; i < nk; ++i) {
            weight[map_to_irreducible_k[i]] += delta_lorentz(e_ref - energy[i], epsilon);
        }
    } else if (smearing_method == 1) {
        for (i = 0; i < nk; ++i) {
            weight[map_to_irreducible_k[i]] += delta_gauss(e_ref - energy[i], epsilon);
        }
    }

    for (i = 0; i < nk_irreducible; ++i) weight[i] *= invnk;
}


double Integration::volume(const int *klist) const
{
    double k1[3], k2[3], k3[3];

    for (int i = 0; i < 3; ++i) {
        k1[i] = refold(kpoint->xk[klist[1]][i] - kpoint->xk[klist[0]][i]);
        k2[i] = refold(kpoint->xk[klist[2]][i] - kpoint->xk[klist[0]][i]);
        k3[i] = refold(kpoint->xk[klist[3]][i] - kpoint->xk[klist[0]][i]);
    }

    rotvec(k1, k1, system->rlavec_p, 'T');
    rotvec(k2, k2, system->rlavec_p, 'T');
    rotvec(k3, k3, system->rlavec_p, 'T');

    const auto vol = std::abs(k1[0] * (k2[1] * k3[2] - k2[2] * k3[1])
        + k1[1] * (k2[2] * k3[0] - k2[0] * k3[2])
        + k1[2] * (k2[0] * k3[1] - k2[1] * k3[0]));

    return vol;
}

double Integration::fij(const double ei,
                        const double ej,
                        const double e) const
{
    return (e - ej) / (ei - ej);
}

double Integration::refold(double x) const
{
    if (std::abs(x) > 0.5) {
        if (x < 0.0) {
            return x + 1.0;
        }
        return x - 1.0;
    }
    return x;
}

void Integration::insertion_sort(double *a,
                                 int *ind,
                                 int n) const
{
    int i;

    for (i = 0; i < n; ++i) ind[i] = i;

    for (i = 1; i < n; ++i) {
        double tmp = a[i];

        int j = i;
        while (j > 0 && tmp < a[j - 1]) {
            a[j] = a[j - 1];
            ind[j] = ind[j - 1];
            --j;
        }
        a[j] = tmp;
        ind[j] = i;
    }
}
