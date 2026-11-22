#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# The certificates test_tls_wrap checks (made once; the keys aren't kept, so
# nothing here can sign anything). Needs OpenSSL 3.4 or later (-not_before).
#   bundle.pem    two authorities: "Other CA", then "Test CA"
#   ok.pem        localhost, from Test CA, valid 2020-2120
#   future.pem    localhost, from Test CA, valid from 2090
#   expired.pem   localhost, from Test CA, valid 2001-2002
#   stranger.pem  localhost, from "Stranger CA", which isn't in the bundle
# KEYS=dir keeps the keys there (for trying a real server by hand).
set -eu
cd "$(dirname "$0")"
k=${KEYS:-$(mktemp -d)}
ca() {
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$k/$1.key" \
        -subj "/CN=$2" -not_before 20200101000000Z -not_after 21200101000000Z \
        -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign,cRLSign \
        -out "$k/$1.pem" 2>/dev/null
}
leaf() {   # name issuer from to
    openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout "$k/$1.key" \
        -subj /CN=localhost -out "$k/$1.csr" 2>/dev/null
    printf 'subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\nkeyUsage=critical,digitalSignature\nbasicConstraints=CA:FALSE\n' >"$k/ext"
    openssl x509 -req -in "$k/$1.csr" -CA "$k/$2.pem" -CAkey "$k/$2.key" -set_serial "0x$(openssl rand -hex 8)" \
        -not_before "$3" -not_after "$4" -extfile "$k/ext" -out "$1.pem" 2>/dev/null
}
ca testca "Test CA"
ca otherca "Other CA"
ca strangerca "Stranger CA"
cat "$k/otherca.pem" "$k/testca.pem" >bundle.pem
leaf ok testca 20200101000000Z 21200101000000Z
leaf future testca 20900101000000Z 21200101000000Z
leaf expired testca 20010101000000Z 20020101000000Z
leaf stranger strangerca 20200101000000Z 21200101000000Z
[ -n "${KEYS:-}" ] || rm -rf "$k"
