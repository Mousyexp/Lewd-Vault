#!/usr/bin/env python3
"""The Vita's certificates (certs.bin, in the VPK): Mozilla's roots (cacert.pem, from curl.se/ca) and
Mozilla's intermediates (intermediates.pem, from CCADB's MozillaIntermediateCertsCSVReport), the
expired ones left out. The intermediates are there for sites that don't send their whole chain (a
desktop browser fills the gap; Firefox ships the same list): they only ever fill a chain, which must
still end at one of the roots.

Each one as the Vita needs it, nothing to parse there: "LVCA1", then per certificate its kind (0 a
root, 1 an intermediate), OpenSSL's subject hash (X509_NAME_hash: what the lookup is asked for), its
length and its DER. Needs the openssl command.

    python3 vita/tools/make-certs.py                         (from vita/cacert.pem, vita/intermediates.pem)
    python3 vita/tools/make-certs.py --fetch-intermediates   (intermediates.pem made afresh from CCADB)
"""
import base64
import csv
import io
import os
import re
import struct
import subprocess
import sys
import urllib.request

here = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
CCADB = 'https://ccadb.my.salesforce-sites.com/mozilla/MozillaIntermediateCertsCSVReport'


def pems(text):
    return re.findall(r'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', text, re.S)


def info(pem):
    """(subject hash, DER) or None when it has expired or can't be read"""
    if subprocess.run(['openssl', 'x509', '-noout', '-checkend', '0'], input=pem.encode(), capture_output=True).returncode != 0:
        return None
    r = subprocess.run(['openssl', 'x509', '-noout', '-subject_hash'], input=pem.encode(), capture_output=True)
    if r.returncode != 0:
        return None
    der = base64.b64decode(''.join(pem.split('\n')[1:-1]))
    return int(r.stdout.decode().strip(), 16), der


def main():
    if '--fetch-intermediates' in sys.argv:
        raw = urllib.request.urlopen(CCADB).read().decode('utf-8')
        rows = csv.DictReader(io.StringIO(raw))
        out = '\n'.join(r['PEM'].strip() for r in rows if r.get('PEM', '').startswith('-----BEGIN')) + '\n'
        with open(os.path.join(here, 'intermediates.pem'), 'w') as f:
            f.write('# Mozilla\'s intermediate certificates (CCADB, MozillaIntermediateCertsCSVReport): tools/make-certs.py\n' + out)
    blob = bytearray(b'LVCA1')
    counts = [0, 0, 0]
    for kind, name in ((0, 'cacert.pem'), (1, 'intermediates.pem')):
        with open(os.path.join(here, name)) as f:
            for pem in pems(f.read()):
                got = info(pem)
                if not got:
                    counts[2] += 1
                    continue
                h, der = got
                blob += struct.pack('<BII', kind, h, len(der)) + der
                counts[kind] += 1
    with open(os.path.join(here, 'certs.bin'), 'wb') as f:
        f.write(blob)
    print('certs.bin: %d roots, %d intermediates (%d expired or unreadable left out), %d bytes' % (counts[0], counts[1], counts[2], len(blob)))


main()
