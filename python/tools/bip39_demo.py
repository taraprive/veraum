"""
BIP39 Checksum Verification & Multiprocessing Demo
===================================================
Part 1: Verify BIP39 mnemonic checksums
Part 2: Multiprocessing experiment with dummy data

Run: python bip39_demo.py
"""

import hashlib
import multiprocessing as mp
from itertools import product


# ============================================================
# Part 1: BIP39 Checksum Verification
# ============================================================

MOCK_WORDLIST = [
    "alpha", "bravo", "charlie", "delta",
    "echo", "foxtrot", "golf", "hotel"
]


def mnemonic_to_entropy_bits(mnemonic, wordlist):
    words = mnemonic.split()

    if len(words) not in (12, 15, 18, 21, 24):
        return False, "Invalid word count"

    for w in words:
        if w not in wordlist:
            return False, f"Word '{w}' not in wordlist"

    indexes = [wordlist.index(w) for w in words]

    bits = "".join(f"{i:011b}" for i in indexes)
    total_bits = len(bits)

    entropy_length = total_bits - (total_bits // 33)
    cs_length = total_bits // 33

    entropy_bits = bits[:entropy_length]
    checksum_bits = bits[entropy_length:]

    entropy = int(entropy_bits, 2).to_bytes(entropy_length // 8, byteorder="big")

    digest = hashlib.sha256(entropy).digest()
    calculated_checksum = "".join(f"{byte:08b}" for byte in digest)[:cs_length]

    return checksum_bits == calculated_checksum, {
        "entropy_bits": entropy_length,
        "checksum_bits": cs_length,
        "match": checksum_bits == calculated_checksum
    }


def demo_checksum():
    print("=" * 50)
    print("Part 1: BIP39 Checksum Verification")
    print("=" * 50)

    test_mnemonics = [
        "alpha bravo charlie delta echo foxtrot golf hotel alpha bravo charlie delta",
        "alpha alpha alpha alpha alpha alpha alpha alpha alpha alpha alpha alpha",
    ]

    for mnemonic in test_mnemonics:
        print(f"\nMnemonic: {mnemonic}")
        valid, result = mnemonic_to_entropy_bits(mnemonic, MOCK_WORDLIST)
        print(f"Valid: {valid}")
        print(f"Details: {result}")

    print()


# ============================================================
# Part 2: Multiprocessing Experiment
# ============================================================

def mock_checksum_candidate(candidate):
    data = " ".join(candidate).encode()
    digest = hashlib.sha256(data).digest()
    score = int.from_bytes(digest[:4], "big")
    match = score % 100000 == 0
    return {"candidate": candidate, "score": score, "match": match}


def worker(candidate):
    return mock_checksum_candidate(candidate)


def demo_multiprocessing():
    print("=" * 50)
    print("Part 2: Multiprocessing Experiment")
    print("=" * 50)

    mock_candidates = list(product(MOCK_WORDLIST[:4], repeat=4))
    print(f"Total candidates: {len(mock_candidates)}")

    processes = mp.cpu_count()
    print(f"Using {processes} processes")

    with mp.Pool(processes=processes) as pool:
        results = pool.map(worker, mock_candidates)

    matches = [r for r in results if r["match"]]
    print(f"Matches found: {len(matches)}")

    print("\nFirst 10 matches:")
    for result in matches[:10]:
        print(result)

    print()


# ============================================================
# Main
# ============================================================

if __name__ == "__main__":
    demo_checksum()
    demo_multiprocessing()
    print("Done!")
