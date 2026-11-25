#!/bin/sh
# The builder's own keys (make keys). Private halves stay outside the
# repository, in KEYS (default ~/.config/bm168-mb-v04); public halves go to
# local/keys/, from where the build puts them into the image:
#
#   update-signing-key.pem  RSA-2048: signs app update bundles (make app-bundle).
#                           The image takes only bundles signed with it.
#   dev-ssh-key             Ed25519: the developer shell (docs/developer-access.md).
#                           Without local/keys/dev-authorized_keys an image has
#                           no developer access at all.
#
# Existing keys are never replaced. Back the private halves up: without the
# signing key a unit takes no more app updates until its image is replaced.
set -e
cd "$(dirname "$0")/.."
KEYS=${KEYS:-$HOME/.config/bm168-mb-v04}
mkdir -p "$KEYS" local/keys
chmod 700 "$KEYS"
if [ ! -f "$KEYS/update-signing-key.pem" ]; then
    openssl genrsa -out "$KEYS/update-signing-key.pem" 2048 2> /dev/null
    chmod 600 "$KEYS/update-signing-key.pem"
    echo "made $KEYS/update-signing-key.pem"
fi
openssl rsa -in "$KEYS/update-signing-key.pem" -pubout -out local/keys/update-key.pem 2> /dev/null
if [ ! -f "$KEYS/dev-ssh-key" ]; then
    ssh-keygen -q -t ed25519 -N '' -C bm168-dev -f "$KEYS/dev-ssh-key"
    echo "made $KEYS/dev-ssh-key"
fi
[ "${DEV_ACCESS:-1}" = 0 ] && rm -f local/keys/dev-authorized_keys || cp "$KEYS/dev-ssh-key.pub" local/keys/dev-authorized_keys
echo "public halves in local/keys/: $(ls local/keys | tr '\n' ' ')"
