#!/usr/bin/env python3
"""Regenerate src/hal/ca_certs.h — the root CA bundle used for TLS validation (P4.27).

The output is the esp_crt_bundle wire format that
WiFiClientSecure::setCACertBundle() expects:

    uint16 big-endian   number of certificates
    per certificate, sorted by subject DER ascending:
        uint16 big-endian   subject DER length
        uint16 big-endian   SubjectPublicKeyInfo DER length
        subject DER bytes
        SubjectPublicKeyInfo DER bytes

Usage:
    python3 tools/gen_ca_bundle.py [path-to-pem-bundle]

Defaults to the build host's Mozilla store. Requires `cryptography`.
Re-run whenever the roots need refreshing, then rebuild and re-run the
hardware network smoke test (notes sync, weather, Telegram) — a bad bundle
fails closed and takes every HTTPS feature with it.
"""
import re
import struct
import sys
from cryptography import x509
from cryptography.hazmat.primitives import serialization

DEFAULT_PEM = "/etc/ssl/certs/ca-certificates.crt"
OUT = "src/hal/ca_certs.h"

HEADER = """// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
// P4.27 - root CA bundle for TLS certificate validation.
//
// GENERATED FILE, DO NOT HAND-EDIT. Regenerate with tools/gen_ca_bundle.py.
// Source: {src} ({n} roots), converted to the esp_crt_bundle wire format.
//
// Why the whole bundle rather than a handful of pinned roots: the hosts this
// firmware talks to do not share a root and they rotate. A pinned-root table
// gets one of them wrong and that feature fails closed in the field with no
// recovery short of a reflash. ~{kb} KB of flash buys immunity to that.
//
// Consumed via WiFiClientSecure::setCACertBundle() in hal/wireless.cpp.
// -----------------------------------------------------------------------------
#pragma once

#ifdef ARDUINO

#include <stdint.h>

// esp_crt_bundle-format blob: {n} roots, {nbytes} bytes.
static const uint8_t kRootCaBundle[] = {{
"""


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PEM
    pem = open(src, "rb").read()
    blocks = re.findall(
        rb"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", pem, re.S
    )
    certs = []
    for b in blocks:
        c = x509.load_pem_x509_certificate(b)
        certs.append(
            (
                c.subject.public_bytes(),
                c.public_key().public_bytes(
                    serialization.Encoding.DER,
                    serialization.PublicFormat.SubjectPublicKeyInfo,
                ),
            )
        )
    certs.sort(key=lambda t: t[0])

    blob = bytearray(struct.pack(">H", len(certs)))
    for subj, pub in certs:
        blob += struct.pack(">HH", len(subj), len(pub)) + subj + pub

    body = "\n".join(
        "    " + "".join("0x%02x," % b for b in blob[i : i + 16])
        for i in range(0, len(blob), 16)
    )
    with open(OUT, "w") as f:
        f.write(
            HEADER.format(src=src, n=len(certs), kb=len(blob) // 1024, nbytes=len(blob))
        )
        f.write(body)
        f.write("\n};\n\n#endif  // ARDUINO\n")
    print("wrote %s: %d roots, %d bytes" % (OUT, len(certs), len(blob)))


if __name__ == "__main__":
    main()
