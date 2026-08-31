"""
Probability Density Analysis in 2^256 Field
============================================
Pure mathematical analysis of probability distributions
within a closed numerical field of size 2^256.

For number theory and advanced cryptography research.
No external connections. No wallet checking.

Run: python probability_analysis.py
"""

import secrets
import math
from collections import Counter


FIELD_SIZE = 2**256


def generate_uniform_samples(n_samples):
    return [secrets.randbits(256) for _ in range(n_samples)]


def bit_distribution(samples):
    bit_counts = Counter()
    for sample in samples:
        for i in range(256):
            bit_counts[i] += (sample >> i) & 1
    return bit_counts


def entropy_analysis(samples):
    total_bits = len(samples) * 256
    bit_counts = bit_distribution(samples)

    entropy = 0.0
    for i in range(256):
        p = bit_counts[i] / len(samples)
        if p > 0:
            entropy -= p * math.log2(p)

    return {
        "samples": len(samples),
        "total_bits": total_bits,
        "measured_entropy": entropy,
        "theoretical_entropy": 256,
        "entropy_ratio": entropy / 256,
    }


def statistical_tests(samples):
    n = len(samples)

    bit_counts = bit_distribution(samples)

    deviations = []
    for i in range(256):
        expected = n / 2
        actual = bit_counts[i]
        deviation = abs(actual - expected) / expected
        deviations.append(deviation)

    avg_deviation = sum(deviations) / len(deviations)
    max_deviation = max(deviations)

    return {
        "average_deviation": avg_deviation,
        "max_deviation": max_deviation,
        "uniformity_score": 1.0 - avg_deviation,
    }


def birthday_bound_analysis():
    field_bits = 256
    collision_probability_50 = math.sqrt(2**field_bits * math.pi / 2)

    return {
        "field_size_bits": field_bits,
        "collision_50_percent": f"2^{math.log2(collision_probability_50):.1f}",
        "collision_50_percent_approx": f"{collision_probability_50:.2e}",
    }


def distribution_in_ranges(samples, num_ranges=10):
    range_size = FIELD_SIZE // num_ranges
    counts = [0] * num_ranges

    for sample in samples:
        idx = min(sample // range_size, num_ranges - 1)
        counts[idx] += 1

    expected = len(samples) / num_ranges
    ranges = []
    for i, count in enumerate(counts):
        low = i * range_size
        high = (i + 1) * range_size - 1
        ranges.append({
            "range": f"[{low}, {high}]",
            "count": count,
            "expected": expected,
            "deviation": abs(count - expected) / expected,
        })

    return ranges


def main():
    print("=" * 65)
    print("Probability Density Analysis in 2^256 Field")
    print("=" * 65)
    print("Pure mathematical analysis - No external connections")
    print("=" * 65)

    sample_sizes = [1000, 10000, 100000]

    for n in sample_sizes:
        print(f"\n{'='*65}")
        print(f"Sample Size: {n:,}")
        print(f"{'='*65}")

        samples = generate_uniform_samples(n)

        print("\n[1] Entropy Analysis:")
        entropy = entropy_analysis(samples)
        print(f"    Measured Entropy:     {entropy['measured_entropy']:.4f} bits")
        print(f"    Theoretical Entropy:  {entropy['theoretical_entropy']} bits")
        print(f"    Entropy Ratio:        {entropy['entropy_ratio']:.6f}")

        print("\n[2] Statistical Uniformity:")
        stats = statistical_tests(samples)
        print(f"    Average Deviation:    {stats['average_deviation']:.6f}")
        print(f"    Max Deviation:        {stats['max_deviation']:.6f}")
        print(f"    Uniformity Score:     {stats['uniformity_score']:.6f}")

        print("\n[3] Distribution Across Ranges:")
        ranges = distribution_in_ranges(samples)
        for r in ranges:
            print(f"    {r['range']}: count={r['count']}, dev={r['deviation']:.4f}")

    print(f"\n{'='*65}")
    print("[4] Birthday Bound Analysis:")
    birthday = birthday_bound_analysis()
    print(f"    Field Size:           2^{birthday['field_size_bits']}")
    print(f"    50% Collision Point:  {birthday['collision_50_percent']}")
    print(f"    Approximate:          {birthday['collision_50_percent_approx']}")
    print(f"{'='*65}")

    print("\n[5] Key Mathematical Properties:")
    print(f"    2^256 = {FIELD_SIZE}")
    print(f"    Bits:  256")
    print(f"    Hex:   64 characters")
    print(f"    Bytes: 32")
    print(f"\n    Probability of guessing 1 specific value:")
    print(f"    1/2^256 = 2^(-256) ~= 1.0 x 10^(-77)")
    print(f"\n    Atoms in observable universe ~= 10^80")
    print(f"    2^256 ~= 1.16 x 10^77")
    print(f"    Key space ~= number of atoms in universe")
    print(f"{'='*65}")


if __name__ == "__main__":
    main()
