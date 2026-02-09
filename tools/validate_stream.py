#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
PhantomFPGA Stream Validator

Validates a raw frame recording produced by the viewer's --record option.
Checks frame structure, CRCs, and sequence continuity against hardcoded
reference values (no external files needed).

Usage:
    python3 validate_stream.py <recording.bin> [-v]
"""

import argparse
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# Frame constants (must match phantomfpga_view.h)
# ---------------------------------------------------------------------------

FRAME_SIZE   = 5120
HEADER_SIZE  = 16
DATA_OFFSET  = HEADER_SIZE
DATA_SIZE    = 4995
CRC_OFFSET   = FRAME_SIZE - 4
FRAME_COUNT  = 250
MAGIC        = 0xF00DFACE

# ---------------------------------------------------------------------------
# Reference CRCs -- one per frame, indexed by sequence number (0-249)
#
# These are IEEE 802.3 CRC32 values computed over bytes 0..5115 of each
# canonical frame. Extracted from the QEMU frame data.
# ---------------------------------------------------------------------------

REFERENCE_CRCS = [
    0xFAF55D68, 0x329450CD, 0x54B5F102, 0x770E8751, 0x1CB75D99,  # 0-4
    0x2D544E59, 0x813687F5, 0x7B5DE0D4, 0x6D107629, 0x54E729B8,  # 5-9
    0x8E666561, 0x6B2BB8FD, 0x5E11C1FC, 0x0D804103, 0x4A094281,  # 10-14
    0x4FD806A0, 0xB7E79F19, 0xF7F34865, 0x6A573637, 0x97806F96,  # 15-19
    0xA99710E1, 0xB0BAC136, 0xA3974E62, 0x9431EC79, 0x51403D6C,  # 20-24
    0xF61BDCCC, 0x16BB8FCC, 0x088EB067, 0x6CC3CFF4, 0x85A53CB7,  # 25-29
    0x1C83DCC0, 0x4A40C186, 0xBA51DF67, 0xAF5EC6EC, 0x81C9247E,  # 30-34
    0xF3118B98, 0x01F1F17D, 0x4E5F8EBD, 0xB5A09D95, 0x9BE40F64,  # 35-39
    0xA106C780, 0x166047C1, 0x8A3EB7D5, 0xAA08E16B, 0x1252F035,  # 40-44
    0x5B348D59, 0x3649EB8F, 0x33F793BB, 0x133BA931, 0xBA5F9C59,  # 45-49
    0x91D06354, 0x2B04775F, 0x35FA9F0F, 0x1D3E87F0, 0x24CB85AB,  # 50-54
    0x7A76733A, 0x647E30CA, 0xFDCF7A7A, 0xD678E126, 0x6726478A,  # 55-59
    0x1E089C07, 0x51A6E3C7, 0x1045EA1A, 0x45AFB5BD, 0xFF00F339,  # 60-64
    0x114BF288, 0x8AE9D314, 0x7FAD295A, 0x79F7F662, 0x195F82D9,  # 65-69
    0xAB5B9DCC, 0x55593BE4, 0x47E9D136, 0x561EFE3E, 0x707AA1B8,  # 70-74
    0x43A4F133, 0x29D1EF67, 0x0CB6B720, 0xB40B0351, 0x65CE58EC,  # 75-79
    0xE504E919, 0x238BA0BF, 0xB087F791, 0xDF9EC494, 0x98001CCF,  # 80-84
    0xD7AE630F, 0x6A46AAF2, 0x27758CA7, 0xE34D0657, 0x6CE67F5E,  # 85-89
    0xE707584A, 0x0E06F458, 0xB5523FED, 0x7EC56368, 0x6623F6CA,  # 90-94
    0x7945B23D, 0x2D3B075B, 0xA4955221, 0xDF537E53, 0x32AF25FA,  # 95-99
    0x2A18A213, 0x0EC117CB, 0x93C10DF6, 0x37C4E533, 0x562A41A6,  # 100-104
    0x214290CF, 0xAB0F662D, 0x10AC217C, 0xA2BD7EA2, 0x5728315A,  # 105-109
    0x87DAB11A, 0x6978A3FC, 0x5FCA4045, 0xE488E63D, 0xD69FE85D,  # 110-114
    0x4271B193, 0x45DEFA88, 0xD092485A, 0x348C33A6, 0x41C6D64B,  # 115-119
    0x993A623D, 0xEBDBDCB4, 0xFDD19DD0, 0x8F8A2E46, 0xCE03B998,  # 120-124
    0xE0558B71, 0xD5231186, 0x5E131AE6, 0xF6E83504, 0xD6970776,  # 125-129
    0xB8067384, 0x380B5A60, 0x6AA5C31B, 0xF9713DB7, 0x2983BDF7,  # 130-134
    0xA0D0C4CB, 0xE934867B, 0x01CF155B, 0xA673BC31, 0x212AF013,  # 135-139
    0x087EF64D, 0xCB5AA449, 0x64B1B8D8, 0x11FDDE04, 0x6E14C3D2,  # 140-144
    0x4E013DEF, 0xD5D52870, 0x4F6E0C32, 0xDE05CF3A, 0x1ABF3EBF,  # 145-149
    0xDADF4169, 0xF71BDD36, 0x52D32599, 0xDE39B6D3, 0x644B6DE8,  # 150-154
    0x115B26DF, 0xC5EFBC16, 0x896AD46E, 0xBB300F1D, 0xF49E70DD,  # 155-159
    0x97848F1F, 0x6D4CB01C, 0xD613D300, 0x727BA293, 0xBA58799F,  # 160-164
    0xA564F678, 0x5C358A6D, 0x80D5E3F4, 0xFC4D30FC, 0xF01B4502,  # 165-169
    0x5545DFD0, 0x2B1F0FE0, 0xDE94EBB2, 0x10CCF62B, 0xE695C18A,  # 170-174
    0xC4887D18, 0x1CE16C6D, 0x7314FC17, 0x1A1007FA, 0xDBCC6A87,  # 175-179
    0x1D151815, 0x5B02DEFA, 0x60B10B07, 0x2F1F74C7, 0x3134FA28,  # 180-184
    0xF718D8CC, 0x0B218F0D, 0x4D7BD761, 0x98621C57, 0x8BC161CF,  # 185-189
    0x770D14C6, 0xD3BEE8BF, 0x536C6134, 0x8FC342E4, 0x722EB4E2,  # 190-194
    0xFD14D150, 0xF76ACF18, 0xA88F2780, 0x15B26BAF, 0x35E23AD4,  # 195-199
    0x9ED8734E, 0x219E538A, 0x191AF9E0, 0x7F44423F, 0xE82D46CE,  # 200-204
    0x95732580, 0xC772E7D8, 0x8B8ADF6D, 0x8F91BB29, 0x349ED2FD,  # 205-209
    0x4C9D328E, 0x272D5810, 0xD8A718C6, 0x072A4D90, 0x3E3A3D66,  # 210-214
    0xADEE437E, 0x5291B275, 0x759E6B98, 0xC628A995, 0xF60B9054,  # 215-219
    0x858EF3F9, 0xE9076752, 0x9A5A5248, 0x1F95EF57, 0x780C38DC,  # 220-224
    0x6CF8D366, 0x6060BB55, 0x3028EB36, 0xCF1EDE61, 0x142B4845,  # 225-229
    0x3193AD3C, 0x0CA5AEFB, 0x297F2339, 0xE2771A6B, 0xBBF96DEC,  # 230-234
    0xC98A480F, 0x9C615D53, 0xB6D8DB9E, 0x46B4C266, 0x6CD9C7B5,  # 235-239
    0x9CE67E68, 0xFCB12DAF, 0xDD8D5E44, 0x76532CA0, 0x06EAC7F5,  # 240-244
    0xEE9ADE8F, 0x935D8075, 0x5F7B8823, 0x684373BE, 0x6965FFEA,  # 245-249
]

assert len(REFERENCE_CRCS) == FRAME_COUNT, "Bug: expected 250 reference CRCs"

# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

def crc32(data):
    """IEEE 802.3 CRC32 -- matches the C++ and QEMU implementations."""
    return zlib.crc32(data) & 0xFFFFFFFF


def validate_frame(frame_data, index, verbose):
    """Validate a single frame. Returns a list of error strings (empty = OK)."""
    errors = []

    # Header: magic (LE u32), sequence (LE u32), reserved (u64)
    magic, sequence, reserved = struct.unpack_from("<IIQ", frame_data, 0)

    if magic != MAGIC:
        errors.append(f"bad magic: 0x{magic:08X} (expected 0x{MAGIC:08X})")

    if sequence >= FRAME_COUNT:
        errors.append(f"sequence {sequence} out of range (0-{FRAME_COUNT - 1})")

    if reserved != 0:
        errors.append(f"reserved field not zero: 0x{reserved:016X}")

    # CRC: recompute over bytes 0..5115, compare to stored CRC at offset 5116
    stored_crc = struct.unpack_from("<I", frame_data, CRC_OFFSET)[0]
    computed_crc = crc32(frame_data[:CRC_OFFSET])

    if computed_crc != stored_crc:
        errors.append(
            f"CRC mismatch: stored 0x{stored_crc:08X}, "
            f"computed 0x{computed_crc:08X}"
        )

    # Compare against reference CRC (only if sequence is valid)
    if sequence < FRAME_COUNT and computed_crc == stored_crc:
        ref_crc = REFERENCE_CRCS[sequence]
        if stored_crc != ref_crc:
            errors.append(
                f"CRC does not match reference for seq {sequence}: "
                f"got 0x{stored_crc:08X}, expected 0x{ref_crc:08X}"
            )

    if verbose and errors:
        for e in errors:
            print(f"  frame {index} (seq {sequence}): {e}")

    return errors


def validate_recording(path, verbose=False):
    """Validate a full recording file. Returns (total, valid, error_counts, seq_gaps)."""
    file_size = os.path.getsize(path)

    if file_size == 0:
        print(f"Error: {path} is empty")
        return 0, 0, {"empty_file": 1}, 0

    if file_size % FRAME_SIZE != 0:
        print(
            f"Warning: file size {file_size} is not a multiple of {FRAME_SIZE} "
            f"({file_size % FRAME_SIZE} trailing bytes -- truncated recording?)"
        )

    total_frames = file_size // FRAME_SIZE

    error_counts = {
        "magic": 0,
        "sequence_range": 0,
        "reserved": 0,
        "crc_mismatch": 0,
        "crc_vs_reference": 0,
    }

    valid = 0
    prev_seq = None
    seq_gaps = 0

    with open(path, "rb") as f:
        for i in range(total_frames):
            frame_data = f.read(FRAME_SIZE)
            if len(frame_data) < FRAME_SIZE:
                break

            errors = validate_frame(frame_data, i, verbose)

            if not errors:
                valid += 1

            for e in errors:
                if "bad magic" in e:
                    error_counts["magic"] += 1
                elif "out of range" in e:
                    error_counts["sequence_range"] += 1
                elif "reserved" in e:
                    error_counts["reserved"] += 1
                elif "CRC mismatch" in e:
                    error_counts["crc_mismatch"] += 1
                elif "reference" in e and "CRC" in e:
                    error_counts["crc_vs_reference"] += 1

            # Sequence continuity (informational, not a failure)
            seq = struct.unpack_from("<I", frame_data, 4)[0]
            if prev_seq is not None and seq < FRAME_COUNT:
                expected = (prev_seq + 1) % FRAME_COUNT
                if seq != expected:
                    gap = (seq - expected + FRAME_COUNT) % FRAME_COUNT
                    seq_gaps += gap
                    if verbose:
                        print(
                            f"  frame {i}: sequence gap -- "
                            f"expected {expected}, got {seq} "
                            f"({gap} frame(s) skipped)"
                        )
            if seq < FRAME_COUNT:
                prev_seq = seq

    return total_frames, valid, error_counts, seq_gaps


def main():
    parser = argparse.ArgumentParser(
        description="Validate a PhantomFPGA frame recording"
    )
    parser.add_argument(
        "recording",
        help="Path to raw recording file (5120-byte frames, back to back)"
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Print per-frame error details"
    )
    args = parser.parse_args()

    if not os.path.isfile(args.recording):
        print(f"Error: file not found: {args.recording}")
        sys.exit(1)

    print(f"Validating: {args.recording}")
    print(f"Frame size: {FRAME_SIZE} bytes")

    result = validate_recording(args.recording, args.verbose)
    total, valid, error_counts, seq_gaps = result

    # Summary
    print()
    print(f"Frames:       {total}")
    print(f"Valid:        {valid}")
    print(f"Errors:       {total - valid}")

    has_errors = False
    for name, count in error_counts.items():
        if count > 0:
            has_errors = True
            label = name.replace("_", " ")
            print(f"  {label}: {count}")

    if seq_gaps > 0:
        print(f"Sequence gaps: {seq_gaps} frame(s) skipped (not an error)")

    print()
    if has_errors:
        print("Result: FAIL")
        sys.exit(1)
    else:
        print("Result: PASS")
        sys.exit(0)


if __name__ == "__main__":
    main()
