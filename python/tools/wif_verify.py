"""
WIF (Wallet Import Format) Checksum Verifier
=============================================
Verifies the integrity of WIF-encoded private keys by checking
the checksum structure. For digital forensics and academic research.

Supports: WIF (0x80) and WIF-compressed (0x80 + 0x01)

Run: python wif_verify.py
"""

import hashlib
import sys

VERSIONS = {
    0x80: "WIF (uncompressed)",
    0x8001: "WIF-compressed",
}


def base58_decode(s):
    ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    n = 0
    for char in s:
        n = n * 58 + ALPHABET.index(char)

    byte_length = (n.bit_length() + 7) // 8
    if byte_length == 0:
        byte_length = 1
    bytes_result = n.to_bytes(byte_length, byteorder="big")

    leading_zeros = 0
    for char in s:
        if char == "1":
            leading_zeros += 1
        else:
            break

    return b'\x00' * leading_zeros + bytes_result.lstrip(b'\x00')


def sha256d(data):
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def verify_wif(wif_string):
    try:
        decoded = base58_decode(wif_string)
    except Exception as e:
        return False, f"Base58 decode error: {e}"

    if len(decoded) not in (37, 38):
        return False, f"Invalid decoded length: {len(decoded)} bytes (expected 37 or 38)"

    payload = decoded[:-4]
    checksum = decoded[-4:]

    expected_checksum = sha256d(payload)[:4]

    if checksum != expected_checksum:
        return False, {
            "checksum_valid": False,
            "provided": checksum.hex(),
            "expected": expected_checksum.hex(),
        }

    version = payload[0]
    key_data = payload[1:]

    if version == 0x80 and len(key_data) == 32:
        key_type = "WIF (uncompressed)"
        private_key = key_data.hex()
    elif version == 0x80 and len(key_data) == 33 and key_data[-1] in (0x01, 0x02, 0x03):
        key_type = "WIF-compressed"
        private_key = key_data[:32].hex()
        compression_flag = key_data[-1]
    else:
        key_type = f"Unknown version: 0x{version:02x}"
        private_key = key_data.hex()

    result = {
        "checksum_valid": True,
        "version": f"0x{version:02x}",
        "key_type": key_type,
        "private_key_hex": private_key,
        "payload_length": len(payload),
    }

    if len(key_data) == 33:
        result["compression_flag"] = f"0x{key_data[-1]:02x}"

    return True, result


def main():
    print("=" * 55)
    print("WIF Checksum Verifier - Digital Forensics Tool")
    print("=" * 55)
    print("Enter WIF keys to verify (one per line)")
    print("Type 'quit' to exit")
    print("=" * 55)

    while True:
        print()
        wif = input("WIF> ").strip()

        if wif.lower() in ("quit", "exit", "q"):
            print("Bye!")
            break

        if not wif:
            continue

        is_valid, details = verify_wif(wif)

        if isinstance(details, str):
            print(f"INVALID: {details}")
        else:
            if is_valid:
                print(f"Checksum:  VALID")
                print(f"Version:   {details['version']}")
                print(f"Key Type:  {details['key_type']}")
                print(f"Priv Key:  {details['private_key_hex']}")
                if "compression_flag" in details:
                    print(f"Compress:  {details['compression_flag']}")
            else:
                print(f"Checksum:  INVALID")
                print(f"Provided:  {details['provided']}")
                print(f"Expected:  {details['expected']}")


if __name__ == "__main__":
    main()
