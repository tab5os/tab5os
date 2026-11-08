# Trusted update keys

Public halves of the ECDSA P-256 keys whose signed `.t5os` updates this
firmware installs (plan 9.1). Each `NAME.pem` is a `PUBLIC KEY` block;
`tools/ota/otakeys.py` builds them into the image and refuses private keys.

```sh
t5 keygen release.pem                                  # keep this one private, outside the tree
openssl pkey -in release.pem -pubout -out firmware/ota_keys/release.pem
t5 build && t5 ota-pack --sign release.pem             # -> tab5os-<version>.t5os
```

With no keys here, the OS installs only unsigned updates pushed in developer
mode (`t5 ota`, or an SD card file while developer mode is on). Changing the
keys takes effect from the next image you flash or install.
