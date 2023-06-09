#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <cmath>

#include <stdint.h>
#include <gmp.h>
#include <gmpxx.h>
 
#undef ENABLE_TIMER

#include "timer.h"
#include "matrix.h"
#include "gf/GaloisField.h"
#include "gf/GaloisFieldElement.h"
#include "gf/GaloisFieldPolynomial.h"
using namespace galois;

using namespace std;

// Minimal smoothness bound.
inline uint32_t MINIMAL_BOUND = 300;

// Sieving interval length.
const static uint32_t INTERVAL_LENGTH = 65536;


/*
 * Строит факторную базу B-гладких чисел
 * Возвращает вектор простых чисел p <= B
 */
vector<uint32_t> generateFactorBase(const mpz_class& N, uint32_t B) {
    vector<uint32_t> factorBase;
    /*
     * Решето Эратосфена, дополнительно проверяющее условие N/p = 1
     */
    vector<bool> sieve(B + 1, false);
    for (uint32_t p = 2; p <= B; ++p) {
        if (sieve[p])
            continue;

        // Добавляет p в фактотрную базу если N является квадратичным вычетом по модулю p
        if (mpz_legendre(N.get_mpz_t(), mpz_class(p).get_mpz_t()) == 1)
            factorBase.push_back(p);
        // Добавляет числа p*k <= B (k=1,2,..) для просеивания
        for (uint32_t i = p; i <= B; i += p)
            sieve[i] = true;
    }

    return factorBase;
}

/*
 * Возвращает b^e (mod m) используя схему "справа-налево".
 */
uint64_t modularPow(uint64_t b, uint64_t e, uint64_t m) {
    uint64_t result = 1;
    while (e > 0) {
        if (e & 1)
            result = (result * b) % m; 
        e >>= 1;
        b = (b * b) % m;
    }
    return result;
}

/*
 * Считает символ Лежандра
 *
 *   1 Если а является квадратичным вычетом по модулю p и a != 0 (mod p)
 *  -1 Если а не квадратичный вычет по модулю p
 *   0 Если a == 0 (mod p)
 */
int32_t legendreSymbol(uint32_t a, uint32_t p) {
    uint64_t result = modularPow(a, (p - 1) / 2, p);
    return result > 1 ? -1 : result;
}

/*
 * Возвращает решения для x^2 = n (mod p).
 * Использует алгоритм Тоннели-Шенкса. 
 */
pair<uint32_t, uint32_t> tonelliShanks(uint32_t n, uint32_t p) {
    if (p == 2)
        return make_pair(n, n);

    // Вычисление Q2^S = p - 1.
    uint64_t Q = p - 1;
    uint64_t S = 0;
    while (Q % 2 == 0) {
        Q /= 2;
        ++S;
    }

    // Вычисление первого такого числа z, что оно не будет являться квадратичным вычетом по модулю p
    uint64_t z = 2;
    while (legendreSymbol(z, p) != -1)
        ++z;

    uint64_t c = modularPow(z, Q, p);            // c = z^Q         (mod p)
    uint64_t R = modularPow(n, (Q + 1) / 2, p);  // R = n^((Q+1)/2) (mod p)
    uint64_t t = modularPow(n, Q, p);            // t = n^Q         (mod p)
    uint64_t M = S;

    // Invariant: R^2 = nt (mod p)
    while (t % p != 1) {
        // Find lowest 0 < i < M such that t^2^i = 1 (mod p).
        int32_t i = 1;
        while (modularPow(t, pow(2, i), p) != 1)
            ++i;

        // Set b = c^2^(M - i - 1)
        uint64_t b = modularPow(c, pow(2, M - i - 1), p);

        // Update c, R, t and M.
        R = R * b % p;      // R = Rb (mod p)
        t = t * b * b % p;  // t = tb^2
        c = b * b % p;      // c = b^2 (mod p)
        M = i;

        // Invariant: R^2 = nt (mod p)
    }

    return make_pair(R, p - R);
}

/*
 * A basic implementation of the Quadratic Sieve algorithm.
 *
 * No bells and whistles what so ever :)
 *
 * Takes an integer N as input and returns a factor of N.
 */
mpz_class quadraticSieve(const mpz_class& N) {

    // Some useful functions of N.
    const float logN = mpz_sizeinbase(N.get_mpz_t(), 2) * log(2); // Approx.
    const float loglogN = log(logN);
    const mpz_class sqrtN = sqrt(N);

    // Smoothness bound B.
    const uint32_t B = MINIMAL_BOUND + ceil(exp(0.55*sqrt(logN * loglogN)));

    /******************************
     *                            *
     * STAGE 1: Data Collection   *
     *                            *
     ******************************/

    /*
     * Step 1
     *
     * Generate factor base.
     */
    START();
    const vector<uint32_t> factorBase = generateFactorBase(N, B);
    STOP("Generated factor base");
    /*
     * Step 2
     *
     * Calculate start indices for each number in the factor base.
     */
    START();
    pair<vector<uint32_t>, vector<uint32_t> > startIndex(
        vector<uint32_t>(factorBase.size()), // Vector of first start index.
        vector<uint32_t>(factorBase.size())  // Vector of second start index.
    );
    for (uint32_t i = 0; i < factorBase.size(); ++i) {
        uint32_t p = factorBase[i];                   // Prime from our factor base.
        uint32_t N_mod_p = mpz_class(N % p).get_ui(); // N reduced modulo p.

        /*
         * We want the initial values of a such that (a + sqrt(N))^2 - N is
         * divisible by N. So we solve the congruence x^2 = N (mod p), which
         * will give us the desired values of a as a = x - sqrt(N).
         */
        pair<uint32_t, uint32_t> x = tonelliShanks(N_mod_p, p);

        /* 
         * The value we want is now a = x - sqrt(N) (mod p). This may be negative,
         * so we also add one p to get back on the positive side.
         */
        startIndex.first[i] = mpz_class((((x.first - sqrtN) % p) + p) % p).get_ui();
        startIndex.second[i] = mpz_class((((x.second - sqrtN) % p) + p) % p).get_ui();
    }
    STOP("Calculated indices");

    /************************************
     *                                  *
     * STAGE 2: Sieving Step            *
     *                                  *
     ***********************************/

    // In the comments below, Q = (a + sqrt(N))^2 - N , a = 1, 2, ...

    /*
     * Step 2.1
     *
     * Sieve through the log approximations in intervals of length INTERVAL_LENGTH
     * until we have at least factorBase.size() + 20 B-smooth numbers.
     */
    uint32_t intervalStart = 0;
    uint32_t intervalEnd = INTERVAL_LENGTH;

    vector<uint32_t> smooth;                      // B-smooth numbers.
    vector<vector<uint32_t> > smoothFactors; // Factorization of each B-smooth number.
    vector<float> logApprox(INTERVAL_LENGTH, 0);  // Approx. 2-logarithms of a^2 - N.

    // Rough log estimates instead of full approximations.
    float prevLogEstimate = 0;
    uint32_t nextLogEstimate = 1;

    while (smooth.size() < factorBase.size() + 20) {
        /*
         * Step 2.1.1
         *
         * Generate log approximations of Q = (a + sqrt(N))^2 - N in the current interval.
         */
        START();
        for (uint32_t i = 1, a = intervalStart + 1; i < INTERVAL_LENGTH; ++i, ++a) {
            if (nextLogEstimate <= a) {
                const mpz_class Q = (a + sqrtN) * (a + sqrtN) - N;
                prevLogEstimate = mpz_sizeinbase(Q.get_mpz_t(), 2);    // ~log_2(Q)
                nextLogEstimate = nextLogEstimate * 1.8 + 1;
            }
            logApprox[i] = prevLogEstimate;
        }
        STOP("Log approx");

        /*
         * Step 2.1.2
         *
         * Sieve for numbers in the sequence that factor completely over the factor base.
         */
        START();
        for (uint32_t i = 0; i < factorBase.size(); ++i) {
            const uint32_t p = factorBase[i];
            const float logp = log(factorBase[i]) / log(2);

            // Sieve first sequence.
            while (startIndex.first[i] < intervalEnd) {
                logApprox[startIndex.first[i] - intervalStart] -= logp;
                startIndex.first[i] += p;
            }

            if (p == 2)
                continue; // a^2 = N (mod 2) only has one root.

            // Sieve second sequence.
            while (startIndex.second[i] < intervalEnd) {
                logApprox[startIndex.second[i] - intervalStart] -= logp;
                startIndex.second[i] += p;
            }
        }
        STOP("Sieve");
        /*
         * Step 2.1.3
         *
         * Factor values of Q whose ~logarithms were reduced to ~zero during sieving.
         */
        START();
        const float threshold = log(factorBase.back()) / log(2);
        for (uint32_t i = 0, a = intervalStart; i < INTERVAL_LENGTH; ++i, ++a) {
            if (fabs(logApprox[i]) < threshold) {
                mpz_class Q = (a + sqrtN) * (a + sqrtN) - N;
                vector<uint32_t> factors;

                // For each factor p in the factor base.
                for (uint32_t j = 0; j < factorBase.size(); ++j) {
                    // Repeatedly divide Q by p until it's not possible anymore.
                    const uint32_t p = factorBase[j];
                    while (mpz_divisible_ui_p(Q.get_mpz_t(), p)) {
                        mpz_divexact_ui(Q.get_mpz_t(), Q.get_mpz_t(), p);
                        factors.push_back(j); // The j:th factor base number was a factor.
                    }
                }
                if (Q == 1) {
                    // Q really was B-smooth, so save its factors and the corresponding a.
                    smoothFactors.push_back(factors);
                    smooth.push_back(a);
                }
                if (smooth.size() >= factorBase.size() + 20)
                    break; // We have enough smooth numbers, so stop factoring.
            }
        }
        STOP("Factor");

        // Move on to next interval.
        intervalStart += INTERVAL_LENGTH;
        intervalEnd += INTERVAL_LENGTH;
    }
    

    /************************************
     *                                  *
     * STAGE 3: Linear Algebra Step     *
     *                                  *
     ***********************************/

    /*
     * Step 3.1
     *
     * Construct a binary matrix M with M_ij = the parity of the i:th prime factor
     * from the factor base in the factorization of the j:th B-smooth number.
     */
    Matrix M(factorBase.size(), smoothFactors.size() + 1);
    for (uint32_t i = 0; i < smoothFactors.size(); ++i) {
        for (uint32_t j = 0; j < smoothFactors[i].size(); ++j) {
            M(smoothFactors[i][j], i).flip();
        }
    }
    
    /*
     * Step 3.2
     *
     * Reduce the matrix to row echelon form and solve it repeatedly until a factor
     * is found.
     */
    M.reduce();
    mpz_class a;
    mpz_class b;
//    cout << M;


    do {
        vector<uint32_t> x = M.solve();

        a = 1;
        b = 1;

        /*
         * Calculate b = product(smooth[i] + sqrt(N)).
         *
         * Also calculate the the power of each prime in a's decomposition on the
         * factor base.
         */
        vector<uint32_t> decomp(factorBase.size(), 0);
        for (uint32_t i = 0; i < smoothFactors.size(); ++i) {
            if (x[i] == 1) {
                for(uint32_t p = 0; p < smoothFactors[i].size(); ++p)
                    ++decomp[smoothFactors[i][p]];
                b *= (smooth[i] + sqrtN);
            }
        }

        /*
         * Calculate a = sqrt(product(factorBase[p])).
         */
        for(uint32_t p = 0; p < factorBase.size(); ++p) {
            for(uint32_t i = 0; i < (decomp[p] / 2); ++i)
                a *= factorBase[p];
        }
        // a = +/- b (mod N) means we have a trivial factor :(
    } while (a % N == b % N || a % N == (- b) % N + N);


    /************************************
     *                                  *
     * STAGE 4: Success!                *
     *                                  *
     ***********************************/

    mpz_class factor;
    mpz_gcd(factor.get_mpz_t(), mpz_class(b - a).get_mpz_t(), N.get_mpz_t());

    return factor;
}