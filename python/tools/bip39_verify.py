"""
BIP39 Mnemonic Checksum Verifier
=================================
Real BIP39 checksum verification with multiprocessing support.

Run: python bip39_verify.py
"""

import hashlib
import multiprocessing as mp
import sys
import urllib.request

BIP39_URL = "https://raw.githubusercontent.com/bitcoin/bips/master/bip-0039/english.txt"
CACHE_FILE = "english.txt"


def download_wordlist():
    try:
        with open(CACHE_FILE, "r") as f:
            words = [line.strip() for line in f if line.strip()]
            if len(words) == 2048:
                return words
    except FileNotFoundError:
        pass

    print("Downloading BIP39 English wordlist...")
    urllib.request.urlretrieve(BIP39_URL, CACHE_FILE)

    with open(CACHE_FILE, "r") as f:
        return [line.strip() for line in f if line.strip()]


def mnemonic_to_entropy_bits(mnemonic, wordlist):
    words = mnemonic.split()

    if len(words) not in (12, 15, 18, 21, 24):
        return False, f"Invalid word count: {len(words)}"

    try:
        indexes = [wordlist.index(w) for w in words]
    except ValueError as e:
        return False, f"Unknown word: {e}"

    bits = "".join(f"{i:011b}" for i in indexes)
    total_bits = len(bits)

    cs_length = total_bits // 33
    entropy_length = total_bits - cs_length

    entropy_bits = bits[:entropy_length]
    checksum_bits = bits[entropy_length:]

    entropy = int(entropy_bits, 2).to_bytes(entropy_length // 8, byteorder="big")

    digest = hashlib.sha256(entropy).digest()
    calculated_checksum = "".join(f"{byte:08b}" for byte in digest)[:cs_length]

    is_valid = checksum_bits == calculated_checksum

    return is_valid, {
        "entropy_bits": entropy_length,
        "checksum_bits": cs_length,
        "provided": checksum_bits,
        "calculated": calculated_checksum,
    }


def worker(args):
    mnemonic, wordlist = args
    valid, details = mnemonic_to_entropy_bits(mnemonic, wordlist)
    return {"mnemonic": mnemonic, "valid": valid, "details": details}


def verify_batch(mnemonics, wordlist, processes=None):
    if processes is None:
        processes = mp.cpu_count()

    args = [(m, wordlist) for m in mnemonics]

    with mp.Pool(processes=processes) as pool:
        results = pool.map(worker, args)

    return results


def main():
    wordlist = download_wordlist()
    if len(wordlist) != 2048:
        print(f"Error: Wordlist has {len(wordlist)} words, expected 2048")
        sys.exit(1)

    print(f"BIP39 English wordlist loaded: {len(wordlist)} words")
    print("=" * 50)
    print("Enter mnemonics (one per line)")
    print("Type 'batch' to verify multiple mnemonics in parallel")
    print("Type 'quit' to exit")
    print("=" * 50)

    while True:
        print()
        user_input = input("Mnemonic> ").strip()

        if user_input.lower() in ("quit", "exit", "q"):
            print("Bye!")
            break

        if not user_input:
            continue

        if user_input.lower() == "batch":
            print("Enter mnemonics (empty line to start verification):")
            mnemonics = []
            while True:
                line = input("  > ").strip()
                if not line:
                    break
                mnemonics.append(line)

            if mnemonics:
                print(f"\nVerifying {len(mnemonics)} mnemonics...")
                results = verify_batch(mnemonics, wordlist)

                valid_count = sum(1 for r in results if r["valid"])
                print(f"Results: {valid_count} valid, {len(results) - valid_count} invalid\n")

                for r in results:
                    status = "VALID" if r["valid"] else "INVALID"
                    print(f"[{status}] {r['mnemonic']}")
            continue

        is_valid, details = mnemonic_to_entropy_bits(user_input, wordlist)

        if isinstance(details, str):
            print(f"Error: {details}")
        else:
            status = "VALID" if is_valid else "INVALID"
            print(f"Status:   {status}")
            print(f"Entropy:  {details['entropy_bits']} bits")
            print(f"Checksum: {details['checksum_bits']} bits")
            print(f"Provided: {details['provided']}")
            print(f"Expected: {details['calculated']}")


if __name__ == "__main__":
    main()
